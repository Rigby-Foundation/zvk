/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (C) 2026 Rigby Foundation */
/* Independent SPIR-V -> Metal compute compiler. Fixed SSA storage and a
 * basic-block dispatcher preserve branch/parallel-phi semantics. No Mesa,
 * SPIRV-Cross, or CPU shader execution is used. Unsupported operations fail. */
#include "zvk_internal.h"
#define SPV_ENABLE_UTILITY_CODE
#define inline static inline
#include <spirv/unified1/spirv.h>
#undef inline
#include <spirv/unified1/GLSL.std.450.h>
#include <setjmp.h>
#include <stdarg.h>
#include "texel_msl.h"
#include "sampling_msl.h"
#include "spirv_float.h"

#define C_IDS 0x400000
#define C_MEMBERS 64
struct cn {
    const uint32_t *p;
    uint32_t type, literal, root, pointee, function;
    int known,slot,set,builtin,stride,block,dynamic,location,component,input_attachment,io_block;
    int member_builtin[C_MEMBERS],member_location[C_MEMBERS],member_component[C_MEMBERS];
    uint64_t flat_members,noperspective_members,centroid_members;
    int flat,noperspective,centroid;
    int64_t binding,spec;
    uint32_t static_offset;
    uint32_t offsets[C_MEMBERS], matrix_strides[C_MEMBERS];
    uint64_t offset_mask, row_major_mask;
    uint32_t matrix_stride, component_stride, row_major, image_handle;
    unsigned reachable, visiting, depth, active;
};
struct cc {
    int structured;
    struct cn *n;
    zvk_id_map map;
    const uint32_t *code;
    uint32_t *expanded;
    size_t words, size, capacity;
    uint32_t bound, entry, shared, current_function;
    char *text;
    zvk_shader *out;
    VkShaderStageFlagBits stage;
    int early_tests;
    unsigned metal_depth,flow_serial;
    void *flow_alloc[33];
    jmp_buf fail;
};
static void cfail(struct cc *c, const char *fmt, ...)
{
    char text[384];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    zvk_error("compute SPIR-V: %s", text);
    if (getenv("ZVK_DEBUG_SHADERS"))
        fprintf(stderr, "zvk: %s\n", zvkGetLastError());
    longjmp(c->fail, 1);
}
static unsigned cop(const uint32_t *p)
{
    return p[0] & 65535;
}
static unsigned cnw(const uint32_t *p)
{
    return p[0] >> 16;
}
static struct cn *lookup(struct cc *c,uint32_t id)
{
    uint32_t index=zvk_spirv_id(&c->map,id);
    if (index==UINT32_MAX)cfail(c,"undefined id %u",id);
    return c->n+index;
}
static struct cn *node(struct cc *c,uint32_t id)
{
    struct cn *n=lookup(c,id);
    if (!n->p)cfail(c,"id %u is not yet defined",id);
    return n;
}
static void emit(struct cc *c, const char *fmt, ...)
{
    va_list ap, aq;
    va_start(ap, fmt);
    va_copy(aq, ap);
    int n = vsnprintf(NULL, 0, fmt, aq);
    va_end(aq);
    if (n < 0 || (size_t)n > 1024 * 1024 - c->size)
        cfail(c, "generated shader exceeds 1 MiB");
    size_t need = c->size + (size_t)n + 1;
    if (need > c->capacity) {
        size_t cap = need * 2;
        char *s = realloc(c->text, cap);
        if (!s)
            cfail(c, "out of memory");
        c->text = s;
        c->capacity = cap;
    }
    vsnprintf(c->text + c->size, c->capacity - c->size, fmt, ap);
    va_end(ap);
    c->size += (size_t)n;
}
static uint32_t constant_word(struct cc *c, uint32_t id, unsigned lane, unsigned depth);
static uint32_t literal(struct cc *c, uint32_t id)
{
    struct cn *n = node(c, id);
    if (!n->known) {
        if (cop(n->p)!=SpvOpSpecConstantOp)
            cfail(c,"id %u is not a scalar constant",id);
        n->literal=constant_word(c,id,0,0);
        n->known=1;
    }
    return n->literal;
}
static unsigned components(struct cc *c, uint32_t id)
{
    const uint32_t *p = node(c, id)->p;
    if (cop(p) == SpvOpTypeVector) {
        if (p[3] < 2 || p[3] > 4)
            cfail(c, "vector size");
        const uint32_t *base = node(c, p[2])->p;
        if (cop(base) != SpvOpTypeBool && cop(base) != SpvOpTypeInt && cop(base) != SpvOpTypeFloat)
            cfail(c, "vector component type");
        if (cop(base) != SpvOpTypeBool && base[2] != 32)
            cfail(c, "scalar width");
        return p[3];
    }
    if (cop(p) == SpvOpTypeBool)
        return 1;
    if (cop(p) == SpvOpTypeInt || cop(p) == SpvOpTypeFloat) {
        if (p[2] != 32)
            cfail(c, "only 32-bit scalars are supported");
        return 1;
    }
    return 0;
}
/* Opaque handles carry a runtime image binding and sampler metadata index. */
static int opaque(struct cc *c, uint32_t type)
{
    const uint32_t *p=node(c,type)->p;
    if (cop(p)==SpvOpTypePointer) {
        p=node(c,p[3])->p;
    }
    if (cop(p)==SpvOpTypeArray) p=node(c,p[2])->p;
    return cop(p)==SpvOpTypeImage || cop(p)==SpvOpTypeSampledImage || cop(p)==SpvOpTypeSampler;
}
static uint32_t bytes(struct cc *c, uint32_t id, unsigned depth);
static uint32_t member_offset(struct cc *c, uint32_t type, unsigned member, unsigned depth)
{
    struct cn *n = node(c,type);
    if (depth>24 || cop(n->p)!=SpvOpTypeStruct || member>=cnw(n->p)-2 || member>=C_MEMBERS)
        cfail(c,"struct member layout");
    uint64_t offset=0;
    for (unsigned i=0;i<=member;i++) {
        if (n->offset_mask & (1ull<<i))
            offset=n->offsets[i];
        else if (n->block && !n->io_block)
            cfail(c,"buffer struct member offset missing");
        if (i==member) break;
        offset+=bytes(c,n->p[2+i],depth+1);
        if (offset>65536) cfail(c,"local struct too large");
    }
    return offset;
}
static uint32_t bytes(struct cc *c, uint32_t id, unsigned depth)
{
    if (depth > 24)
        cfail(c, "recursive type");
    struct cn *n = node(c, id);
    const uint32_t *p = n->p;
    unsigned k = components(c, id);
    if (k)
        return k * 4;
    if (cop(p) == SpvOpTypeMatrix) {
        if (p[3] < 2 || p[3] > 4 || cop(node(c, p[2])->p) != SpvOpTypeVector)
            cfail(c, "matrix dimensions");
        return p[3] * bytes(c, p[2], depth + 1);
    }
    if (cop(p) == SpvOpTypeArray) {
        uint64_t s =
            (uint64_t)(n->stride ? n->stride : bytes(c, p[2], depth + 1)) * literal(c, p[3]);
        if (s > 65536)
            cfail(c, "local aggregate too large");
        return s;
    }
    if (cop(p) == SpvOpTypeRuntimeArray)
        return 0;
    if (cop(p) == SpvOpTypeStruct) {
        uint32_t end = 0;
        for (unsigned i = 2; i < cnw(p); i++) {
            uint32_t offset = member_offset(c, id, i - 2, depth);
            const uint32_t *member = node(c, p[i])->p;
            uint32_t size = bytes(c, p[i], depth + 1);
            if (cop(member) == SpvOpTypeMatrix && n->matrix_strides[i - 2])
                size = n->matrix_strides[i - 2] *
                       ((n->row_major_mask >> (i - 2)) & 1 ? components(c, member[2]) : member[3]);
            uint64_t e = (uint64_t)offset + size;
            if (e > 65536)
                cfail(c, "aggregate too large");
            if (e > end)
                end = e;
        }
        return end;
    }
    cfail(c, "unsupported memory type %s", SpvOpToString((SpvOp)cop(p)));
    return 0;
}
static unsigned value_registers(struct cc *c,uint32_t type,unsigned depth);
static uint32_t value_part(struct cc *c,uint32_t type,const uint32_t *indices,unsigned count,unsigned *offset,unsigned *lane);
#include "compute_constants.h"
#include "compute_values.h"
static const char *builtin(struct cc *c, int value)
{
    switch (value) {
    case SpvBuiltInGlobalInvocationId:
        return "uint4(gid,0)";
    case SpvBuiltInLocalInvocationId:
        return "uint4(lid,0)";
    case SpvBuiltInWorkgroupId:
        return "uint4(wid,0)";
    case SpvBuiltInNumWorkgroups:
        return "uint4(ng,0)";
    case SpvBuiltInLocalInvocationIndex:
        return "uint4(linear)";
    case SpvBuiltInWorkgroupSize:
        return "uint4(bs,0)";
    default:
        cfail(c, "unsupported builtin %d", value);
        return "";
    }
}
static VkFormat image_format(struct cc *c, unsigned format)
{
    switch (format) {
    case SpvImageFormatUnknown:
        return VK_FORMAT_UNDEFINED;
    case SpvImageFormatRgba32f:
        return VK_FORMAT_R32G32B32A32_SFLOAT;
    case SpvImageFormatRgba16f:
        return VK_FORMAT_R16G16B16A16_SFLOAT;
    case SpvImageFormatR32f:
        return VK_FORMAT_R32_SFLOAT;
    case SpvImageFormatRgba8:
        return VK_FORMAT_R8G8B8A8_UNORM;
    case SpvImageFormatRgba8Snorm:
        return VK_FORMAT_R8G8B8A8_SNORM;
    case SpvImageFormatRg32f:
        return VK_FORMAT_R32G32_SFLOAT;
    case SpvImageFormatRg16f:
        return VK_FORMAT_R16G16_SFLOAT;
    case SpvImageFormatR11fG11fB10f:
        return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    case SpvImageFormatR16f:
        return VK_FORMAT_R16_SFLOAT;
    case SpvImageFormatRgba16:
        return VK_FORMAT_R16G16B16A16_UNORM;
    case SpvImageFormatRgb10A2:
        return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case SpvImageFormatRg16:
        return VK_FORMAT_R16G16_UNORM;
    case SpvImageFormatRg8:
        return VK_FORMAT_R8G8_UNORM;
    case SpvImageFormatR16:
        return VK_FORMAT_R16_UNORM;
    case SpvImageFormatR8:
        return VK_FORMAT_R8_UNORM;
    case SpvImageFormatRgba16Snorm:
        return VK_FORMAT_R16G16B16A16_SNORM;
    case SpvImageFormatRg16Snorm:
        return VK_FORMAT_R16G16_SNORM;
    case SpvImageFormatRg8Snorm:
        return VK_FORMAT_R8G8_SNORM;
    case SpvImageFormatR16Snorm:
        return VK_FORMAT_R16_SNORM;
    case SpvImageFormatR8Snorm:
        return VK_FORMAT_R8_SNORM;
    case SpvImageFormatRgba32i:
        return VK_FORMAT_R32G32B32A32_SINT;
    case SpvImageFormatRgba16i:
        return VK_FORMAT_R16G16B16A16_SINT;
    case SpvImageFormatRgba8i:
        return VK_FORMAT_R8G8B8A8_SINT;
    case SpvImageFormatR32i:
        return VK_FORMAT_R32_SINT;
    case SpvImageFormatRg32i:
        return VK_FORMAT_R32G32_SINT;
    case SpvImageFormatRg16i:
        return VK_FORMAT_R16G16_SINT;
    case SpvImageFormatRg8i:
        return VK_FORMAT_R8G8_SINT;
    case SpvImageFormatR16i:
        return VK_FORMAT_R16_SINT;
    case SpvImageFormatR8i:
        return VK_FORMAT_R8_SINT;
    case SpvImageFormatRgba32ui:
        return VK_FORMAT_R32G32B32A32_UINT;
    case SpvImageFormatRgba16ui:
        return VK_FORMAT_R16G16B16A16_UINT;
    case SpvImageFormatRgba8ui:
        return VK_FORMAT_R8G8B8A8_UINT;
    case SpvImageFormatR32ui:
        return VK_FORMAT_R32_UINT;
    case SpvImageFormatRg32ui:
        return VK_FORMAT_R32G32_UINT;
    case SpvImageFormatRg16ui:
        return VK_FORMAT_R16G16_UINT;
    case SpvImageFormatRg8ui:
        return VK_FORMAT_R8G8_UINT;
    case SpvImageFormatR16ui:
        return VK_FORMAT_R16_UINT;
    case SpvImageFormatR8ui:
        return VK_FORMAT_R8_UINT;
    case SpvImageFormatRgb10a2ui:
        return VK_FORMAT_A2B10G10R10_UINT_PACK32;
    default:
        cfail(c, "unsupported image format %u", format);
        return VK_FORMAT_UNDEFINED;
    }
}
static unsigned call_graph(struct cc *c,uint32_t id,unsigned nesting);
static void active_variables(struct cc *c)
{
    uint32_t function=0;
    for (size_t i=5;i<c->words;i+=cnw(c->code+i)) {
        const uint32_t *p=c->code+i;unsigned op=cop(p);
        if (op==SpvOpFunction)function=p[2];
        if (op==SpvOpFunctionEnd)function=0;
        if (!function || !node(c,function)->reachable)continue;
        unsigned first=0,last=0;
        if (op==SpvOpLoad || op==SpvOpAccessChain || op==SpvOpInBoundsAccessChain ||
            op==SpvOpCopyObject || op==SpvOpArrayLength || op==SpvOpImageTexelPointer)
            first=3,last=4;
        else if (op==SpvOpStore || op==SpvOpAtomicStore)first=1,last=2;
        else if (op==SpvOpCopyMemory || op==SpvOpCopyMemorySized)first=1,last=3;
        else if (op==SpvOpFunctionCall)first=4,last=cnw(p);
        else if (op>=SpvOpAtomicLoad && op<=SpvOpAtomicXor)first=3,last=4;
        for (unsigned j=first;j<last;j++) {
            struct cn *n=node(c,p[j]);
            if (cop(n->p)==SpvOpVariable && !n->function)n->active=1;
        }
    }
}
static void parse(struct cc *c, const char *entry, const VkSpecializationInfo *spec)
{
    unsigned function = 0;
    for (size_t i = 5; i < c->words; i += cnw(c->code + i)) {
        const uint32_t *p = c->code + i;
        bool rt = false, rr = false;
        SpvHasResultAndType((SpvOp)cop(p), &rr, &rt);
        if (rr) {
            uint32_t id = p[rt ? 2 : 1];
            if (!id || id >= c->bound || (rt && (!p[1] || p[1] >= c->bound)))
                cfail(c, "result ID or type out of range");
            struct cn *n = lookup(c,id);
            if (n->p)
                cfail(c, "duplicate id");
            n->p = p;
            n->type = rt ? p[1] : 0;
            n->function = function;
            n->slot = -1;
            n->binding = n->set = n->builtin = n->spec = n->location = n->input_attachment = -1;
            for (unsigned member=0;member<C_MEMBERS;member++)
                n->member_builtin[member]=n->member_location[member]=-1;
        }
        if (cop(p) == SpvOpFunction) {
            if (function)
                cfail(c, "nested function");
            function = p[2];
        }
        if (cop(p) == SpvOpFunctionEnd)
            function = 0;
        if (cop(p) == SpvOpEntryPoint) {
            if (!memchr(p + 3, 0, (cnw(p) - 3) * 4))
                cfail(c, "unterminated entry name");
            unsigned model=c->stage==VK_SHADER_STAGE_VERTEX_BIT?SpvExecutionModelVertex:
                c->stage==VK_SHADER_STAGE_FRAGMENT_BIT?SpvExecutionModelFragment:SpvExecutionModelGLCompute;
            if (p[1] == model && !strcmp((const char *)(p + 3), entry))
                c->entry = p[2];
        }
        if (cop(p)==SpvOpQuantizeToF16)c->out->image_metadata=1;
        if (cop(p) == SpvOpCapability && p[1] != SpvCapabilityShader &&
            p[1] != SpvCapabilityMatrix && p[1] != SpvCapabilityImageQuery &&
            p[1] != SpvCapabilityImageMSArray && p[1] != SpvCapabilityImageBuffer &&
            p[1] != SpvCapabilitySampledBuffer && p[1] != SpvCapabilityImage1D &&
            p[1] != SpvCapabilitySampled1D && p[1] != SpvCapabilityStorageImageExtendedFormats &&
            !(c->stage==VK_SHADER_STAGE_FRAGMENT_BIT && (p[1]==SpvCapabilityDerivativeControl || p[1]==SpvCapabilityInputAttachment)))
            cfail(c, "unsupported capability %u", p[1]);
        if (cop(p) == SpvOpCapability && p[1] == SpvCapabilityStorageImageExtendedFormats)
            c->out->extended_storage_formats = 1;
        if (cop(p) == SpvOpMemoryModel &&
            (p[1] != SpvAddressingModelLogical || p[2] != SpvMemoryModelGLSL450))
            cfail(c, "memory model");
    }
    if (!c->entry || function)
        cfail(c, "compute entry point missing or unterminated function");
    for (uint32_t dense=0;dense<c->map.count;dense++) {
        uint32_t id=c->map.ids[dense];
        struct cn *n = lookup(c,id);
        if (!n->p)
            continue;
        const uint32_t *p = n->p;
        if (cop(p) == SpvOpConstant || cop(p) == SpvOpSpecConstant) {
            if (cnw(p) != 4 || components(c, n->type) != 1)
                cfail(c, "scalar constant");
            n->literal = p[3];
            n->known = 1;
        }
        if (cop(p) == SpvOpConstantTrue || cop(p) == SpvOpSpecConstantTrue) {
            n->known = 1;
            n->literal = UINT32_MAX;
        }
        if (cop(p) == SpvOpConstantFalse || cop(p) == SpvOpSpecConstantFalse ||
            cop(p) == SpvOpConstantNull) {
            n->known = 1;
            n->literal = 0;
        }

    }
    for (size_t i = 5; i < c->words; i += cnw(c->code + i)) {
        const uint32_t *p = c->code + i;
        if (cop(p) == SpvOpDecorate) {
            struct cn *n = node(c, p[1]);
            if ((p[2] == SpvDecorationBinding || p[2] == SpvDecorationDescriptorSet ||
                 p[2] == SpvDecorationBuiltIn || p[2] == SpvDecorationSpecId ||
                 p[2] == SpvDecorationArrayStride || p[2]==SpvDecorationLocation || p[2]==SpvDecorationInputAttachmentIndex) &&
                cnw(p) < 4)
                cfail(c, "decoration operand");
            switch (p[2]) {
            case SpvDecorationComponent:
                if (cnw(p)<4)cfail(c,"component decoration operand");
                if(p[3]>3)cfail(c,"graphics interface component range");
                n->component=p[3];
                break;
            case SpvDecorationCentroid:
                n->centroid=1;
                break;
            case SpvDecorationSample:
                if (c->stage!=VK_SHADER_STAGE_COMPUTE_BIT)
                    cfail(c,"Metal graphics sample interpolation is not implemented");
                break;
            case SpvDecorationInputAttachmentIndex:
                n->input_attachment=p[3];
                break;
            case SpvDecorationLocation:
                n->location=p[3];
                break;
            case SpvDecorationFlat:
                n->flat=1;
                break;
            case SpvDecorationNoPerspective:
                n->noperspective=1;
                break;
            case SpvDecorationBinding:
                n->binding = p[3];
                break;
            case SpvDecorationDescriptorSet:
                n->set = p[3];
                break;
            case SpvDecorationBuiltIn:
                n->builtin = p[3];
                break;
            case SpvDecorationSpecId:
                n->spec = p[3];
                break;
            case SpvDecorationArrayStride:
                n->stride = p[3];
                break;
            case SpvDecorationBlock:
                n->block = 2;
                break;
            case SpvDecorationBufferBlock:
                n->block = 1;
                break;
            default:
                break;
            }
        }
        if (cop(p)==SpvOpMemberDecorate && c->stage!=VK_SHADER_STAGE_COMPUTE_BIT) {
            struct cn *n=node(c,p[1]);
            if (p[2]>=C_MEMBERS)cfail(c,"interface member limit");
            if ((p[3]==SpvDecorationBuiltIn || p[3]==SpvDecorationLocation || p[3]==SpvDecorationComponent) && cnw(p)<5)
                cfail(c,"interface member decoration operand");
            if (p[3]==SpvDecorationBuiltIn)n->member_builtin[p[2]]=p[4];
            if (p[3]==SpvDecorationLocation)n->member_location[p[2]]=p[4];
            if (p[3]==SpvDecorationComponent) {
                if(p[4]>3)cfail(c,"graphics interface member component range");
                n->member_component[p[2]]=p[4];
            }
            if (p[3]==SpvDecorationFlat)n->flat_members|=1ull<<p[2];
            if (p[3]==SpvDecorationCentroid)n->centroid_members|=1ull<<p[2];
            if (p[3]==SpvDecorationNoPerspective)n->noperspective_members|=1ull<<p[2];
        }
        if (cop(p) == SpvOpMemberDecorate && p[3] == SpvDecorationOffset) {
            if (cnw(p) < 5)
                cfail(c, "member offset operand");
            struct cn *n = node(c, p[1]);
            if (p[2] >= C_MEMBERS || (p[4] & 3))
                cfail(c, "member offset");
            n->offsets[p[2]] = p[4];
            n->offset_mask |= 1ull << p[2];
        }
        if (cop(p) == SpvOpMemberDecorate &&
            (p[3] == SpvDecorationMatrixStride || p[3] == SpvDecorationRowMajor)) {
            struct cn *n = node(c, p[1]);
            if (p[2] >= C_MEMBERS)
                cfail(c, "matrix member index");
            if (p[3] == SpvDecorationRowMajor)
                n->row_major_mask |= 1ull << p[2];
            else {
                if (cnw(p) < 5 || !p[4] || (p[4] & 3) || p[4] > 65536)
                    cfail(c, "matrix stride");
                n->matrix_strides[p[2]] = p[4];
            }
        }
        if (cop(p) == SpvOpExecutionMode && p[1] == c->entry) {
            if (c->stage==VK_SHADER_STAGE_COMPUTE_BIT) {
                if (p[2] != SpvExecutionModeLocalSize || cnw(p) != 6)
                    cfail(c, "execution mode");
                for (unsigned k = 0; k < 3; k++)
                    c->out->local_size[k] = p[3 + k];
            } else if (p[2]==SpvExecutionModeEarlyFragmentTests)
                c->early_tests=1;
            else if (p[2]!=SpvExecutionModeOriginUpperLeft && p[2]!=SpvExecutionModeDepthReplacing &&
                     p[2]!=SpvExecutionModeDepthGreater && p[2]!=SpvExecutionModeDepthLess &&
                     p[2]!=SpvExecutionModeDepthUnchanged)
                cfail(c,"unsupported graphics execution mode %u",p[2]);
        }
    }
    if (spec)
        for (uint32_t j=0;j<spec->mapEntryCount;j++) {
            const VkSpecializationMapEntry *m=spec->pMapEntries+j;
            if (m->size!=4 || !zvk_range(spec->dataSize,m->offset,m->size))
                cfail(c,"specialization range");
            for (uint32_t dense=0;dense<c->map.count;dense++) {
                struct cn *n=c->n+dense;
                if (n->p && n->spec==(int64_t)m->constantID) {
                    if (!n->known)cfail(c,"non-scalar specialization");
                    memcpy(&n->literal,(const char *)spec->pData+m->offset,4);
                    if (cop(node(c,n->type)->p)==SpvOpTypeBool)
                        n->literal=n->literal?UINT32_MAX:0;
                }
            }
        }
    if (c->stage!=VK_SHADER_STAGE_COMPUTE_BIT)
        for (uint32_t dense=0;dense<c->map.count;dense++) {
            struct cn *n=c->n+dense;
            if (n->p && cop(n->p)==SpvOpVariable &&
                (n->p[3]==SpvStorageClassInput || n->p[3]==SpvStorageClassOutput)) {
                const uint32_t *pointer=node(c,n->type)->p;
                if(cop(pointer)!=SpvOpTypePointer)
                    cfail(c,"graphics interface variable requires a pointer type");
                node(c,pointer[3])->io_block=1;
            }
        }
    call_graph(c,c->entry,1);
    active_variables(c);
    for (uint32_t dense=0;dense<c->map.count;dense++) {
        uint32_t id=c->map.ids[dense];
        struct cn *n = lookup(c,id);
        if (!n->p)
            continue;
        const uint32_t *p = n->p;
        if (n->builtin == SpvBuiltInWorkgroupSize &&
            (cop(p) == SpvOpConstantComposite || cop(p) == SpvOpSpecConstantComposite)) {
            if (cnw(p) != 6)
                cfail(c, "workgroup size");
            for (unsigned k = 0; k < 3; k++)
                c->out->local_size[k] = literal(c, p[3 + k]);
        }
        if (cop(p) != SpvOpVariable || (!n->function && !n->active) ||
            (n->function && !node(c,n->function)->reachable))
            continue;
        const uint32_t *type = node(c, n->type)->p;
        if (cop(type) != SpvOpTypePointer)
            cfail(c, "variable pointer type");
        n->root = id;
        n->pointee = type[3];
        if (p[3] == SpvStorageClassUniform || p[3] == SpvStorageClassStorageBuffer) {
            const uint32_t *block = node(c, type[3])->p;
            uint32_t count = 1;
            if (cop(block) == SpvOpTypeArray) {
                count = literal(c, block[3]);
                block = node(c, block[2])->p;
            }
            if (n->set < 0 || n->set >= ZVK_SETS || n->binding < 0 ||
                !count || count > 16 || count > ZVK_RESOURCES - c->out->nbindings ||
                cop(block) != SpvOpTypeStruct)
                cfail(c, "buffer descriptor binding");
            n->slot = c->out->nbindings;
            VkDescriptorType t = p[3] == SpvStorageClassStorageBuffer || node(c, block[1])->block == 1
                                     ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
                                     : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            for (uint32_t element = 0; element < count; element++)
                c->out->bindings[c->out->nbindings++] =
                    (zvk_shader_binding){.set = n->set,
                                         .binding = n->binding,
                                         .element = element,
                                         .type = t,
                                         .slot = n->slot + element,
                                         .input_attachment = UINT32_MAX};
        } else if (p[3] == SpvStorageClassUniformConstant) {
            const uint32_t *im = node(c, type[3])->p;
            uint32_t count = 1;
            if (cop(im) == SpvOpTypeArray) {
                count = literal(c, im[3]);
                im = node(c, im[2])->p;
                if (!count || count > 16)
                    cfail(c, "image descriptor array length");
            }
            if (cop(im)==SpvOpTypeSampler) {
                if (n->set<0 || n->set>=ZVK_SETS || n->binding<0 ||
                    count>ZVK_SAMPLERS-c->out->nsamplers)
                    cfail(c,"sampler descriptor binding");
                n->slot=ZVK_RESOURCES+c->out->nsamplers;
                for (unsigned j=0;j<count;j++)
                    c->out->samplers[c->out->nsamplers++]=(zvk_shader_binding){
                        .set=n->set,.binding=n->binding,.element=j,.type=VK_DESCRIPTOR_TYPE_SAMPLER};
                c->out->image_metadata=1;
                continue;
            }
            int combined = cop(im) == SpvOpTypeSampledImage;
            if (combined)
                im = node(c, im[2])->p;
            if (cop(im) != SpvOpTypeImage ||
                (im[3] != SpvDim1D && im[3] != SpvDim2D && im[3] != SpvDim3D &&
                 im[3] != SpvDimCube && im[3] != SpvDimBuffer && im[3]!=SpvDimSubpassData) ||
                (im[6] && (im[3] != SpvDimSubpassData && (im[3] != SpvDim2D || im[7] != 1))) ||
                (im[3]==SpvDimSubpassData && (combined || im[5] || im[7]!=2 ||
                    c->stage!=VK_SHADER_STAGE_FRAGMENT_BIT || n->input_attachment<0 ||
                    (uint64_t)n->input_attachment+count>ZVK_ATTACHMENTS)) ||
                ((im[3] == SpvDimBuffer || im[3] == SpvDim3D || im[3] == SpvDimCube) && im[5]) ||
                (im[7] != 2 && im[7] != 1) ||
                n->set < 0 || n->set >= ZVK_SETS || n->binding < 0 ||
                count > ZVK_RESOURCES - c->out->nbindings)
                cfail(c, "unsupported image descriptor");
            VkFormat format = image_format(c, im[8]);
            const uint32_t *scalar = node(c, im[2])->p;
            if (components(c, im[2]) != 1 ||
                (cop(scalar) != SpvOpTypeInt && cop(scalar) != SpvOpTypeFloat))
                cfail(c, "image sampled scalar type");
            if (im[7] == 2 && im[3]!=SpvDimSubpassData && format == VK_FORMAT_UNDEFINED)
                cfail(c, "formatless storage images require unimplemented features");
            n->slot = c->out->nbindings;
            for (uint32_t element = 0; element < count; element++) {
                unsigned slot = c->out->nbindings++;
                c->out->bindings[slot] = (zvk_shader_binding){
                    .set = n->set,
                    .binding = n->binding,
                    .element = element,
                    .slot = slot,
                    .input_attachment=im[3]==SpvDimSubpassData?(uint32_t)n->input_attachment+element:UINT32_MAX,
                    .type = im[3]==SpvDimSubpassData?VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
                            im[3] != SpvDimBuffer && im[7] == 1
                                ? (combined ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                            : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE)
                            : im[3] != SpvDimBuffer ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                            : im[7] == 1            ? VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER
                                                    : VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER,
                    .image_format = format,
                    .image_dim = im[3],
                    .image_array = im[5],
                    .image_ms = im[6],
                    .image_kind = cop(scalar) == SpvOpTypeFloat ? 2u
                                  : scalar[3]                   ? 1u
                                                                : 0u};
            }
            c->out->image_metadata = 1;
        } else if (p[3] == SpvStorageClassWorkgroup) {
            c->shared += bytes(c, type[3], 0);
            if (c->shared > 16384)
                cfail(c, "shared memory exceeds 16 KiB");
        } else if (p[3] != SpvStorageClassInput &&
                   !(p[3]==SpvStorageClassOutput && c->stage!=VK_SHADER_STAGE_COMPUTE_BIT) &&
                   p[3] != SpvStorageClassPushConstant &&
                   p[3] != SpvStorageClassFunction && p[3] != SpvStorageClassPrivate)
            cfail(c, "unsupported storage class %u", p[3]);
    }
    for (uint32_t dense=0;dense<c->map.count;dense++) {
        uint32_t id=c->map.ids[dense];
        struct cn *n = lookup(c,id);
        if (!n->p || cop(n->p) != SpvOpFunctionParameter)
            continue;
        const uint32_t *type = node(c, n->type)->p;
        if (cop(type) == SpvOpTypePointer) {
            n->root = id;
            n->pointee = type[3];
        }
    }
    uint64_t size = 1;
    for (unsigned i = 0; c->stage==VK_SHADER_STAGE_COMPUTE_BIT && i < 3; i++) {
        unsigned k = c->out->local_size[i];
        if (!k || k > (i == 2 ? 64u : 128u))
            cfail(c, "local size exceeds limits");
        size *= k;
    }
    if (size > 128)
        cfail(c, "local invocation count exceeds 128");
    unsigned ubos = 0, storage = 0, images = 0, texels = 0, sampled = 0;
    for (unsigned i = 0; i < c->out->nbindings; i++) {
        VkDescriptorType t = c->out->bindings[i].type;
        ubos += t == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        storage += t == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        images +=
            t == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE || t == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
        texels += t == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
        sampled +=
            t == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER || t == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    }
    if (ubos > ZVK_UBOS || storage > 8 || images > 4 || texels > ZVK_SAMPLERS ||
        sampled > ZVK_SAMPLERS)
        cfail(c, "per-stage descriptor limit");
}
static unsigned storage(struct cc *c, struct cn *n)
{
    const uint32_t *type = node(c, n->type)->p;
    if (cop(type) != SpvOpTypePointer)
        cfail(c, "pointer storage class");
    return type[2];
}
static void address(struct cc *c, uint32_t id, char *name, size_t capacity, char *limit,
                    size_t lcap)
{
    struct cn *ptr = node(c, id), *root = node(c, ptr->root);
    unsigned sc = storage(c, root);
    if (ptr->image_handle) {
        snprintf(name,capacity,"aimg%u",ptr->root);
        snprintf(limit,lcap,"sizes[h%u.x]",ptr->image_handle);
    } else if (cop(root->p) == SpvOpFunctionParameter) {
        snprintf(name, capacity, "m%u", ptr->root);
        snprintf(limit, lcap, "q%u", ptr->root);
    } else if (root->slot >= 0) {
        int slot = ptr->slot >= 0 ? ptr->slot : root->slot;
        snprintf(name, capacity, "b%d", slot);
        snprintf(limit, lcap, "sizes[%d]", slot);
    } else if (sc == SpvStorageClassPushConstant) {
        snprintf(name, capacity, "push");
        snprintf(limit, lcap, "128u");
    } else if (sc == SpvStorageClassInput) {
        snprintf(name, capacity, "m%u", ptr->root);
        snprintf(limit, lcap, "%uu", c->stage==VK_SHADER_STAGE_COMPUTE_BIT?16u:bytes(c,root->pointee,0));
    } else {
        snprintf(name, capacity, "m%u", ptr->root);
        snprintf(limit, lcap, "%uu", bytes(c, root->pointee, 0));
    }
}
static void access(struct cc *c, const uint32_t *p)
{
    struct cn *dst = node(c, p[2]), *base = node(c, p[3]);
    if (!base->root)
        cfail(c, "access chain base");
    if (opaque(c,base->type)) {
        const uint32_t *array=node(c,base->pointee)->p;
        if (cnw(p)!=5 || cop(array)!=SpvOpTypeArray)
            cfail(c,"opaque descriptor access chain");
        uint32_t element=literal(c,p[4]);
        if (element>=literal(c,array[3])) cfail(c,"descriptor array index");
        dst->root=base->root;dst->pointee=array[2];dst->slot=base->slot+element;
        int sampler=cop(node(c,array[2])->p)==SpvOpTypeSampler;
        int combined=cop(node(c,array[2])->p)==SpvOpTypeSampledImage;
        emit(c,"h%u=h%u+uint2(%uu,%uu);\n",p[2],p[3],sampler?0:element,sampler||combined?element:0);
        return;
    }
    dst->root = base->root;
    dst->slot = base->slot;
    dst->static_offset = base->static_offset;
    dst->dynamic = base->dynamic;
    dst->matrix_stride = base->matrix_stride;
    dst->component_stride = base->component_stride;
    dst->row_major = base->row_major;
    uint32_t type = base->pointee;
    emit(c, "o%u=o%u;\n", p[2], p[3]);
    unsigned first = 4;
    if (base->slot >= 0 && base->pointee == node(c, base->root)->pointee &&
        cop(node(c, type)->p) == SpvOpTypeArray) {
        const uint32_t *array = node(c, type)->p;
        if (cnw(p) <= first)
            cfail(c, "missing buffer descriptor index");
        uint32_t element = literal(c, p[first++]);
        if (element >= literal(c, array[3]))
            cfail(c, "buffer descriptor array index");
        dst->slot += element;
        type = array[2];
    }
    for (unsigned i = first; i < cnw(p); i++) {
        struct cn *n = node(c, type);
        const uint32_t *t = n->p;
        node(c, p[i]);
        if (cop(t) == SpvOpTypeStruct) {
            unsigned member = literal(c, p[i]);
            uint32_t offset = member_offset(c, type, member, 0);
            emit(c, "o%u+=%uu;\n", p[2], offset);
            dst->static_offset += offset;
            dst->matrix_stride = n->matrix_strides[member];
            dst->row_major = (n->row_major_mask >> member) & 1;
            dst->component_stride = 4;
            type = t[2 + member];
        } else if (cop(t) == SpvOpTypeArray || cop(t) == SpvOpTypeRuntimeArray) {
            unsigned stride = n->stride ? n->stride : bytes(c, t[2], 0);
            if (!stride || stride & 3)
                cfail(c, "array stride");
            emit(c, "o%u+=v%u.x*%uu;\n", p[2], p[i], stride);
            if (node(c, p[i])->known)
                dst->static_offset += literal(c, p[i]) * stride;
            else
                dst->dynamic = 1;
            type = t[2];
        } else if (cop(t) == SpvOpTypeMatrix || cop(t) == SpvOpTypeVector) {
            unsigned stride = dst->component_stride ? dst->component_stride : 4;
            if (cop(t) == SpvOpTypeMatrix) {
                unsigned column = dst->matrix_stride ? dst->matrix_stride : bytes(c, t[2], 0);
                stride = dst->row_major ? 4 : column;
                dst->component_stride = dst->row_major ? column : 4;
            }
            emit(c, "o%u+=v%u.x*%uu;\n", p[2], p[i], stride);
            if (node(c, p[i])->known)
                dst->static_offset += literal(c, p[i]) * stride;
            else
                dst->dynamic = 1;
            type = t[2];
        } else
            cfail(c, "access chain type");
    }
    dst->pointee = type;
}
#include "compute_memory.h"
static void binary(struct cc *c, const uint32_t *p, const char *op, const char *type, int compare)
{
    node(c, p[3]);
    node(c, p[4]);
    unsigned operation=cop(p);
    /* SPIR-V gives undefined values for these operands. Keep them local to
     * the result instead of introducing Metal/C undefined behavior that may
     * invalidate otherwise defined instructions in the same invocation. */
    if (operation==SpvOpUDiv || operation==SpvOpUMod ||
        operation==SpvOpSDiv || operation==SpvOpSRem) {
        emit(c,"{%s4 a=as_type<%s4>(v%u),b=as_type<%s4>(v%u);\n",
             type,type,p[3],type,p[4]);
        if (operation==SpvOpSDiv || operation==SpvOpSRem)
            emit(c,"b=select(b,int4(1),(b==int4(0))|((a==int4(as_type<int>(0x80000000u)))&(b==int4(-1))));\n");
        else emit(c,"b=select(b,uint4(1),b==uint4(0));\n");
        emit(c,"v%u=as_type<uint4>(a%sb);}\n",p[2],op);
        return;
    }
    if (operation==SpvOpShiftLeftLogical || operation==SpvOpShiftRightLogical ||
        operation==SpvOpShiftRightArithmetic) {
        emit(c,"v%u=as_type<uint4>(as_type<%s4>(v%u)%sas_type<%s4>(v%u&uint4(31)));\n",
             p[2],type,p[3],op,type,p[4]);
        return;
    }
    if (compare)
        emit(c, "v%u=select(uint4(0),uint4(0xffffffffu),as_type<%s4>(v%u)%sas_type<%s4>(v%u));\n",
             p[2], type, p[3], op, type, p[4]);
    else
        emit(c, "v%u=as_type<uint4>(as_type<%s4>(v%u)%sas_type<%s4>(v%u));\n", p[2], type, p[3], op,
             type, p[4]);
}
static void atomic_op(struct cc *c, const uint32_t *p)
{
    unsigned op = cop(p), store = op == SpvOpAtomicStore;
    unsigned ptrid = p[store ? 1 : 3];
    struct cn *ptr = node(c, ptrid), *root = node(c, ptr->root);
    unsigned scope = literal(c, p[store ? 2 : 4]), sem = literal(c, p[store ? 3 : 5]);
    if (scope != SpvScopeDevice && scope != SpvScopeWorkgroup && scope != SpvScopeInvocation)
        cfail(c,"unsupported atomic scope");
    unsigned sc=storage(c,root);
    if (sc!=SpvStorageClassWorkgroup && sc!=SpvStorageClassStorageBuffer &&
        sc!=SpvStorageClassUniform && sc!=SpvStorageClassImage && root->slot<0)
        cfail(c,"atomic pointer storage");
    unsigned order=sem;
    if (op==SpvOpAtomicCompareExchange || op==SpvOpAtomicCompareExchangeWeak)
        order |= literal(c,p[6]);
    unsigned release=SpvMemorySemanticsReleaseMask|SpvMemorySemanticsAcquireReleaseMask|SpvMemorySemanticsSequentiallyConsistentMask;
    unsigned acquire=SpvMemorySemanticsAcquireMask|SpvMemorySemanticsAcquireReleaseMask|SpvMemorySemanticsSequentiallyConsistentMask;
    const char *fence_scope=scope==SpvScopeDevice?"thread_scope_device":"thread_scope_threadgroup";
    if ((order&release) && scope!=SpvScopeInvocation)
        emit(c,"\n#if __METAL_VERSION__ >= 320\natomic_thread_fence(mem_flags::mem_threadgroup|mem_flags::mem_device,memory_order_seq_cst,%s);\n#else\n#error Atomic memory ordering requires Metal 3.2\n#endif\n",fence_scope);
    int slot = ptr->slot >= 0 ? ptr->slot : root->slot;
    if (slot >= 0 && c->out->bindings[slot].type != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER &&
        c->out->bindings[slot].type != VK_DESCRIPTOR_TYPE_STORAGE_IMAGE &&
        c->out->bindings[slot].type != VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER)
        cfail(c, "uniform atomic");
    int signed_op = op == SpvOpAtomicSMin || op == SpvOpAtomicSMax;
    char name[32], limit[32];
    address(c, ptrid, name, sizeof name, limit, sizeof limit);
    if (!store)
        emit(c, "v%u=uint4(0);\n", p[2]);
    emit(
        c,
        "if(o%u<=%s && 4u<=%s-o%u){ %s atomic_%s *a=reinterpret_cast<%s atomic_%s *>(%s+o%u/4u);\n",
        ptrid, limit, limit, ptrid, sc == SpvStorageClassWorkgroup ? "threadgroup" : "device",
        signed_op ? "int" : "uint", sc == SpvStorageClassWorkgroup ? "threadgroup" : "device",
        signed_op ? "int" : "uint", name, ptrid);
    if (op == SpvOpAtomicLoad)
        emit(c, "v%u=uint4(atomic_load_explicit(a,memory_order_relaxed));", p[2]);
    else if (store) {
        node(c, p[4]);
        emit(c, "atomic_store_explicit(a,v%u.x,memory_order_relaxed);", p[4]);
    } else if (op == SpvOpAtomicCompareExchange || op == SpvOpAtomicCompareExchangeWeak) {
        node(c, p[7]);
        node(c, p[8]);
        emit(c,
             "uint old=v%u.x;while(old==v%u.x && "
             "!atomic_compare_exchange_weak_explicit(a,&old,v%u.x,memory_order_relaxed,memory_"
             "order_relaxed)){}v%u=uint4(old);",
             p[8], p[8], p[7], p[2]);
    } else if (op==SpvOpAtomicIIncrement || op==SpvOpAtomicIDecrement) {
        emit(c,"v%u=uint4(atomic_fetch_%s_explicit(a,1u,memory_order_relaxed));",p[2],
             op==SpvOpAtomicIIncrement?"add":"sub");
    } else {
        const char *fn = op == SpvOpAtomicIAdd                            ? "add"
                         : op == SpvOpAtomicISub                          ? "sub"
                         : op == SpvOpAtomicAnd                           ? "and"
                         : op == SpvOpAtomicOr                            ? "or"
                         : op == SpvOpAtomicXor                           ? "xor"
                         : op == SpvOpAtomicSMin || op == SpvOpAtomicUMin ? "min"
                         : op == SpvOpAtomicSMax || op == SpvOpAtomicUMax ? "max"
                         : op == SpvOpAtomicExchange                      ? "exchange"
                                                                          : NULL;
        if (!fn)
            cfail(c, "unsupported atomic opcode");
        node(c, p[6]);
        emit(c, "v%u=uint4(atomic_%s%s_explicit(a,%s(v%u.x),memory_order_relaxed));", p[2],
             op == SpvOpAtomicExchange ? "" : "fetch_", fn, signed_op ? "int" : "uint", p[6]);
    }
    emit(c,"}\n");
    if ((order&acquire) && scope!=SpvScopeInvocation)
        emit(c,"\n#if __METAL_VERSION__ >= 320\natomic_thread_fence(mem_flags::mem_threadgroup|mem_flags::mem_device,memory_order_seq_cst,%s);\n#else\n#error Atomic memory ordering requires Metal 3.2\n#endif\n",fence_scope);
}
static const uint32_t *image_type(struct cc *c,uint32_t id)
{
    const uint32_t *p=node(c,node(c,id)->type)->p;
    if (cop(p)==SpvOpTypePointer) p=node(c,p[3])->p;
    if (cop(p)==SpvOpTypeSampledImage) p=node(c,p[2])->p;
    if (cop(p)!=SpvOpTypeImage) cfail(c,"image operand type");
    return p;
}
static void image_coord(struct cc *c,const uint32_t *im,uint32_t id)
{
    if(im[3]==SpvDimSubpassData) {
        emit(c,"uint3(uint2(fragment_coord.xy),0)");return;
    }
    unsigned count=im[3]==SpvDim1D?1+im[5]:im[3]==SpvDimBuffer?1:
                   im[3]==SpvDim3D||im[3]==SpvDimCube?3:2+im[5];
    if (components(c,node(c,id)->type)!=count) cfail(c,"image coordinate dimensions");
    if (count==1) emit(c,"uint3(v%u.x,0,0)",id);
    else if (im[3]==SpvDim1D) emit(c,"uint3(v%u.x,0,v%u.y)",id,id);
    else if (count==2) emit(c,"uint3(v%u.xy,0)",id);
    else emit(c,"v%u.xyz",id);
}
static void image_op(struct cc *c,const uint32_t *p)
{
    unsigned op=cop(p),store=op==SpvOpImageWrite,id=store?0:p[2];
    uint32_t image=p[store?1:3];
    const uint32_t *im=image_type(c,image);
    int sampled=im[7]==1 && im[3]!=SpvDimBuffer;
    emit(c,"{uint s=h%u.x;zvk_info m=imageinfo[s];\n",image);
    if (op==SpvOpImageQuerySamples) {
        emit(c,"v%u=uint4((m.format.y>>8u)&255u);\n}\n",id);
        return;
    }
    if (op==SpvOpImageQueryLevels) {
        emit(c,"v%u=uint4(m.format.y>>16u);\n}\n",id);
        return;
    }
    if (op==SpvOpImageQuerySizeLod || (op==SpvOpImageFetch && sampled && !im[6])) {
        unsigned lod=op==SpvOpImageQuerySizeLod?p[4]:cnw(p)>=7 && p[5]==SpvImageOperandsLodMask?p[6]:0;
        if (lod) {node(c,lod);emit(c,"uint lod=v%u.x;",lod);}
        else emit(c,"uint lod=0;");
        emit(c,"bool valid=lod<(m.format.y>>16u);m=imageinfo[%u+s*%u+min(lod,%uu)];\n",
             ZVK_RESOURCES,ZVK_MIPS,ZVK_MIPS-1);
    }
    if (op==SpvOpImageQuerySize || op==SpvOpImageQuerySizeLod) {
        if (im[6]) emit(c,"m.extent.z/=((m.format.y>>8u)&255u);\n");
        if (im[3]==SpvDim1D && im[5]) emit(c,"v%u=uint4(m.extent.x,m.extent.z,0,0);\n",id);
        else if (im[3]==SpvDim1D || im[3]==SpvDimBuffer) emit(c,"v%u=uint4(m.extent.x);\n",id);
        else emit(c,"v%u=uint4(m.extent.xyz,0);\n",id);
        emit(c,"}\n");return;
    }
    if (op==SpvOpImageTexelPointer) {
        if ((im[8]!=SpvImageFormatR32ui && im[8]!=SpvImageFormatR32i) || literal(c,p[5]))
            cfail(c,"image atomic format/sample");
        struct cn *dst=node(c,id);
        dst->root=id;dst->pointee=node(c,dst->type)->p[3];dst->dynamic=1;dst->image_handle=image;
        emit(c,"aimg%u=const_cast<device uint *>(buffers[s]);o%u=zvk_ioffset(m,",id,id);image_coord(c,im,p[4]);emit(c,");\n}\n");return;
    }
    uint32_t coord=p[store?2:4];
    if (!store && components(c,node(c,id)->type)!=4) cfail(c,"image read result shape");
    if (im[6]) {
        if ((op!=(im[3]==SpvDimSubpassData?SpvOpImageRead:SpvOpImageFetch)) || cnw(p)!=7 || p[5]!=SpvImageOperandsSampleMask)
            cfail(c,"multisample fetch operands");
        node(c,p[6]);node(c,coord);
        emit(c,"uint sample=v%u.x;v%u=uint4(0);if(sample<((m.format.y>>8u)&255u))v%u=zvk_iread(buffers[s],sizes[s],m,uint3(",p[6],id,id);
        if(im[3]==SpvDimSubpassData)emit(c,"uint2(fragment_coord.xy),");
        else emit(c,"v%u.xy,",coord);
        if (im[5]) emit(c,"v%u.z*((m.format.y>>8u)&255u)+",coord);
        emit(c,"sample));\n}\n");return;
    }
    if (!sampled && cnw(p)!=(store?4u:5u) && !(cnw(p)==(store?5u:6u) && !p[store?4:5]))
        cfail(c,"unsupported storage image operands");
    if (store) {
        if (components(c,node(c,p[3])->type)!=4 || im[7]!=2) cfail(c,"image store value/type");
        emit(c,"zvk_iwrite(const_cast<device uint *>(buffers[s]),sizes[s],m,");
    } else {
        if (sampled) emit(c,"v%u=uint4(0);if(valid)",id);
        emit(c,"v%u=zvk_iread(buffers[s],sizes[s],m,",id);
    }
    image_coord(c,im,coord);
    if (store) emit(c,",v%u",p[3]);
    emit(c,");\n}\n");
}
#include "compute_sampling.h"
static const uint32_t *skip_function_debug(struct cc *c,const uint32_t *p)
{
    while(p<c->code+c->words && (cop(p)==SpvOpLine || cop(p)==SpvOpNoLine))p+=cnw(p);
    if(p>=c->code+c->words)cfail(c,"unterminated function header");
    return p;
}
static const uint32_t *function_body(struct cc *c, uint32_t id)
{
    const uint32_t *p = node(c, id)->p;
    if (cop(p) != SpvOpFunction)
        cfail(c, "call target is not a function");
    p=skip_function_debug(c,p+cnw(p));
    while (p<c->code+c->words && cop(p) == SpvOpFunctionParameter)
        p=skip_function_debug(c,p+cnw(p));
    if (p>=c->code+c->words || cop(p) != SpvOpLabel)
        cfail(c, "function has no entry block");
    return p;
}
static unsigned call_graph(struct cc *c, uint32_t id, unsigned nesting)
{
    struct cn *fn = node(c, id);
    if (fn->visiting || nesting > 32)
        cfail(c, "recursive or excessively nested function calls");
    if (fn->depth)
        return fn->depth;
    fn->visiting = fn->reachable = 1;
    unsigned depth = 1;
    const uint32_t *body = function_body(c, id);
    for (const uint32_t *p = body; p < c->code + c->words && cop(p) != SpvOpFunctionEnd;
         p += cnw(p)) {
        if (cop(p) != SpvOpFunctionCall)
            continue;
        const uint32_t *callee = node(c, p[3])->p;
        if (cop(callee) != SpvOpFunction || p[1] != callee[1] || p[3] == c->entry)
            cfail(c, "function call type/target");
        const uint32_t *param = skip_function_debug(c,callee+cnw(callee));
        unsigned arg = 4;
        while (cop(param) == SpvOpFunctionParameter) {
            if (arg >= cnw(p) || node(c, p[arg])->type != param[1] ||
                (!value_registers(c, param[1], 0) && cop(node(c, param[1])->p) != SpvOpTypePointer && !opaque(c,param[1])))
                cfail(c, "function parameter type");
            param=skip_function_debug(c,param+cnw(param));
            arg++;
        }
        if (arg != cnw(p) ||
            (cop(node(c, callee[1])->p) != SpvOpTypeVoid && !value_registers(c, callee[1], 0) && !opaque(c,callee[1])))
            cfail(c, "function call signature");
        unsigned child = 1 + call_graph(c, p[3], nesting + 1);
        if (child > depth)
            depth = child;
    }
    if (depth > 32)
        cfail(c, "function call depth exceeds 32");
    fn->visiting = 0;
    return fn->depth = depth;
}
static void return_call(struct cc *c, uint32_t value)
{
    if (value) {
        struct cn *v = node(c, value);
        if (v->type != node(c, c->current_function)->p[1] || (!value_registers(c, v->type, 0) && !opaque(c,v->type)))
            cfail(c, "function return type");
        emit(c, "if(!callsp){pc=0;break;}--callsp;switch(calldst[callsp]){\n");
        for (size_t i = 5; i < c->words; i += cnw(c->code + i)) {
            const uint32_t *p = c->code + i;
            if (cop(p) == SpvOpFunctionCall && p[3] == c->current_function) {
                emit(c, "case %uu:", p[2]);
                if (opaque(c,v->type)) emit(c,"h%u=h%u;\n",p[2],value);
                else value_copy(c,p[2],value,v->type);
                emit(c, "break;\n");
            }
        }
        emit(c, "default:break;}pc=callpc[callsp];break;\n");
    } else
        emit(c, "if(callsp)pc=callpc[--callsp];else pc=0;break;\n");
}
#include "compute_algebra.h"
#include "compute_math.h"
static void metal_function_emit(struct cc *c,unsigned function,unsigned result);
static const char *metal_scalar(struct cc *c,unsigned type);
static void instruction(struct cc *c, const uint32_t *p, uint32_t label)
{
    unsigned op = cop(p);
    uint32_t id = cnw(p) > 2 ? p[2] : 0;
    switch (op) {
    case SpvOpExtInst:
        extended(c, p);
        break;
    case SpvOpDPdx:
    case SpvOpDPdy:
    case SpvOpFwidth:
    case SpvOpDPdxFine:
    case SpvOpDPdyFine:
    case SpvOpFwidthFine:
    case SpvOpDPdxCoarse:
    case SpvOpDPdyCoarse:
    case SpvOpFwidthCoarse: {
        if(c->stage!=VK_SHADER_STAGE_FRAGMENT_BIT)cfail(c,"derivative outside fragment stage");
        if(node(c,p[3])->type!=p[1] || strcmp(metal_scalar(c,p[1]),"float"))
            cfail(c,"derivative operand type");
        int coarse=op==SpvOpDPdxCoarse || op==SpvOpDPdyCoarse || op==SpvOpFwidthCoarse;
        int x=op==SpvOpDPdx || op==SpvOpDPdxFine || op==SpvOpDPdxCoarse;
        int y=op==SpvOpDPdy || op==SpvOpDPdyFine || op==SpvOpDPdyCoarse;
        emit(c,"{float4 dx=dfdx(as_type<float4>(v%u)),dy=dfdy(as_type<float4>(v%u));\n",p[3],p[3]);
        if(coarse)emit(c,"dx=quad_broadcast(dx,0);dy=quad_broadcast(dy,0);\n");
        emit(c,"v%u=as_type<uint4>(%s);}\n",id,x?"dx":y?"dy":"abs(dx)+abs(dy)");
        break;
    }
    case SpvOpFunctionCall: {
        const uint32_t *callee = node(c, p[3])->p;
        const uint32_t *param = skip_function_debug(c,callee+cnw(callee));
        for (unsigned arg = 4; arg < cnw(p); arg++,param=skip_function_debug(c,param+cnw(param))) {
            struct cn *dst = node(c, param[2]), *src = node(c, p[arg]);
            if (opaque(c,dst->type))
                emit(c,"h%u=h%u;\n",param[2],p[arg]);
            else if (cop(node(c, dst->type)->p) == SpvOpTypePointer) {
                if (!src->root || storage(c, node(c, src->root)) == SpvStorageClassUniformConstant)
                    cfail(c, "function pointer argument");
                char name[32], limit[32];
                address(c, p[arg], name, sizeof name, limit, sizeof limit);
                emit(c, "m%u=%s;q%u=%s;o%u=o%u;\n", param[2], name, param[2], limit, param[2], p[arg]);
            } else
                value_copy(c, param[2], p[arg], dst->type);
        }
        if (c->structured)
            metal_function_emit(c,p[3],id);
        else {
            uint32_t resume = c->bound + (uint32_t)(p - c->code);
            emit(c, "callpc[callsp]=%uu;calldst[callsp++]=%uu;pc=%uu;pred=0;break;}case %uu:{\n",
                 resume, id, function_body(c, p[3])[1], resume);
        }
        break;
    }
    case SpvOpVariable: {
        node(c, id);
        if (p[3] != SpvStorageClassFunction)
            cfail(c, "function variable class");
        if (cnw(p) > 4) {
            const uint32_t store[] = { (3u << 16) | SpvOpStore, id, p[4] };
            load_store(c, store, 1);
        }
        break;
    }
    case SpvOpLoad:
        if (opaque(c,p[1]))
            emit(c,"h%u=h%u;\n",id,p[3]);
        else
            load_store(c, p, 0);
        break;
    case SpvOpCopyMemory:
        copy_memory(c,p);
        break;
    case SpvOpQuantizeToF16: {
        unsigned n=components(c,node(c,id)->type);
        if (!n || node(c,p[3])->type!=node(c,id)->type)cfail(c,"quantize operand shape");
        for (unsigned k=0;k<n;k++)
            emit(c,"{uint h=zvk_float_pack(as_type<float>(v%u[%u]),16u,true);if((h&31744u)==0u)h&=32768u;v%u[%u]=as_type<uint>(zvk_float_unpack(h,16u,true));}\n",p[3],k,id,k);
        if (n==1)emit(c,"v%u=uint4(v%u.x);\n",id,id);
        break;
    }
    case SpvOpStore:
        if (opaque(c,node(c,p[2])->type)) emit(c,"h%u=h%u;\n",p[1],p[2]);
        else load_store(c,p,1);
        break;
    case SpvOpAccessChain:
    case SpvOpInBoundsAccessChain:
        access(c, p);
        break;
    case SpvOpCopyObject:
    case SpvOpBitcast: {
        struct cn *dst = node(c, id), *src = node(c, p[3]);
        if (opaque(c,dst->type))
            emit(c,"h%u=h%u;\n",id,p[3]);
        else if (cop(node(c, dst->type)->p) == SpvOpTypePointer) {
            if (!src->root)
                cfail(c, "pointer copy");
            dst->root = src->root;
            dst->slot = src->slot;
            dst->pointee = src->pointee;
            dst->static_offset = src->static_offset;
            dst->dynamic = src->dynamic;
            dst->matrix_stride = src->matrix_stride;
            dst->component_stride = src->component_stride;
            dst->row_major = src->row_major;
            dst->image_handle=src->image_handle;
            emit(c, "o%u=o%u;\n", id, p[3]);
        } else
            value_copy(c, id, p[3], dst->type);
        break;
    }
    case SpvOpImageSampleImplicitLod:
    case SpvOpImageSampleExplicitLod:
    case SpvOpImageSampleDrefImplicitLod:
    case SpvOpImageSampleDrefExplicitLod:
    case SpvOpImageSampleProjImplicitLod:
    case SpvOpImageSampleProjExplicitLod:
    case SpvOpImageSampleProjDrefImplicitLod:
    case SpvOpImageSampleProjDrefExplicitLod:
    case SpvOpImageGather:
    case SpvOpImageDrefGather:
        sample_op(c,p);
        break;
    case SpvOpImageQueryLod:
        query_lod(c,p);
        break;
    case SpvOpImageRead:
    case SpvOpImageWrite:
    case SpvOpImageFetch:
    case SpvOpImageQuerySize:
    case SpvOpImageQuerySizeLod:
    case SpvOpImageQueryLevels:
    case SpvOpImageQuerySamples:
    case SpvOpImageTexelPointer:
        image_op(c, p);
        break;
    case SpvOpSampledImage:
        node(c,p[3]);node(c,p[4]);
        emit(c,"h%u=uint2(h%u.x,h%u.y);\n",id,p[3],p[4]);
        break;
    case SpvOpImage:
        node(c,p[3]);
        emit(c,"h%u=h%u;\n",id,p[3]);
        break;
    case SpvOpAll:
    case SpvOpAny: {
        unsigned n = components(c, node(c, p[3])->type);
        if (n < 2 || n > 4)
            cfail(c, "boolean reduction vector");
        emit(c, "v%u=uint4(%s(v%u.%s!=uint%u(0))?0xffffffffu:0u);\n", id,
             op == SpvOpAll ? "all" : "any", p[3],
             n == 2   ? "xy"
             : n == 3 ? "xyz"
                      : "xyzw",
             n);
        break;
    }
    case SpvOpCompositeConstruct: {
        uint32_t type = node(c, id)->type;
        unsigned width = components(c, type), offset = 0;
        const uint32_t *t = node(c, type)->p;
        if (!width) {
            unsigned parts = cop(t) == SpvOpTypeStruct ? cnw(t) - 2
                             : cop(t) == SpvOpTypeArray ? literal(c, t[3])
                             : cop(t) == SpvOpTypeMatrix ? t[3] : 0;
            if (!parts || cnw(p) != parts + 3)
                cfail(c, "aggregate constructor shape");
        }
        for (unsigned i = 3; i < cnw(p); i++) {
            struct cn *n = node(c, p[i]);
            unsigned k = components(c, n->type);
            if (width) {
                if (!k || offset + k > width)
                    cfail(c, "vector constructor shape");
                for (unsigned j = 0; j < k; j++)
                    emit(c, "v%u[%u]=v%u[%u];\n", id, offset++, p[i], j);
            } else {
                uint32_t child = cop(t) == SpvOpTypeStruct ? t[i - 1] : t[2];
                if (child != n->type)
                    cfail(c, "aggregate constructor constituent type");
                unsigned regs = value_registers(c, child, 0);
                for (unsigned j = 0; j < regs; j++) {
                    if (k)
                        emit(c, "v%u[%u]=v%u;\n", id, offset++, p[i]);
                    else
                        emit(c, "v%u[%u]=v%u[%u];\n", id, offset++, p[i], j);
                }
            }
        }
        if (width && offset != width)
            cfail(c, "composite length");
        break;
    }
    case SpvOpCompositeExtract: {
        struct cn *source = node(c, p[3]);
        unsigned offset, lane;
        uint32_t selected = value_part(c, source->type, p + 4, cnw(p) - 4, &offset, &lane);
        if (selected != node(c, id)->type)
            cfail(c, "composite extraction result type");
        int aggregate = !components(c, source->type);
        unsigned width = components(c, selected);
        if (width == 1) {
            if (aggregate)
                emit(c, "v%u=uint4(v%u[%u][%u]);\n", id, p[3], offset, lane);
            else
                emit(c, "v%u=uint4(v%u[%u]);\n", id, p[3], lane);
        } else if (width)
            emit(c, "v%u=v%u[%u];\n", id, p[3], offset);
        else
            for (unsigned i = 0; i < value_registers(c, selected, 0); i++)
                emit(c, "v%u[%u]=v%u[%u];\n", id, i, p[3], offset + i);
        break;
    }
    case SpvOpCompositeInsert: {
        struct cn *src = node(c, p[4]), *part = node(c, p[3]);
        unsigned offset, lane;
        uint32_t selected = value_part(c, src->type, p + 5, cnw(p) - 5, &offset, &lane);
        if (selected != part->type || src->type != node(c, id)->type)
            cfail(c, "composite insertion type");
        value_copy(c, id, p[4], src->type);
        unsigned width = components(c, selected);
        if (width == 1) {
            if (components(c, src->type))
                emit(c, "v%u[%u]=v%u.x;\n", id,lane,p[3]);
            else
                emit(c, "v%u[%u][%u]=v%u.x;\n", id,offset,lane,p[3]);
        } else if (width)
            emit(c, "v%u[%u]=v%u;\n", id,offset,p[3]);
        else
            for (unsigned k=0;k<value_registers(c,selected,0);k++)
                emit(c, "v%u[%u]=v%u[%u];\n", id,offset+k,p[3],k);
        break;
    }
    case SpvOpVectorExtractDynamic:
    case SpvOpVectorInsertDynamic: {
        unsigned width = components(c, node(c, p[3])->type);
        unsigned index = p[op == SpvOpVectorInsertDynamic ? 5 : 4];
        node(c, index);
        if (width < 2)
            cfail(c, "dynamic vector operand");
        if (op == SpvOpVectorInsertDynamic) {
            node(c, p[4]);
            emit(c, "v%u=v%u;if(v%u.x<%uu)v%u[v%u.x]=v%u.x;\n", id,p[3],index,width,id,index,p[4]);
        } else
            emit(c, "v%u=uint4(v%u.x<%uu?v%u[v%u.x]:0u);\n", id,index,width,p[3],index);
        break;
    }
    case SpvOpTranspose:
    case SpvOpDot:
    case SpvOpMatrixTimesScalar:
    case SpvOpMatrixTimesVector:
    case SpvOpVectorTimesMatrix:
    case SpvOpMatrixTimesMatrix:
    case SpvOpOuterProduct:
        algebra(c, p);
        break;
    case SpvOpVectorShuffle: {
        node(c, p[3]);
        node(c, p[4]);
        unsigned a = components(c, node(c, p[3])->type), b = components(c, node(c, p[4])->type);
        for (unsigned i = 5; i < cnw(p); i++) {
            unsigned k = p[i];
            if (k == UINT32_MAX)
                emit(c, "v%u[%u]=0;", id, i - 5);
            else {
                if (k >= a + b || i - 5 >= 4)
                    cfail(c, "vector shuffle");
                emit(c, "v%u[%u]=v%u[%u];", id, i - 5, k < a ? p[3] : p[4], k < a ? k : k - a);
            }
        }
        break;
    }
    case SpvOpIAddCarry:
    case SpvOpISubBorrow:
    case SpvOpUMulExtended:
    case SpvOpSMulExtended: {
        const uint32_t *t = node(c, node(c, id)->type)->p;
        struct cn *a = node(c, p[3]), *b = node(c, p[4]);
        unsigned width = components(c, a->type);
        if (cnw(p) != 5 || cop(t) != SpvOpTypeStruct || cnw(t) != 4 ||
            !width || a->type != b->type || t[2] != a->type || t[3] != a->type)
            cfail(c, "extended integer result shape");
        for (unsigned k = 0; k < width; k++) {
            const char *low = op == SpvOpIAddCarry ? "+" : op == SpvOpISubBorrow ? "-" : "*";
            emit(c, "v%u[0][%u]=v%u[%u]%sv%u[%u];\n", id, k, p[3], k, low, p[4], k);
            if (op == SpvOpIAddCarry)
                emit(c, "v%u[1][%u]=uint(v%u[0][%u]<v%u[%u]);\n", id,k,id,k,p[3],k);
            else if (op == SpvOpISubBorrow)
                emit(c, "v%u[1][%u]=uint(v%u[%u]<v%u[%u]);\n", id,k,p[3],k,p[4],k);
            else {
                const char *kind = op == SpvOpSMulExtended ? "int" : "uint";
                emit(c, "v%u[1][%u]=as_type<uint>(mulhi(as_type<%s>(v%u[%u]),as_type<%s>(v%u[%u])));\n",
                     id,k,kind,p[3],k,kind,p[4],k);
            }
        }
        break;
    }
    case SpvOpIAdd:
        binary(c, p, "+", "uint", 0);
        break;
    case SpvOpISub:
        binary(c, p, "-", "uint", 0);
        break;
    case SpvOpIMul:
        binary(c, p, "*", "uint", 0);
        break;
    case SpvOpFAdd:
        binary(c, p, "+", "float", 0);
        break;
    case SpvOpFSub:
        binary(c, p, "-", "float", 0);
        break;
    case SpvOpFMul:
    case SpvOpVectorTimesScalar:
        binary(c, p, "*", "float", 0);
        break;
    case SpvOpFDiv:
        binary(c, p, "/", "float", 0);
        break;
    case SpvOpFRem:
        node(c, p[3]);
        node(c, p[4]);
        emit(c, "v%u=as_type<uint4>(fmod(as_type<float4>(v%u),as_type<float4>(v%u)));\n", id, p[3], p[4]);
        break;
    case SpvOpFMod:
        node(c, p[3]);
        node(c, p[4]);
        emit(c, "v%u=as_type<uint4>(as_type<float4>(v%u)-as_type<float4>(v%u)*floor(as_type<float4>(v%u)/as_type<float4>(v%u)));\n",
             id, p[3], p[4], p[3], p[4]);
        break;
    case SpvOpUDiv:
        binary(c, p, "/", "uint", 0);
        break;
    case SpvOpSDiv:
        binary(c, p, "/", "int", 0);
        break;
    case SpvOpUMod:
        binary(c, p, "%", "uint", 0);
        break;
    case SpvOpSRem:
        binary(c, p, "%", "int", 0);
        break;
    case SpvOpSMod: {
        node(c, p[3]);
        node(c, p[4]);
        unsigned n = components(c, node(c, id)->type);
        for (unsigned k = 0; k < n; k++)
            emit(c, "{int a=as_type<int>(v%u[%u]),b=as_type<int>(v%u[%u]);"
                    "if(b==0||(a==as_type<int>(0x80000000u)&&b==-1))b=1;int r=a%%b;"
                    "if(r!=0&&((r<0)!=(b<0)))r+=b;v%u[%u]=as_type<uint>(r);}\n",
                 p[3], k, p[4], k, id, k);
        if (n == 1) emit(c, "v%u=uint4(v%u.x);\n", id, id);
        break;
    }
    case SpvOpIsNan:
    case SpvOpIsInf:
        node(c, p[3]);
        emit(c, "v%u=select(uint4(0),uint4(0xffffffffu),%s(as_type<float4>(v%u)));\n",
             id, op == SpvOpIsNan ? "isnan" : "isinf", p[3]);
        break;
    case SpvOpBitCount:
    case SpvOpBitReverse:
        node(c, p[3]);
        emit(c, "v%u=%s(v%u);\n", id, op == SpvOpBitCount ? "popcount" : "reverse_bits", p[3]);
        break;
    case SpvOpBitFieldInsert:
    case SpvOpBitFieldSExtract:
    case SpvOpBitFieldUExtract: {
        int insert = op == SpvOpBitFieldInsert;
        node(c, p[3]);
        if (insert) node(c, p[4]);
        node(c, p[insert ? 5 : 4]);
        node(c, p[insert ? 6 : 5]);
        unsigned n = components(c, node(c, id)->type);
        emit(c, "{uint offset=min(v%u.x,32u),count=min(v%u.x,32u-offset);\n",
             p[insert ? 5 : 4], p[insert ? 6 : 5]);
        for (unsigned k = 0; k < n; k++) {
            if (insert)
                emit(c, "if(count==0)v%u[%u]=v%u[%u];else{uint mask=count==32?0xffffffffu:(1u<<count)-1u;"
                        "mask<<=offset;v%u[%u]=(v%u[%u]&~mask)|((v%u[%u]<<offset)&mask);}\n",
                     id, k, p[3], k, id, k, p[3], k, p[4], k);
            else {
                emit(c, "if(count==0)v%u[%u]=0;else{uint bits=v%u[%u]>>offset;", id, k, p[3], k);
                if (op == SpvOpBitFieldSExtract)
                    emit(c, "v%u[%u]=as_type<uint>(as_type<int>(bits<<(32u-count))>>(32u-count));}\n", id, k);
                else
                    emit(c, "v%u[%u]=count==32?bits:bits&((1u<<count)-1u);}\n", id, k);
            }
        }
        emit(c, "}\n");
        if (n == 1) emit(c, "v%u=uint4(v%u.x);\n", id, id);
        break;
    }
    case SpvOpLogicalAnd:
    case SpvOpLogicalOr:
    case SpvOpLogicalNotEqual:
    case SpvOpLogicalEqual:
        node(c,p[3]);node(c,p[4]);
        emit(c,"v%u=select(uint4(0),uint4(0xffffffffu),(v%u!=uint4(0))%s(v%u!=uint4(0)));\n",id,p[3],
             op==SpvOpLogicalAnd?"&":op==SpvOpLogicalOr?"|":op==SpvOpLogicalEqual?"==":"!=",p[4]);
        break;
    case SpvOpBitwiseAnd:
        binary(c, p, "&", "uint", 0);
        break;
    case SpvOpBitwiseOr:
        binary(c, p, "|", "uint", 0);
        break;
    case SpvOpBitwiseXor:
        binary(c, p, "^", "uint", 0);
        break;
    case SpvOpIEqual:
        binary(c, p, "==", "uint", 1);
        break;
    case SpvOpINotEqual:
        binary(c, p, "!=", "uint", 1);
        break;
    case SpvOpULessThan:
        binary(c, p, "<", "uint", 1);
        break;
    case SpvOpULessThanEqual:
        binary(c, p, "<=", "uint", 1);
        break;
    case SpvOpUGreaterThan:
        binary(c, p, ">", "uint", 1);
        break;
    case SpvOpUGreaterThanEqual:
        binary(c, p, ">=", "uint", 1);
        break;
    case SpvOpSLessThan:
        binary(c, p, "<", "int", 1);
        break;
    case SpvOpSLessThanEqual:
        binary(c, p, "<=", "int", 1);
        break;
    case SpvOpSGreaterThan:
        binary(c, p, ">", "int", 1);
        break;
    case SpvOpSGreaterThanEqual:
        binary(c, p, ">=", "int", 1);
        break;
    case SpvOpFOrdLessThan:
        binary(c, p, "<", "float", 1);
        break;
    case SpvOpFOrdGreaterThan:
        binary(c, p, ">", "float", 1);
        break;
    case SpvOpFOrdLessThanEqual:
        binary(c, p, "<=", "float", 1);
        break;
    case SpvOpFOrdGreaterThanEqual:
        binary(c, p, ">=", "float", 1);
        break;
    case SpvOpFOrdEqual:
        binary(c, p, "==", "float", 1);
        break;
    case SpvOpFOrdNotEqual:
        node(c, p[3]);
        node(c, p[4]);
        emit(c,
             "v%u=select(uint4(0),uint4(0xffffffffu),!isnan(as_type<float4>(v%u)) & "
             "!isnan(as_type<float4>(v%u)) & (as_type<float4>(v%u)!=as_type<float4>(v%u)));\n",
             id, p[3], p[4], p[3], p[4]);
        break;
    case SpvOpFUnordEqual:
    case SpvOpFUnordNotEqual:
    case SpvOpFUnordLessThan:
    case SpvOpFUnordGreaterThan:
    case SpvOpFUnordLessThanEqual:
    case SpvOpFUnordGreaterThanEqual: {
        node(c, p[3]);
        node(c, p[4]);
        const char *operation = op == SpvOpFUnordEqual ? "==" : op == SpvOpFUnordNotEqual ? "!="
                                : op == SpvOpFUnordLessThan ? "<" : op == SpvOpFUnordGreaterThan ? ">"
                                : op == SpvOpFUnordLessThanEqual ? "<=" : ">=";
        emit(c, "v%u=select(uint4(0),uint4(0xffffffffu),isnan(as_type<float4>(v%u))|"
                "isnan(as_type<float4>(v%u))|(as_type<float4>(v%u)%sas_type<float4>(v%u)));\n",
             id, p[3], p[4], p[3], operation, p[4]);
        break;
    }
    case SpvOpShiftLeftLogical:
        binary(c, p, "<<", "uint", 0);
        break;
    case SpvOpShiftRightLogical:
        binary(c, p, ">>", "uint", 0);
        break;
    case SpvOpShiftRightArithmetic:
        binary(c, p, ">>", "int", 0);
        break;
    case SpvOpLogicalNot:
        node(c,p[3]);
        emit(c,"v%u=select(uint4(0),uint4(0xffffffffu),v%u==uint4(0));\n",id,p[3]);
        break;
    case SpvOpNot:
        node(c, p[3]);
        emit(c, "v%u=~v%u;\n", id, p[3]);
        break;
    case SpvOpSNegate:
        node(c, p[3]);
        emit(c, "v%u=uint4(0)-v%u;\n", id, p[3]);
        break;
    case SpvOpFNegate:
        node(c, p[3]);
        emit(c, "v%u=as_type<uint4>(-as_type<float4>(v%u));\n", id, p[3]);
        break;
    case SpvOpConvertUToF:
    case SpvOpConvertSToF:
        node(c, p[3]);
        emit(c, "v%u=as_type<uint4>(float4(as_type<%s4>(v%u)));\n", id,
             op == SpvOpConvertSToF ? "int" : "uint", p[3]);
        break;
    case SpvOpConvertFToU:
    case SpvOpConvertFToS:
        node(c, p[3]);
        emit(c, "v%u=as_type<uint4>(%s4(as_type<float4>(v%u)));\n", id,
             op == SpvOpConvertFToS ? "int" : "uint", p[3]);
        break;
    case SpvOpSelect: {
        node(c, p[3]);
        node(c, p[4]);
        node(c, p[5]);
        int scalar = components(c, node(c, p[3])->type) == 1;
        emit(c, "v%u=select(v%u,v%u,v%u%s!=%s(0));\n", id, p[5], p[4], p[3], scalar ? ".x" : "",
             scalar ? "uint" : "uint4");
        break;
    }
    case SpvOpPhi:
    case SpvOpLoopMerge:
    case SpvOpSelectionMerge:
    case SpvOpLine:
    case SpvOpNoLine:
    case SpvOpNop:
    case SpvOpUndef:
        break;
    case SpvOpBranch:
        if (cop(node(c, p[1])->p) != SpvOpLabel || node(c, p[1])->function != c->current_function)
            cfail(c, "branch target");
        emit(c, "pred=%uu;pc=%uu;break;\n", label, p[1]);
        break;
    case SpvOpBranchConditional:
        node(c, p[1]);
        if (cop(node(c, p[2])->p) != SpvOpLabel || cop(node(c, p[3])->p) != SpvOpLabel ||
            node(c, p[2])->function != c->current_function ||
            node(c, p[3])->function != c->current_function)
            cfail(c, "branch target");
        emit(c, "pred=%uu;pc=v%u.x?%uu:%uu;break;\n", label, p[1], p[2], p[3]);
        break;
    case SpvOpSwitch:
        node(c, p[1]);
        if (cop(node(c, p[2])->p) != SpvOpLabel || node(c, p[2])->function != c->current_function)
            cfail(c, "switch default target");
        emit(c, "pred=%uu;pc=%uu;", label, p[2]);
        for (unsigned i = 3; i + 1 < cnw(p); i += 2) {
            if (cop(node(c, p[i + 1])->p) != SpvOpLabel ||
                node(c, p[i + 1])->function != c->current_function)
                cfail(c, "switch case target");
            emit(c, "if(v%u.x==%uu)pc=%uu;", p[1], p[i], p[i + 1]);
        }
        emit(c, "break;\n");
        break;
    case SpvOpUnreachable:
        return_call(c,0);
        break;
    case SpvOpKill:
        if (c->stage!=VK_SHADER_STAGE_FRAGMENT_BIT)
            cfail(c,"discard outside a fragment shader");
        emit(c,"discard_fragment();pc=0;break;\n");
        break;
    case SpvOpReturn:
        if (cop(node(c, node(c, c->current_function)->p[1])->p) != SpvOpTypeVoid)
            cfail(c, "missing function return value");
        return_call(c, 0);
        break;
    case SpvOpReturnValue:
        return_call(c, p[1]);
        break;
    case SpvOpControlBarrier:
        if (literal(c, p[1]) != SpvScopeWorkgroup || literal(c, p[2]) != SpvScopeWorkgroup)
            cfail(c, "barrier scope");
        emit(c, "threadgroup_barrier(mem_flags::mem_threadgroup|mem_flags::mem_device);\n");
        break;
    case SpvOpMemoryBarrier: {
        unsigned scope = literal(c, p[1]), semantics = literal(c, p[2]);
        unsigned memory = SpvMemorySemanticsUniformMemoryMask |
                          SpvMemorySemanticsWorkgroupMemoryMask | SpvMemorySemanticsImageMemoryMask |
                          SpvMemorySemanticsCrossWorkgroupMemoryMask | SpvMemorySemanticsAtomicCounterMemoryMask;
        unsigned ordering = SpvMemorySemanticsAcquireMask | SpvMemorySemanticsReleaseMask |
                            SpvMemorySemanticsAcquireReleaseMask |
                            SpvMemorySemanticsSequentiallyConsistentMask;
        if ((scope != SpvScopeDevice && scope != SpvScopeWorkgroup &&
             scope != SpvScopeInvocation) ||
            (semantics & ~(memory | ordering)))
            cfail(c, "memory barrier scope %u semantics 0x%x",scope,semantics);
        /* A memory barrier must not rendezvous invocations: it is legal in
         * divergent control flow. Metal 3.2 fences provide stronger seq_cst
         * ordering without introducing an execution barrier. */
        if (semantics & ordering)
            emit(c,
                 "\n#if __METAL_VERSION__ >= 320\n"
                 "atomic_thread_fence(mem_flags::mem_threadgroup|mem_flags::mem_device,"
                 "memory_order_seq_cst,%s);\n#else\n#error Memory fences require Metal "
                 "3.2\n#endif\n",
                 scope == SpvScopeDevice ? "thread_scope_device" : "thread_scope_threadgroup");
        break;
    }
    case SpvOpArrayLength: {
        struct cn *ptr = node(c, p[3]), *root = node(c, ptr->root), *type = node(c, ptr->pointee);
        if (root->slot < 0 || cop(type->p) != SpvOpTypeStruct || p[4] >= cnw(type->p) - 2 ||
            p[4] >= C_MEMBERS || !(type->offset_mask & (1ull << p[4])))
            cfail(c, "runtime array length");
        struct cn *array = node(c, type->p[2 + p[4]]);
        if (cop(array->p) != SpvOpTypeRuntimeArray || array->stride <= 0)
            cfail(c, "runtime array type");
        int slot = ptr->slot >= 0 ? ptr->slot : root->slot;
        emit(c, "v%u=uint4(sizes[%d]>%uu?(sizes[%d]-%uu)/%uu:0u);\n", id, slot, type->offsets[p[4]],
             slot, type->offsets[p[4]], array->stride);
        break;
    }
    case SpvOpAtomicLoad:
    case SpvOpAtomicStore:
    case SpvOpAtomicExchange:
    case SpvOpAtomicCompareExchange:
    case SpvOpAtomicCompareExchangeWeak:
    case SpvOpAtomicIIncrement:
    case SpvOpAtomicIDecrement:
    case SpvOpAtomicIAdd:
    case SpvOpAtomicISub:
    case SpvOpAtomicSMin:
    case SpvOpAtomicUMin:
    case SpvOpAtomicSMax:
    case SpvOpAtomicUMax:
    case SpvOpAtomicAnd:
    case SpvOpAtomicOr:
    case SpvOpAtomicXor:
        atomic_op(c, p);
        break;
    default:
        cfail(c, "unsupported instruction %s", SpvOpToString((SpvOp)op));
    }
}
#include "metal_flow.h"
#include "metal_io.h"
static VkResult metal_compile(const uint32_t *code, size_t words, const char *entry,
                         const VkSpecializationInfo *spec, VkShaderStageFlagBits stage, zvk_shader *out)
{
    memset(out, 0, sizeof *out);
    if (zvk_shader_check(code, words) != VK_SUCCESS)
        return VK_ERROR_INVALID_SHADER_NV;
    if (code[3] > C_IDS) {
        zvk_error("compute module exceeds %u IDs", C_IDS);
        return VK_ERROR_INVALID_SHADER_NV;
    }
    struct cc *c = calloc(1, sizeof *c);
    if (!c)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    VkResult group_result=zvk_spirv_groups(code,words,&c->expanded,&words);
    if (group_result!=VK_SUCCESS){free(c);return group_result;}
    if (c->expanded)code=c->expanded;
    c->code = code;
    c->words = words;
    c->bound = code[3];
    c->out = out;
    c->stage=stage;
    VkResult ids_result=zvk_spirv_ids(code,words,&c->map);
    if (ids_result!=VK_SUCCESS){free(c->expanded);free(c);return ids_result;}
    c->n=calloc(c->map.count,sizeof *c->n);
    if (!c->n) {
        free(c->map.ids);
        free(c->expanded);
        free(c);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    VkResult result = VK_SUCCESS;
    if (setjmp(c->fail)) {
        result = VK_ERROR_INVALID_SHADER_NV;
        goto done;
    }
    parse(c, entry ? entry : "main", spec);
    call_graph(c, c->entry, 1);
    int graphics=stage!=VK_SHADER_STAGE_COMPUTE_BIT;
    /* Barriers must rendezvous after divergent branches/calls have merged.
     * A per-invocation program-counter dispatcher cannot express that merge
     * to the native GPU compiler. Preserve structured flow for these modules. */
    c->structured=graphics;
    for(size_t i=5;i<c->words;i+=cnw(c->code+i))
        if(cop(c->code+i)==SpvOpControlBarrier)c->structured=1;
    out->packed_buffers = graphics || out->nbindings > 28;
    out->image_metadata |= out->packed_buffers;
    emit(c, "#include <metal_stdlib>\n#pragma clang fp contract(off)\nusing namespace metal;\n");
    if (out->image_metadata) {
        emit(c,"#define ZVK_RESOURCES %u\n#define ZVK_MIPS %u\n#define ZVK_META_SAMPLER %u\n",ZVK_RESOURCES,ZVK_MIPS,ZVK_META_SAMPLER);
        emit(c,"%s%s",zvk_texel_msl,zvk_sampling_msl);
    }
    unsigned graphics_outputs=0;
    if (graphics) {
        emit(c,"struct Zvk%sIn {\n",metal_stage_name(c));
        unsigned inputs=metal_io_emit(c,0);emit(c,"};\n");
        emit(c,"struct Zvk%sOut {\n",metal_stage_name(c));
        graphics_outputs=metal_io_emit(c,1);emit(c,"};\n");
        metal_graphics_signature(c,inputs,graphics_outputs);
    } else {
    emit(c, "kernel void zvk_main(");
    if (out->packed_buffers)
        emit(c, "device uint *arena [[buffer(0)]],const device zvk_info *imageinfo [[buffer(1)]],");
    else {
        for (unsigned i = 0; i < out->nbindings; i++)
            emit(c, "%s uint *b%u [[buffer(%u)]],",
                 out->bindings[i].type == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER
                     ? "const device"
                     : "device",
                 i, i);
        if (out->image_metadata)
            emit(c, "const device zvk_info *imageinfo [[buffer(%u)]],", out->nbindings);
    }
    emit(c,
         "constant uint *%s [[buffer(29)]],constant uint *push [[buffer(30)]],uint3 gid "
         "[[thread_position_in_grid]],uint3 lid [[thread_position_in_threadgroup]],uint3 wid "
         "[[threadgroup_position_in_grid]],uint3 ng [[threadgroups_per_grid]],uint3 bs "
         "[[threads_per_threadgroup]],uint linear [[thread_index_in_threadgroup]]) {\n",
         out->packed_buffers ? "physical_sizes" : "sizes");
    }
    if (out->packed_buffers) {
        emit(c,
             "const device uint2 *ranges=reinterpret_cast<const device uint2 *>(imageinfo+%u);\n"
             "uint sizes[%u];\n",
             ZVK_META_RANGES, ZVK_RESOURCES);
        for (unsigned i = 0; i < out->nbindings; i++)
            emit(c, "device uint *b%u=arena+ranges[%u].x/4u;sizes[%u]=ranges[%u].y;\n", i, i, i, i);
    }
    if (out->nbindings) {
        emit(c,"const device uint *buffers[%u]={",out->nbindings);
        for (unsigned i=0;i<out->nbindings;i++) emit(c,"%sb%u",i?",":"",i);
        emit(c,"};\n");
    }
    for (uint32_t dense=0;dense<c->map.count;dense++) {
        uint32_t id=c->map.ids[dense];
        struct cn *n = lookup(c,id);
        if (!n->p || !n->type || (n->function && !node(c,n->function)->reachable))
            continue;
        const uint32_t *p = n->p;
        if (cop(p)==SpvOpVariable && !n->function && !n->active)continue;
        if (opaque(c,n->type)) {
            unsigned image=0,sampler=0;
            if (cop(p)==SpvOpVariable && p[3]==SpvStorageClassUniformConstant) {
                if (n->slot>=ZVK_RESOURCES) sampler=n->slot;
                else image=sampler=n->slot;
            }
            emit(c,"uint2 h%u=uint2(%uu,%uu);\n",id,image,sampler);
        }
        if (components(c, n->type))
            emit(c, "uint4 v%u=uint4(0);\n", id);
        else if (value_registers(c, n->type, 0))
            emit(c, "uint4 v%u[%u]={};\n", id, value_registers(c, n->type, 0));
        if (cop(node(c, n->type)->p) == SpvOpTypePointer) {
            emit(c,"uint o%u=0;\n",id);
            if (cop(p)==SpvOpImageTexelPointer) emit(c,"device uint *aimg%u=nullptr;\n",id);
        }
        if (cop(p) == SpvOpFunctionParameter && cop(node(c, n->type)->p) == SpvOpTypePointer && !opaque(c,n->type)) {
            unsigned sc = storage(c, n);
            const char *space = sc == SpvStorageClassWorkgroup ? "threadgroup"
                                : sc == SpvStorageClassFunction || sc == SpvStorageClassPrivate ? "thread"
                                : sc == SpvStorageClassPushConstant ? "constant" : "device";
            emit(c, "%s uint *m%u=nullptr;uint q%u=0;\n", space, id, id);
        }
        if (cop(p) == SpvOpVariable && n->slot < 0 && p[3] != SpvStorageClassPushConstant && !opaque(c,n->type)) {
            if (p[3] == SpvStorageClassInput && !graphics) {
                emit(c, "uint4 in%u=%s;thread uint *m%u=reinterpret_cast<thread uint *>(&in%u);\n",
                     id, builtin(c, n->builtin), id, id);
            } else {
                unsigned size = bytes(c, n->pointee, 0);
                if (size & 3) cfail(c,"variable size");
                if (!size)size=4;
                emit(c, "%s uint m%u[%u];\n",
                     p[3] == SpvStorageClassWorkgroup ? "threadgroup" : "thread", id, size / 4);
            }
        }
    }
    for (uint32_t dense=0;dense<c->map.count;dense++) {
        uint32_t id=c->map.ids[dense];
        struct cn *n = lookup(c,id);
        if (!n->p || !n->type)
            continue;
        const uint32_t *p = n->p;
        if (n->known || cop(p) == SpvOpConstantComposite || cop(p) == SpvOpSpecConstantComposite || cop(p) == SpvOpSpecConstantOp)
            value_constant(c, id, id, 0, !components(c, n->type), 0);
    }
    if (graphics)metal_io_emit(c,2);
    const uint32_t *fn = node(c, c->entry)->p, *start = fn + cnw(fn);
    for (uint32_t dense=0;dense<c->map.count;dense++) {
        uint32_t id=c->map.ids[dense];
        struct cn *n = lookup(c,id);
        if (!n->p || cop(n->p) != SpvOpVariable || cnw(n->p) < 5)
            continue;
        if (n->p[3] == SpvStorageClassWorkgroup)
            cfail(c, "workgroup variable initializer");
        if (n->p[3] == SpvStorageClassPrivate && n->active) {
            const uint32_t store[] = { (3u << 16) | SpvOpStore, id, n->p[4] };
            load_store(c, store, 1);
        }
    }
    if (cop(fn) != SpvOpFunction)
        cfail(c, "entry is not a function");
    if (cop(start) != SpvOpLabel)
        cfail(c, "compute entry has parameters");
    if (c->structured) {
        emit(c,"bool discarded=false;\n");metal_function_emit(c,c->entry,0);
    } else {
    emit(c, "uint callpc[32],calldst[32],callsp=0;uint pc=%uu,pred=0;while(pc){switch(pc){\n",
         start[1]);
    for (uint32_t dense=0;dense<c->map.count;dense++) {
        uint32_t function=c->map.ids[dense];
        if (!lookup(c,function)->reachable)
            continue;
        c->current_function = function;
        start = function_body(c, function);
        int open = 0;
        for (const uint32_t *p = start; p < code + words && cop(p) != SpvOpFunctionEnd;) {
            if (cop(p) != SpvOpLabel)
                cfail(c, "missing block label");
            uint32_t label = p[1];
            if (open)
                emit(c, "}\n");
            open = 1;
            emit(c, "case %uu:{\n", label);
            p += cnw(p);
            const uint32_t *q = p;
            while (cop(q) == SpvOpPhi) {
                if (cnw(q) < 5 || (cnw(q) - 3) % 2)
                    cfail(c, "phi operands");
                unsigned id = q[2];
                uint32_t type = node(c, id)->type;
                unsigned width = components(c,type), regs = value_registers(c,type,0);
                if (!regs)
                    cfail(c,"pointer phi requires variable pointers");
                if (width)
                    emit(c,"uint4 phi%u=uint4(0);",id);
                else
                    emit(c,"uint4 phi%u[%u]={};",id,regs);
                for (unsigned k = 3; k < cnw(q); k += 2) {
                    if (node(c,q[k])->type != type || cop(node(c,q[k+1])->p) != SpvOpLabel)
                        cfail(c,"phi operand type");
                    emit(c,"if(pred==%uu){",q[k+1]);
                    if (width)
                        emit(c,"phi%u=v%u;",id,q[k]);
                    else
                        for (unsigned j=0;j<regs;j++)
                            emit(c,"phi%u[%u]=v%u[%u];",id,j,q[k],j);
                    emit(c,"}\n");
                }
                q += cnw(q);
            }
            for (const uint32_t *r=p;r<q;r+=cnw(r)) {
                unsigned width=components(c,node(c,r[2])->type);
                if (width)
                    emit(c,"v%u=phi%u;\n",r[2],r[2]);
                else
                    for (unsigned j=0;j<value_registers(c,node(c,r[2])->type,0);j++)
                        emit(c,"v%u[%u]=phi%u[%u];\n",r[2],j,r[2],j);
            }
            p = q;
            while (p < code + words && cop(p) != SpvOpLabel && cop(p) != SpvOpFunctionEnd) {
                instruction(c, p, label);
                p += cnw(p);
            }
        }
        if (open)
            emit(c, "}\n");
    }
    emit(c, "default:pc=0;break;}}\n");
    }
    if (graphics && graphics_outputs) {
        emit(c,"Zvk%sOut result={};\n",metal_stage_name(c));
        metal_io_emit(c,3);emit(c,"return result;\n");
    }
    emit(c,"}\n");
    out->metal = c->text;
    c->text = NULL;
    if (getenv("ZVK_DEBUG_SHADERS"))
        fprintf(stderr, "zvk Metal compute:\n%s\n", out->metal);
done:
    for(unsigned i=0;i<33;i++)free(c->flow_alloc[i]);
    free(c->text);
    free(c->n);
    free(c->map.ids);
    free(c->expanded);
    free(c);
    if (result != VK_SUCCESS)
        zvk_shader_finish(out);
    return result;
}

VkResult zvk_spirv_metal(const uint32_t *code,size_t words,const char *entry,
                         const VkSpecializationInfo *spec,zvk_shader *out)
{
    return metal_compile(code,words,entry,spec,VK_SHADER_STAGE_COMPUTE_BIT,out);
}
VkResult zvk_spirv_metal_graphics(const uint32_t *code,size_t words,const char *entry,
                                  const VkSpecializationInfo *spec,VkShaderStageFlagBits stage,zvk_shader *out)
{
    if(stage!=VK_SHADER_STAGE_VERTEX_BIT && stage!=VK_SHADER_STAGE_FRAGMENT_BIT) {
        memset(out,0,sizeof *out);
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    return metal_compile(code,words,entry,spec,stage,out);
}
