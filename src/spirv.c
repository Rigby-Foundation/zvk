/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (C) 2026 Rigby Foundation */
/* Independent SPIR-V -> TGSI frontend. SSA values have fixed vec4 storage.
 * Structured TGSI regions preserve loop/selection reconvergence and phi
 * parallel copies. Nonrecursive calls inline with structured return unwinding. */
#include "zvk_internal.h"
#include <stdbool.h>
#define SPV_ENABLE_UTILITY_CODE
#define inline static inline
#include <spirv/unified1/spirv.h>
#undef inline
#include <spirv/unified1/GLSL.std.450.h>
#include <setjmp.h>
#include <stdarg.h>
#include "spv_min_words.h"
#include "spirv_float.h"

#define MAX_IDS 0x400000
#define MAX_TEMPS 4096
#define MAX_MEMBERS 64
#define MAX_IMM 4096

struct text {
    char *data;
    size_t size, capacity;
};
struct decoration {
    int location, builtin, set, offset, stride, matrix_stride, input_attachment, component;
    int64_t binding,spec;
    unsigned flat : 1, linear : 1, centroid : 1, row_major : 1, buffer_block : 1, precise : 1, invariant : 1;
};
struct reference {
    uint32_t root, type;
    uint32_t descriptor;
    int offset, dynamic, stride, count, component_stride, matrix_stride, row_major;
};
struct id {
    const uint32_t *ins;
    uint32_t type, function;
    int reg, literal_valid;
    uint32_t literal;
    struct decoration d, *members;
    struct reference ref;
    uint64_t used;
    int entry_interface;
    int io[64], semantic[64];
    int sampler, slot, sampler_slot, processing, accessed, image_ms, image_kind;
    unsigned active,reachable,indirect;
};
struct compiler {
    const uint32_t *code;
    uint32_t *expanded;
    size_t words;
    uint32_t bound, entry;
    VkShaderStageFlagBits stage;
    struct id *ids;
    zvk_id_map map;
    struct text body, prolog, epilog, declarations, output;
    struct text *out;
    uint32_t imm[MAX_IMM][4];
    int nimms, temps, instruction, nin, nout, nsv, fragcoord;
    int precise;
    int bias_depth, writes_depth;
    int color_io[4];
    void *flow_alloc[17];
    zvk_shader *shader;
    jmp_buf error;
};
/* Read-only descriptor buffers use the integer atlas. Keep their resource
 * classification shared for descriptor/layout accounting; stores still reject
 * these roots in the graphics frontend. */
static int constant_binding(VkDescriptorType type)
{
    return type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
}
static _Noreturn void fail(struct compiler *c, const char *fmt, ...)
{
    char message[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(message, sizeof message, fmt, ap);
    va_end(ap);
    zvk_error("SPIR-V: %s", message);
    if (getenv("ZVK_DEBUG_SHADERS"))
        fprintf(stderr, "%s\n", zvkGetLastError());
    longjmp(c->error, 1);
}
static void textv(struct compiler *c, struct text *s, const char *fmt, va_list ap)
{
    va_list aq;
    va_copy(aq, ap);
    int n = vsnprintf(NULL, 0, fmt, aq);
    va_end(aq);
    if (n < 0 || (size_t)n > SIZE_MAX - s->size - 1)
        fail(c, "text size overflow");
    size_t need = s->size + (size_t)n + 1;
    if (need > s->capacity) {
        size_t cap = need * 2;
        char *p = realloc(s->data, cap);
        if (!p)
            fail(c, "out of memory");
        s->data = p;
        s->capacity = cap;
    }
    vsnprintf(s->data + s->size, s->capacity - s->size, fmt, ap);
    s->size += (size_t)n;
}
static void textf(struct compiler *c, struct text *s, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    textv(c, s, fmt, ap);
    va_end(ap);
}
static void emit(struct compiler *c, const char *fmt, ...)
{
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    int length = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (length < 0 || (size_t)length >= sizeof line)
        fail(c, "instruction text too long");
    char *space = strchr(line, ' ');
    if (c->precise && space && !strncmp(space + 1, "TEMP[", 5))
        textf(c, c->out, "%d: %.*s_PRECISE%s\n", c->instruction++, (int)(space - line), line, space);
    else
        textf(c, c->out, "%d: %s\n", c->instruction++, line);
}
static struct id *lookup(struct compiler *c,uint32_t id)
{
    uint32_t index=zvk_spirv_id(&c->map,id);
    if (index==UINT32_MAX)fail(c,"undefined id %u",id);
    return c->ids+index;
}
static struct id *get(struct compiler *c,uint32_t id)
{
    struct id *n=lookup(c,id);
    if (!n->ins)fail(c,"id %u is not yet defined",id);
    return n;
}
static SpvOp opcode(const uint32_t *p)
{
    return (SpvOp)(p[0] & 65535);
}
static int wc(const uint32_t *p)
{
    return (int)(p[0] >> 16);
}
static void minimum(struct compiler *c, const uint32_t *p, int n)
{
    if (wc(p) < n)
        fail(c, "truncated %s", SpvOpToString(opcode(p)));
}
static int temporary(struct compiler *c, int n)
{
    if (n < 0 || n > MAX_TEMPS - c->temps)
        fail(c, "temporary register limit (%d, after %d GPU instructions)", MAX_TEMPS,c->instruction);
    int reg = c->temps;
    c->temps += n;
    return reg;
}
static int imm(struct compiler *c, uint32_t a, uint32_t b, uint32_t d, uint32_t e)
{
    uint32_t words[4] = {a, b, d, e};
    for (int i = 0; i < c->nimms; i++)
        if (!memcmp(c->imm[i], words, 16))
            return i;
    if (c->nimms == MAX_IMM)
        fail(c, "immediate limit");
    int i = c->nimms++;
    memcpy(c->imm[i], words, 16);
    return i;
}
static int scalar_imm(struct compiler *c, uint32_t x)
{
    return imm(c, x, x, x, x);
}
static uint32_t bits(float x)
{
    uint32_t u;
    memcpy(&u, &x, 4);
    return u;
}
static struct decoration decoration(void)
{
    struct decoration d;
    memset(&d, 0, sizeof d);
    d.location = d.builtin = d.binding = d.set = d.offset = d.stride = d.matrix_stride = d.spec =
        d.input_attachment = -1;
    return d;
}
static uint32_t constant(struct compiler *c, uint32_t id)
{
    struct id *n = get(c, id);
    if (!n->literal_valid)
        fail(c, "id %u is not a scalar constant", id);
    return n->literal;
}
static int registers_r(struct compiler *c, uint32_t type, int depth)
{
    if (depth > 32)
        fail(c, "recursive or deeply nested type");
    const uint32_t *p = get(c, type)->ins;
    SpvOp op = opcode(p);
    switch (op) {
    case SpvOpTypeVoid:
    case SpvOpTypePointer:
    case SpvOpTypeImage:
    case SpvOpTypeSampler:
    case SpvOpTypeSampledImage:
        return 0;
    case SpvOpTypeBool:
        return 1;
    case SpvOpTypeInt:
    case SpvOpTypeFloat:
        minimum(c, p, 3);
        if (p[2] != 32)
            fail(c, "only 32-bit scalar types are supported");
        return 1;
    case SpvOpTypeVector:
        minimum(c, p, 4);
        if (p[3] < 2 || p[3] > 4)
            fail(c, "invalid vector width");
        return 1;
    case SpvOpTypeMatrix:
        minimum(c, p, 4);
        if (p[3] < 2 || p[3] > 4)
            fail(c, "invalid matrix width");
        return (int)p[3];
    case SpvOpTypeArray: {
        minimum(c, p, 4);
        uint32_t count = constant(c, p[3]);
        if (!count || count > 256)
            fail(c, "array length outside 1..256");
        return registers_r(c, p[2], depth + 1) * (int)count;
    }
    case SpvOpTypeStruct: {
        int n = 0;
        if (wc(p) - 2 > MAX_MEMBERS)
            fail(c, "too many struct members");
        for (int i = 2; i < wc(p); i++)
            n += registers_r(c, p[i], depth + 1);
        return n;
    }
    default:
        fail(c, "unsupported value type %s", SpvOpToString(op));
        return 0;
    }
}
static int registers(struct compiler *c, uint32_t type)
{
    int n = registers_r(c, type, 0);
    if (n > 1024)
        fail(c, "aggregate type is too large");
    return n;
}
static int width(struct compiler *c, uint32_t type)
{
    const uint32_t *p = get(c, type)->ins;
    return opcode(p) == SpvOpTypeVector ? (int)p[3] : 1;
}
static int field_regs(struct compiler *c, uint32_t type, uint32_t index)
{
    const uint32_t *p = get(c, type)->ins;
    int off = 0;
    if (index >= (uint32_t)(wc(p) - 2))
        fail(c, "struct member out of range");
    for (uint32_t i = 0; i < index; i++)
        off += registers(c, p[2 + i]);
    return off;
}
static const char *mask(int n)
{
    return n == 1 ? "x" : n == 2 ? "xy" : n == 3 ? "xyz" : "xyzw";
}
static void source(struct compiler *c, uint32_t id, int reg, char out[64])
{
    struct id *n = get(c, id);
    if (n->reg < 0 || reg >= registers(c, n->type))
        fail(c, "id %u has no scalar/vector storage", id);
    snprintf(out, 64, "TEMP[%d].%s", n->reg + reg,
             registers(c, n->type) == 1 && width(c, n->type) == 1 ? "xxxx" : "xyzw");
}
static void copy(struct compiler *c, int dst, int src, uint32_t type)
{
    for (int i = 0; i < registers(c, type); i++)
        emit(c, "MOV TEMP[%d], TEMP[%d]", dst + i, src + i);
}
static void unary(struct compiler *c, const char *op, uint32_t result, uint32_t arg)
{
    struct id *r = get(c, result);
    char a[64];
    source(c, arg, 0, a);
    emit(c, "%s TEMP[%d].%s, %s", op, r->reg, mask(width(c, r->type)), a);
}
static void binary(struct compiler *c, const char *op, uint32_t result, uint32_t a, uint32_t b)
{
    struct id *r = get(c, result);
    char sa[64], sb[64];
    source(c, a, 0, sa);
    source(c, b, 0, sb);
    if (!strcmp(op,"IDIV") || !strcmp(op,"UDIV") || !strcmp(op,"MOD") || !strcmp(op,"UMOD")) {
        int valid=temporary(c,1),safe=temporary(c,1);
        emit(c,"USNE TEMP[%d], %s, IMM[%d]",valid,sb,scalar_imm(c,0));
        emit(c,"UCMP TEMP[%d], TEMP[%d], %s, IMM[%d]",safe,valid,sb,scalar_imm(c,1));
        if (!strcmp(op,"IDIV") || !strcmp(op,"MOD")) {
            int first=temporary(c,1),second=temporary(c,1);
            emit(c,"USEQ TEMP[%d], %s, IMM[%d]",first,sa,scalar_imm(c,0x80000000u));
            emit(c,"USEQ TEMP[%d], %s, IMM[%d]",second,sb,scalar_imm(c,UINT32_MAX));
            emit(c,"AND TEMP[%d], TEMP[%d], TEMP[%d]",first,first,second);
            emit(c,"UCMP TEMP[%d], TEMP[%d], IMM[%d], TEMP[%d]",safe,first,scalar_imm(c,1),safe);
        }
        snprintf(sb,sizeof sb,"TEMP[%d]",safe);
    }
    emit(c, "%s TEMP[%d].%s, %s, %s", op, r->reg, mask(width(c, r->type)), sa, sb);
}

VkResult zvk_shader_check(const uint32_t *code, size_t words)
{
    if (!code || words < 5 || words > 262144 || code[0] != SpvMagicNumber || code[1] < 0x00010000 ||
        code[1] > 0x00010300 || !code[3] || code[3] > MAX_IDS || code[4]) {
        zvk_error("SPIR-V: invalid header, version, schema, or id bound");
        return VK_ERROR_INVALID_SHADER_NV;
    }
    for (size_t i = 5; i < words;) {
        uint32_t n = code[i] >> 16;
        if (!n || n > words - i) {
            zvk_error("SPIR-V: truncated instruction at word %zu", i);
            return VK_ERROR_INVALID_SHADER_NV;
        }
        uint32_t op = code[i] & 65535;
        if (op >= ZVK_ARRAY_SIZE(spv_min_words) || !spv_min_words[op] || n < spv_min_words[op]) {
            zvk_error("SPIR-V: invalid opcode or operand count at word %zu", i);
            return VK_ERROR_INVALID_SHADER_NV;
        }
        i += n;
    }
    return VK_SUCCESS;
}
static void apply_decoration(struct compiler *c, struct decoration *d, const uint32_t *p, int start)
{
    SpvDecoration kind = (SpvDecoration)p[start];
    switch (kind) {
    case SpvDecorationLocation:
    case SpvDecorationComponent:
    case SpvDecorationBuiltIn:
    case SpvDecorationBinding:
    case SpvDecorationDescriptorSet:
    case SpvDecorationOffset:
    case SpvDecorationArrayStride:
    case SpvDecorationMatrixStride:
    case SpvDecorationInputAttachmentIndex:
    case SpvDecorationSpecId:
        minimum(c, p, start + 2);
        if (p[start + 1]>INT_MAX && kind!=SpvDecorationBinding && kind!=SpvDecorationSpecId)
            fail(c, "decoration value too large");
        switch (kind) {
        case SpvDecorationLocation:
            d->location = (int)p[start + 1];
            break;
        case SpvDecorationComponent:
            if (p[start + 1] > 3)
                fail(c,"interface component out of range");
            d->component=p[start+1];
            break;
        case SpvDecorationBuiltIn:
            d->builtin = (int)p[start + 1];
            break;
        case SpvDecorationBinding:
            d->binding=p[start+1];
            break;
        case SpvDecorationDescriptorSet:
            d->set = (int)p[start + 1];
            break;
        case SpvDecorationOffset:
            d->offset = (int)p[start + 1];
            break;
        case SpvDecorationArrayStride:
            d->stride = (int)p[start + 1];
            break;
        case SpvDecorationMatrixStride:
            d->matrix_stride = (int)p[start + 1];
            break;
        case SpvDecorationInputAttachmentIndex:
            d->input_attachment = (int)p[start + 1];
            break;
        case SpvDecorationSpecId:
            d->spec=p[start+1];
            break;
        default:
            break;
        }
        break;
    case SpvDecorationInvariant:
        d->invariant=1;
        break;
    case SpvDecorationFlat:
        d->flat = 1;
        break;
    case SpvDecorationNoPerspective:
        d->linear = 1;
        break;
    case SpvDecorationCentroid:
        d->centroid = 1;
        break;
    case SpvDecorationRowMajor:
        d->row_major = 1;
        break;
    case SpvDecorationBufferBlock:
        d->buffer_block = 1;
        break;
    case SpvDecorationNoContraction:
        d->precise = 1;
        break;
    case SpvDecorationColMajor:
    case SpvDecorationBlock:
    case SpvDecorationRelaxedPrecision:
    case SpvDecorationNonWritable:
    case SpvDecorationNonReadable:
    case SpvDecorationRestrict:
    case SpvDecorationAliased:
        break;
    default:
        fail(c, "unsupported decoration %u", (unsigned)kind);
    }
}
#include "graphics_constants.h"
static void active_variables(struct compiler *c)
{
    get(c,c->entry)->reachable=1;
    int changed;
    do {
        changed=0;
        for (size_t i=5;i<c->words;i+=wc(c->code+i)) {
            const uint32_t *p=c->code+i;
            if (opcode(p)!=SpvOpFunctionCall)continue;
            struct id *instruction=get(c,p[2]);
            if (instruction->function && get(c,instruction->function)->reachable &&
                !get(c,p[3])->reachable) {get(c,p[3])->reachable=1;changed=1;}
        }
    } while (changed);
    uint32_t function=0;
    for (size_t i=5;i<c->words;i+=wc(c->code+i)) {
        const uint32_t *p=c->code+i;SpvOp op=opcode(p);
        if (op==SpvOpFunction)function=p[2];
        if (op==SpvOpFunctionEnd)function=0;
        if (!function || !get(c,function)->reachable)continue;
        unsigned first=0,last=0;
        if (op==SpvOpLoad || op==SpvOpAccessChain || op==SpvOpInBoundsAccessChain ||
            op==SpvOpCopyObject || op==SpvOpArrayLength)first=3,last=4;
        else if (op==SpvOpStore)first=1,last=2;
        else if (op==SpvOpCopyMemory || op==SpvOpCopyMemorySized)first=1,last=3;
        else if (op==SpvOpFunctionCall)first=4,last=wc(p);
        for (unsigned j=first;j<last;j++) {
            struct id *n=get(c,p[j]);
            if (opcode(n->ins)==SpvOpVariable && !n->function)n->active=1;
        }
    }
}
static void parse(struct compiler *c, const char *entry, const VkSpecializationInfo *spec)
{
    uint32_t function = 0;
    for (size_t i = 5; i < c->words; i += (c->code[i] >> 16)) {
        const uint32_t *p = c->code + i;
        SpvOp op = opcode(p);
        bool has_result, has_type;
        SpvHasResultAndType(op, &has_result, &has_type);
        uint32_t id = 0;
        if (has_result) {
            minimum(c, p, has_type ? 3 : 2);
            id = p[has_type ? 2 : 1];
            if (!id || id >= c->bound || (*lookup(c,id)).ins)
                fail(c, "duplicate/out-of-range result id %u", id);
            (*lookup(c,id)).ins = p;
            (*lookup(c,id)).type = has_type ? p[1] : 0;
            (*lookup(c,id)).function = function;
        }
        if (op == SpvOpFunction) {
            if (function)
                fail(c, "nested function declaration");
            function = id;
            (*lookup(c,id)).function = id;
        }
        if (op == SpvOpFunctionEnd) {
            if (!function)
                fail(c, "function end without a function");
            function = 0;
        }
        if (op == SpvOpConstant || op == SpvOpSpecConstant) {
            minimum(c, p, 4);
            (*lookup(c,id)).literal_valid = 1;
            (*lookup(c,id)).literal = p[3];
        }
        if (op == SpvOpConstantTrue || op == SpvOpSpecConstantTrue || op == SpvOpConstantFalse ||
            op == SpvOpSpecConstantFalse) {
            (*lookup(c,id)).literal_valid = 1;
            (*lookup(c,id)).literal =
                (op == SpvOpConstantTrue || op == SpvOpSpecConstantTrue) ? UINT32_MAX : 0;
        }
        if (op == SpvOpEntryPoint) {
            minimum(c, p, 4);
            const char *name = (const char *)(p + 3);
            size_t max = (size_t)(wc(p) - 3) * 4;
            if (!memchr(name, 0, max))
                fail(c, "unterminated entry-point name");
            if (!strcmp(name, entry) &&
                ((c->stage == VK_SHADER_STAGE_VERTEX_BIT && p[1] == SpvExecutionModelVertex) ||
                 (c->stage == VK_SHADER_STAGE_FRAGMENT_BIT && p[1] == SpvExecutionModelFragment))) {
                c->entry = p[2];
                size_t first = 3 + (strlen(name) + 4) / 4;
                for (size_t j=first;j<(size_t)wc(p);j++) {
                    if (!p[j] || p[j]>=c->bound)
                        fail(c,"entry interface id out of range");
                    lookup(c,p[j])->entry_interface=1;
                }
            }
        }
        if (op == SpvOpMemoryModel) {
            minimum(c, p, 3);
            if (p[1] != SpvAddressingModelLogical || p[2] != SpvMemoryModelGLSL450)
                fail(c, "only Logical GLSL450 memory model is supported");
        }
        if (op == SpvOpCapability) {
            minimum(c, p, 2);
            if (p[1] != SpvCapabilityShader && p[1] != SpvCapabilityMatrix &&
                p[1] != SpvCapabilityInputAttachment && p[1] != SpvCapabilitySampled1D &&
                p[1] != SpvCapabilityImageMSArray && p[1] != SpvCapabilityImageQuery &&
                p[1] != SpvCapabilitySampleRateShading && p[1] != SpvCapabilitySampledBuffer &&
                p[1] != SpvCapabilityImageBuffer && p[1] != SpvCapabilityDerivativeControl)
                fail(c, "unsupported capability %u", p[1]);
        }
    }
    if (function)
        fail(c, "unterminated function");
    if (!c->entry)
        fail(c, "entry point '%s' not found for requested stage", entry);
    for (size_t i = 5; i < c->words; i += (c->code[i] >> 16)) {
        const uint32_t *p = c->code + i;
        SpvOp op = opcode(p);
        if (op == SpvOpExecutionMode && p[1] == c->entry) {
            switch ((SpvExecutionMode)p[2]) {
            case SpvExecutionModeEarlyFragmentTests:
                if(c->stage!=VK_SHADER_STAGE_FRAGMENT_BIT)fail(c,"early tests outside fragment stage");
                c->shader->early_fragment_tests=1;
                break;
            case SpvExecutionModeOriginUpperLeft:
            case SpvExecutionModeDepthReplacing:
            case SpvExecutionModeDepthGreater:
            case SpvExecutionModeDepthLess:
            case SpvExecutionModeDepthUnchanged:
                break;
            default:
                fail(c, "unsupported execution mode %s",
                     SpvExecutionModeToString((SpvExecutionMode)p[2]));
            }
        }
        if (op == SpvOpDecorate) {
            minimum(c, p, 3);
            apply_decoration(c, &get(c, p[1])->d, p, 2);
        }
        if (op == SpvOpMemberDecorate) {
            minimum(c, p, 4);
            struct id *n = get(c, p[1]);
            if (p[2] >= MAX_MEMBERS)
                fail(c, "member decoration out of range");
            if (!n->members) {
                n->members = malloc(MAX_MEMBERS * sizeof *n->members);
                if (!n->members)
                    fail(c, "out of memory");
                for (int j = 0; j < MAX_MEMBERS; j++)
                    n->members[j] = decoration();
            }
            apply_decoration(c, &n->members[p[2]], p, 3);
        }
    }
    if (spec)
        for (uint32_t k = 0; k < spec->mapEntryCount; k++) {
            const VkSpecializationMapEntry *m = spec->pMapEntries + k;
            if (m->size != 4 || m->offset > spec->dataSize || m->size > spec->dataSize - m->offset)
                fail(c, "invalid specialization data range");
            for (uint32_t dense=0;dense<c->map.count;dense++) {
                uint32_t id=c->map.ids[dense];
                if ((*lookup(c,id)).ins && (*lookup(c,id)).d.spec == (int64_t)m->constantID) {
                    if (!(*lookup(c,id)).literal_valid)
                        fail(c, "only scalar specialization constants are supported");
                    memcpy(&(*lookup(c,id)).literal, (const uint8_t *)spec->pData + m->offset, 4);
                    if (opcode(get(c, (*lookup(c,id)).type)->ins) == SpvOpTypeBool)
                        (*lookup(c,id)).literal = (*lookup(c,id)).literal ? UINT32_MAX : 0;
                }
        }
    }
    for (size_t word = 5; word < c->words; word += wc(c->code + word))
        if (opcode(c->code + word) == SpvOpSpecConstantOp)
            fold_specialization(c, c->code + word);
    active_variables(c);
    /* A byte atlas removes the host's constant-block size and binding limits
     * for read-only storage buffers. Uniform buffers in the same stage share
     * the atlas, so simultaneous UBO/SSBO counts do not compete for GL slots. */
    if (c->stage==VK_SHADER_STAGE_VERTEX_BIT) {
        unsigned attributes=0,system=0;
        for (uint32_t i=0;i<c->map.count;i++) {
            struct id *n=c->ids+i;
            if (!n->ins || opcode(n->ins)!=SpvOpVariable || n->ins[3]!=SpvStorageClassInput || !n->active)continue;
            if (n->d.builtin<0)attributes+=registers(c,get(c,n->type)->ins[3]);
            else if (n->d.builtin==SpvBuiltInVertexIndex || n->d.builtin==SpvBuiltInInstanceIndex ||
                     n->d.builtin==SpvBuiltInVertexId || n->d.builtin==SpvBuiltInInstanceId)system++;
        }
        if (attributes+system>ZVK_ATTRS)c->shader->vertex_pulling=c->shader->resource_atlas=1;
    }
    /* Prefer native texture instructions for simple discard shaders. The
     * Apple GL compiler miscompiles some kills fed by the packed sampler's
     * helper loops. Keep explicit LOD, buffers, separate samplers and complex
     * image operations on the atlas path with its Vulkan-specific semantics. */
    int native_implicit=1,has_discard=0;
    uint32_t image_function=0;
    for(size_t i=5;i<c->words;i+=wc(c->code+i)) {
        const uint32_t *p=c->code+i;SpvOp op=opcode(p);
        if(op==SpvOpFunction)image_function=p[2];
        if(op==SpvOpFunctionEnd)image_function=0;
        if(!image_function || !get(c,image_function)->reachable)continue;
        if(op==SpvOpKill)has_discard=1;
        if(op>=SpvOpSampledImage && op<=SpvOpImageQuerySamples &&
           !((op==SpvOpImageSampleImplicitLod || op==SpvOpImageSampleProjImplicitLod) && wc(p)==5))
            native_implicit=0;
    }
    native_implicit&=has_discard;
    unsigned atlas_images=0;
    for (uint32_t dense=0;dense<c->map.count;dense++) {
        struct id *n=c->ids+dense;
        if (!n->ins || opcode(n->ins)!=SpvOpVariable || !n->active)continue;
        if (n->ins[3]==SpvStorageClassStorageBuffer)c->shader->resource_atlas=1;
        if (n->ins[3]==SpvStorageClassUniform)c->shader->resource_atlas=1;
        if (n->ins[3]==SpvStorageClassUniformConstant) {
            if(!native_implicit)c->shader->resource_atlas=1;
            uint32_t type=get(c,n->type)->ins[3],count=1;
            if (opcode(get(c,type)->ins)==SpvOpTypeArray) {
                count=constant(c,get(c,type)->ins[3]);type=get(c,type)->ins[2];
            }
            const uint32_t *t=get(c,type)->ins;
            if (opcode(t)==SpvOpTypeSampler)c->shader->resource_atlas=1;
            else {
                atlas_images+=count;
                int combined=opcode(t)==SpvOpTypeSampledImage;
                if (combined)t=get(c,t[2])->ins;
                if(!combined || opcode(t)!=SpvOpTypeImage || t[3]!=SpvDim2D || t[4]==1 ||
                   t[5] || t[6] || t[7]!=1 || opcode(get(c,t[2])->ins)!=SpvOpTypeFloat)
                    c->shader->resource_atlas=1;
                if (opcode(t)==SpvOpTypeImage && t[7]==2)c->shader->resource_atlas=1;
                /* Apple GL lacks EXT_texture_shadow_lod for array/cube
                 * comparisons. Vulkan's explicit/vertex LOD forms remain
                 * available through our GPU sampler. */
                if (opcode(t)==SpvOpTypeImage && t[4]==1 && (t[5] || t[3]==SpvDimCube))
                    c->shader->resource_atlas=1;
            }
        }
    }
    if (atlas_images>ZVK_SAMPLERS)c->shader->resource_atlas=1;
    for (uint32_t dense=0;dense<c->map.count;dense++) {
        uint32_t id=c->map.ids[dense];
        struct id *n = &(*lookup(c,id));
        if (!n->ins || !n->type)
            continue;
        if (n->function && !get(c,n->function)->reachable)continue;
        if (opcode(n->ins)==SpvOpVariable && !n->active &&
            (n->ins[3]==SpvStorageClassUniform || n->ins[3]==SpvStorageClassUniformConstant ||
             n->ins[3]==SpvStorageClassStorageBuffer))continue;
        get(c, n->type);
        if (opcode(n->ins)==SpvOpUndef && opcode(get(c,n->type)->ins)==SpvOpTypeRuntimeArray)continue;
        n->reg = temporary(c, registers(c, n->type));
        if (!registers(c, n->type))
            n->reg = -1;
        if (opcode(n->ins) == SpvOpVariable) {
            const uint32_t *type = get(c, n->type)->ins;
            minimum(c, type, 4);
            if (opcode(type) != SpvOpTypePointer)
                fail(c, "variable has non-pointer type");
            n->ref = (struct reference){.root = id,
                                        .type = type[3],
                                        .dynamic = -1,
                                        .component_stride = 1,
                                        .matrix_stride = 4};
            if (n->ins[3] != SpvStorageClassUniform && n->ins[3] != SpvStorageClassStorageBuffer &&
                n->ins[3] != SpvStorageClassPushConstant &&
                n->ins[3] != SpvStorageClassUniformConstant)
                n->reg = temporary(c, registers(c, type[3]));
            if (n->ins[3] == SpvStorageClassUniform || n->ins[3] == SpvStorageClassStorageBuffer ||
                n->ins[3] == SpvStorageClassUniformConstant) {
                if (n->d.set < 0 || n->d.set >= ZVK_SETS || n->d.binding < 0)
                    fail(c, "descriptor set/binding missing or out of range");
                int sampler = n->ins[3] == SpvStorageClassUniformConstant;
                uint32_t element_type = type[3], count = 1, target = 0, image_kind = 2;
                const uint32_t *array = get(c, element_type)->ins;
                if (opcode(array) == SpvOpTypeArray) {
                    count = constant(c, array[3]);
                    element_type = array[2];
                }
                if (!count || count > 16)
                    fail(c, "descriptor array exceeds 16 elements");
                VkDescriptorType descriptor_type = n->ins[3] == SpvStorageClassStorageBuffer ||
                                                           get(c, element_type)->d.buffer_block
                                                       ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
                                                       : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                if (sampler) {
                    const uint32_t *sampled = get(c, element_type)->ins;
                    const uint32_t *image;
                    if (opcode(sampled) == SpvOpTypeSampler) {
                        if (c->shader->resource_atlas) {
                            if (c->shader->nsamplers+count>ZVK_SAMPLERS)fail(c,"separate sampler limit");
                            n->slot=ZVK_RESOURCES+c->shader->nsamplers;
                            for (unsigned k=0;k<count;k++)
                                c->shader->samplers[c->shader->nsamplers++]=(zvk_shader_binding){
                                    .set=n->d.set,.binding=n->d.binding,.element=k,.slot=n->slot+k,
                                    .type=VK_DESCRIPTOR_TYPE_SAMPLER};
                        }
                        continue;
                    }
                    if (opcode(sampled) == SpvOpTypeImage && sampled[3] != SpvDimSubpassData) {
                        image = sampled;
                        descriptor_type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
                    } else if (opcode(sampled) == SpvOpTypeImage &&
                               sampled[3] == SpvDimSubpassData) {
                        image = sampled;
                        descriptor_type = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
                        if (c->stage != VK_SHADER_STAGE_FRAGMENT_BIT || n->d.input_attachment < 0 ||
                            n->d.input_attachment + count > ZVK_ATTACHMENTS)
                            fail(c, "input attachment index missing or out of range");
                    } else if (opcode(sampled) == SpvOpTypeSampledImage) {
                        image = get(c, sampled[2])->ins;
                        descriptor_type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                    } else
                        fail(c, "unsupported image descriptor type");
                    minimum(c, image, 9);
                    if (image[3] == SpvDimBuffer)
                        descriptor_type = image[7] == 2 ? VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER
                                                        : VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
                    else if (image[7]==2 && descriptor_type==VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE)
                        descriptor_type=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                    const uint32_t *scalar = get(c, image[2])->ins;
                    if (opcode(scalar) == SpvOpTypeInt)
                        image_kind = scalar[3] ? 1 : 0;
                    else if (opcode(scalar) != SpvOpTypeFloat)
                        fail(c, "image scalar type");
                    if (opcode(image) != SpvOpTypeImage ||
                        (image[6] && image[3] != SpvDim2D && image[3] != SpvDimSubpassData) ||
                        image[7] !=
                            (descriptor_type == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT ||
                                     descriptor_type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ||
                                     descriptor_type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER ? 2u : 1u))
                        fail(c, "unsupported sampled image type");
                    n->image_ms = !!image[6];
                    if (image[3] == SpvDimBuffer && !image[5] && !image[6])
                        target = ZVK_BUFFER_SAMPLER;
                    else if (image[3] == SpvDim1D)
                        target = image[5] ? PIPE_TEXTURE_1D_ARRAY : PIPE_TEXTURE_1D;
                    else if (image[3] == SpvDim3D && !image[5])
                        target = PIPE_TEXTURE_3D;
                    else if (image[3] == SpvDim2D || image[3] == SpvDimSubpassData)
                        target = image[5] ? PIPE_TEXTURE_2D_ARRAY : PIPE_TEXTURE_2D;
                    else if (image[3] == SpvDimCube && !image[5])
                        target = PIPE_TEXTURE_CUBE;
                    else
                        fail(c, "only 2D, 2D array, cube and subpass images are supported");
                }
                uint32_t slot = 0;
                for (uint32_t j = 0; j < c->shader->nbindings; j++)
                    if ((!constant_binding(c->shader->bindings[j].type)) == sampler)
                        slot++;
                if (c->shader->nbindings+count>ZVK_RESOURCES ||
                    (!c->shader->resource_atlas && slot + count > (sampler ? ZVK_SAMPLERS : ZVK_UBOS)))
                    fail(c, "too many shader resources");
                n->slot = (c->shader->resource_atlas?(int)c->shader->nbindings:(int)slot) + (sampler ? 0 : 1);
                n->sampler_slot=n->slot;
                n->sampler = target;
                n->image_kind = image_kind;
                for (uint32_t element = 0; element < count; element++)
                    c->shader->bindings[c->shader->nbindings++] = (zvk_shader_binding){
                        .set = (uint32_t)n->d.set,
                        .binding = (uint32_t)n->d.binding,
                        .element = element,
                        .slot = (uint32_t)n->slot + element,
                        .bytes = 0,
                        .target = target,
                        .image_kind = image_kind,
                        .image_ms = n->image_ms,
                        .type = descriptor_type,
                        .input_attachment = descriptor_type == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT
                                                ? (uint32_t)n->d.input_attachment + element
                                                : UINT32_MAX};
            }
        }
    }
}
static void mark(struct compiler *c, struct reference r, int regs)
{
    struct id *root = get(c, r.root);
    if (opcode(root->ins)!=SpvOpVariable ||
        (root->ins[3]!=SpvStorageClassInput && root->ins[3]!=SpvStorageClassOutput))return;
    int start = r.offset / 4;
    if (start < 0 || start + regs > 64)
        fail(c, "interface aggregate exceeds 64 registers");
    if (root->ins[3] == SpvStorageClassInput || root->ins[3] == SpvStorageClassOutput)
        for (int i = 0; i < regs; i++)
            root->used |= (uint64_t)1 << (start + i);
}
static struct reference pointer(struct compiler *c, uint32_t id)
{
    struct id *n = get(c, id);
    if (!n->ref.root)
        fail(c, "id %u is not a supported pointer", id);
    return n->ref;
}
static int atlas_root(struct compiler *c,struct id *root)
{
    return c->shader->resource_atlas && opcode(root->ins)==SpvOpVariable &&
        (root->ins[3]==SpvStorageClassUniform || root->ins[3]==SpvStorageClassStorageBuffer);
}
static void reference_offset(struct reference *r,uint64_t extra)
{
    if (r->offset==INT_MAX)return;
    if (extra>(uint64_t)(INT_MAX-r->offset))r->offset=INT_MAX;
    else r->offset+=(int)extra;
}
static struct reference child_ref(struct compiler *c, struct reference r, uint32_t index,
                                  int is_literal)
{
    struct id *type = get(c, r.type), *root = get(c, r.root);
    const uint32_t *p = type->ins;
    SpvOp op = opcode(p);
    int memory =
        opcode(root->ins) == SpvOpVariable &&
        (root->ins[3] == SpvStorageClassUniform || root->ins[3] == SpvStorageClassStorageBuffer ||
         root->ins[3] == SpvStorageClassPushConstant);
    uint32_t ix = is_literal ? index : 0;
    int dyn = -1;
    if (!is_literal) {
        struct id *n = get(c, index);
        if (n->literal_valid)
            ix = n->literal;
        else
            dyn = n->reg;
    }
    const uint32_t *root_pointer = get(c, root->type)->ins;
    if (op == SpvOpTypeArray && r.type == root_pointer[3] &&
        (root->ins[3] == SpvStorageClassUniform || root->ins[3] == SpvStorageClassStorageBuffer ||
         root->ins[3] == SpvStorageClassUniformConstant)) {
        if (dyn >= 0)
            fail(c, "dynamic descriptor array indexing is not supported");
        if (ix >= constant(c, p[3]))
            fail(c, "descriptor array index out of range");
        r.descriptor = ix;
        r.type = p[2];
        return r;
    }
    int count = 0, stride = 0;
    uint32_t element = 0;
    if (op == SpvOpTypeStruct) {
        if (dyn >= 0 || ix >= (uint32_t)(wc(p) - 2))
            fail(c, "struct index must be an in-range constant");
        element = p[2 + ix];
        if (memory) {
            if (!type->members || type->members[ix].offset < 0 || (type->members[ix].offset & 3))
                fail(c, "uniform member needs an aligned Offset decoration");
            int limit=atlas_root(c,root)?INT_MAX:16384;
            if (type->members[ix].offset > limit || type->members[ix].matrix_stride > limit ||
                (type->members[ix].matrix_stride > 0 && (type->members[ix].matrix_stride & 3)))
                fail(c, "uniform offset/stride out of range");
            reference_offset(&r,type->members[ix].offset/4);
            r.matrix_stride =
                type->members[ix].matrix_stride > 0 ? type->members[ix].matrix_stride / 4 : 4;
            r.row_major = type->members[ix].row_major;
        } else
            reference_offset(&r,(uint64_t)field_regs(c,r.type,ix)*4);
    } else {
        if (op == SpvOpTypeVector) {
            count = (int)p[3];
            stride = memory ? r.component_stride : 1;
            element = p[2];
        } else if (op == SpvOpTypeMatrix) {
            count = (int)p[3];
            stride = memory ? (r.row_major ? 1 : r.matrix_stride) : 4;
            element = p[2];
            if (memory)
                r.component_stride = r.row_major ? r.matrix_stride : 1;
        } else if (op == SpvOpTypeArray || (op==SpvOpTypeRuntimeArray && atlas_root(c,root))) {
            uint32_t length = op==SpvOpTypeRuntimeArray?1:constant(c, p[3]);
            if (!length || length > (atlas_root(c,root)?ZVK_MAX_ALLOC/4:256))
                fail(c, "array length outside 1..256");
            count = (int)length;
            stride =
                memory ? (type->d.stride > 0 ? type->d.stride / 4 : 0) : registers(c, p[2]) * 4;
            element = p[2];
            if (memory && ((type->d.stride > 16384 && !atlas_root(c,root)) || (type->d.stride & 3)))
                fail(c, "uniform array stride out of range");
            if (!stride)
                fail(c, "uniform array needs ArrayStride");
        } else
            fail(c, "indexing unsupported type %s", SpvOpToString(op));
        if (dyn >= 0) {
            if (count>256 && !atlas_root(c,root)) fail(c,"dynamic array index limit");
            if (r.dynamic>=0) {
                int a=r.stride,b=stride;
                while(b){int rest=a%b;a=b;b=rest;}
                int combined=temporary(c,1),part=temporary(c,1);
                emit(c,"UMUL TEMP[%d].x, TEMP[%d].xxxx, IMM[%d]",combined,r.dynamic,scalar_imm(c,r.stride/a));
                emit(c,"UMUL TEMP[%d].x, TEMP[%d].xxxx, IMM[%d]",part,dyn,scalar_imm(c,stride/a));
                emit(c,"UADD TEMP[%d].x, TEMP[%d].xxxx, TEMP[%d].xxxx",combined,combined,part);
                r.count=atlas_root(c,root)?1:(r.count-1)*(r.stride/a)+(count-1)*(stride/a)+1;
                r.stride=a;r.dynamic=combined;
            } else {
                r.dynamic=dyn;r.stride=stride;r.count=count;
            }
        } else {
            if (ix >= (uint32_t)count && op!=SpvOpTypeRuntimeArray) {
                if (is_literal || memory)fail(c,"constant index out of range");
                /* An out-of-range private AccessChain in dead control flow
                 * is legal to compile. Retain an invalid reference; never
                 * let it address another variable's temporary registers. */
                r.offset=INT_MAX;
            } else reference_offset(&r,(uint64_t)ix*stride);
        }
    }
    if (r.offset < 0 || (r.offset > 16384 && r.offset!=INT_MAX && !atlas_root(c,root)))
        fail(c, "pointer offset out of range");
    r.type = element;
    return r;
}
static void load_value(struct compiler *c, int dst, uint32_t type, struct reference r)
{
    if (r.offset==INT_MAX) {
        for (int i=0;i<registers(c,type);i++)emit(c,"MOV TEMP[%d], IMM[%d]",dst+i,scalar_imm(c,0));
        return;
    }
    struct id *root = get(c, r.root);
    const uint32_t *t = get(c, type)->ins;
    SpvOp op = opcode(t);
    root->accessed |= 1u << r.descriptor;
    if (op == SpvOpTypeArray || op == SpvOpTypeMatrix || op == SpvOpTypeStruct) {
        int count = op == SpvOpTypeArray    ? (int)constant(c, t[3])
                    : op == SpvOpTypeMatrix ? (int)t[3]
                                            : wc(t) - 2;
        int off = 0;
        for (int i = 0; i < count; i++) {
            struct reference child = child_ref(c, r, (uint32_t)i, 1);
            load_value(c, dst + off, child.type, child);
            off += registers(c, child.type);
        }
        return;
    }
    if (op == SpvOpTypeSampledImage)
        return;
    if (atlas_root(c,root)) {
        if (r.dynamic>=0) {
            emit(c,"UMUL TEMP[4].x, TEMP[%d].xxxx, IMM[%d]",r.dynamic,scalar_imm(c,r.stride));
            emit(c,"UADD TEMP[4].x, TEMP[4].xxxx, IMM[%d]",scalar_imm(c,r.offset));
        } else emit(c,"MOV TEMP[4].x, IMM[%d]",scalar_imm(c,r.offset));
        emit(c,"MOV TEMP[4].y, IMM[%d]",scalar_imm(c,r.component_stride));
        emit(c,"CAL : %u",0x5c0000u+root->slot+r.descriptor-1);
        emit(c,"MOV TEMP[%d].%s, TEMP[0]",dst,mask(width(c,type)));
        return;
    }
    if (r.dynamic>=0 && r.stride%4==0 && root->reg>=0 &&
        opcode(root->ins)==SpvOpVariable && root->ins[3]!=SpvStorageClassUniform &&
        root->ins[3]!=SpvStorageClassStorageBuffer && root->ins[3]!=SpvStorageClassPushConstant) {
        root->indirect=1;
        struct reference all=r;all.offset=0;mark(c,all,registers(c,root->ref.type));
        int index=temporary(c,1),condition=temporary(c,1);
        emit(c,"MOV TEMP[%d], IMM[%d]",dst,scalar_imm(c,0));
        emit(c,"USLT TEMP[%d].x, TEMP[%d].xxxx, IMM[%d]",condition,r.dynamic,scalar_imm(c,r.count));
        emit(c,"UIF TEMP[%d].xxxx",condition);
        emit(c,"UMUL TEMP[%d].x, TEMP[%d].xxxx, IMM[%d]",index,r.dynamic,scalar_imm(c,r.stride/4));
        emit(c,"UARL ADDR[0].x, TEMP[%d].xxxx",index);
        for (int i=0;i<width(c,type);i++) {
            int offset=r.offset+i;char lane="xyzw"[offset%4];
            emit(c,"MOV TEMP[%d].%c, TEMP[ADDR[0].x+%d].%c%c%c%c",dst,"xyzw"[i],
                 root->reg+offset/4,lane,lane,lane,lane);
        }
        emit(c,"ENDIF");return;
    }
    if (r.dynamic >= 0) {
        /* A descriptor access beyond the declared array must not retain a
         * value from an earlier loop iteration or invocation. */
        if (opcode(root->ins) == SpvOpVariable && (root->ins[3] == SpvStorageClassUniform ||
                                                   root->ins[3] == SpvStorageClassStorageBuffer))
            emit(c, "MOV TEMP[%d], IMM[%d]", dst, scalar_imm(c, 0));
        int condition = temporary(c, 1);
        for (int i = 0; i < r.count; i++) {
            emit(c, "USEQ TEMP[%d].x, TEMP[%d].xxxx, IMM[%d]", condition, r.dynamic,
                 scalar_imm(c, (uint32_t)i));
            emit(c, "UIF TEMP[%d].xxxx", condition);
            struct reference direct = r;
            direct.dynamic = -1;
            direct.offset += i * r.stride;
            load_value(c, dst, type, direct);
            emit(c, "ENDIF");
        }
        return;
    }
    int n = width(c, type), mem = root->ins[3] == SpvStorageClassUniform ||
                                  root->ins[3] == SpvStorageClassStorageBuffer ||
                                  root->ins[3] == SpvStorageClassPushConstant;
    if (!mem)
        mark(c, r, 1);
    for (int i = 0; i < n; i++) {
        int off = r.offset + i * (mem ? r.component_stride : 1);
        if (off < 0)
            fail(c, "negative memory offset");
        if (mem) {
            int slot =
                root->ins[3] == SpvStorageClassPushConstant ? 0 : root->slot + (int)r.descriptor;
            int max = slot ? 4096 : ZVK_PUSH / 4;
            if (off >= max)
                fail(c, "uniform/push-constant access out of range");
            if (!slot && (uint32_t)(off * 4 + 4) > c->shader->push_bytes)
                c->shader->push_bytes = (uint32_t)(off * 4 + 4);
            if (slot)
                for (uint32_t j = 0; j < c->shader->nbindings; j++) {
                    zvk_shader_binding *b = c->shader->bindings + j;
                    if (constant_binding(b->type) && b->slot == (uint32_t)slot &&
                        b->bytes < (uint32_t)(off * 4 + 4))
                        b->bytes = (uint32_t)(off * 4 + 4);
                }
            emit(c, "MOV TEMP[%d].%c, CONST[%d][%d].%c%c%c%c", dst, "xyzw"[i], slot, off / 4,
                 "xyzw"[off % 4], "xyzw"[off % 4], "xyzw"[off % 4], "xyzw"[off % 4]);
        } else {
            int count = registers(c, root->ref.type);
            if (off / 4 >= count || root->reg < 0)
                fail(c, "load outside variable");
            emit(c, "MOV TEMP[%d].%c, TEMP[%d].%c%c%c%c", dst, "xyzw"[i], root->reg + off / 4,
                 "xyzw"[off % 4], "xyzw"[off % 4], "xyzw"[off % 4], "xyzw"[off % 4]);
        }
    }
}
static void store_value(struct compiler *c, struct reference r, int src, uint32_t type)
{
    if (r.offset==INT_MAX)return;
    struct id *root = get(c, r.root);
    if (root->ins[3] != SpvStorageClassOutput && root->ins[3] != SpvStorageClassFunction &&
        root->ins[3] != SpvStorageClassPrivate)
        fail(c, "store into read-only/unsupported storage");
    const uint32_t *t = get(c, type)->ins;
    SpvOp op = opcode(t);
    if (op == SpvOpTypeArray || op == SpvOpTypeMatrix || op == SpvOpTypeStruct) {
        int count = op == SpvOpTypeArray    ? (int)constant(c, t[3])
                    : op == SpvOpTypeMatrix ? (int)t[3]
                                            : wc(t) - 2;
        int off = 0;
        for (int i = 0; i < count; i++) {
            struct reference child = child_ref(c, r, (uint32_t)i, 1);
            store_value(c, child, src + off, child.type);
            off += registers(c, child.type);
        }
        return;
    }
    if (r.dynamic>=0 && r.stride%4==0 && root->reg>=0) {
        root->indirect=1;
        struct reference all=r;all.offset=0;mark(c,all,registers(c,root->ref.type));
        int index=temporary(c,1),condition=temporary(c,1);
        emit(c,"USLT TEMP[%d].x, TEMP[%d].xxxx, IMM[%d]",condition,r.dynamic,scalar_imm(c,r.count));
        emit(c,"UIF TEMP[%d].xxxx",condition);
        emit(c,"UMUL TEMP[%d].x, TEMP[%d].xxxx, IMM[%d]",index,r.dynamic,scalar_imm(c,r.stride/4));
        emit(c,"UARL ADDR[0].x, TEMP[%d].xxxx",index);
        for (int i=0;i<width(c,type);i++) {
            int offset=r.offset+i;char lane="xyzw"[offset%4],value="xyzw"[i];
            emit(c,"MOV TEMP[ADDR[0].x+%d].%c, TEMP[%d].%c%c%c%c",root->reg+offset/4,
                 lane,src,value,value,value,value);
        }
        emit(c,"ENDIF");return;
    }
    if (r.dynamic >= 0) {
        int condition = temporary(c, 1);
        for (int i = 0; i < r.count; i++) {
            emit(c, "USEQ TEMP[%d].x, TEMP[%d].xxxx, IMM[%d]", condition, r.dynamic,
                 scalar_imm(c, (uint32_t)i));
            emit(c, "UIF TEMP[%d].xxxx", condition);
            struct reference direct = r;
            direct.dynamic = -1;
            direct.offset += i * r.stride;
            store_value(c, direct, src, type);
            emit(c, "ENDIF");
        }
        return;
    }
    mark(c, r, 1);
    for (int i = 0; i < width(c, type); i++) {
        int off = r.offset + i;
        if (off < 0 || off / 4 >= registers(c, root->ref.type))
            fail(c, "store outside variable");
        emit(c, "MOV TEMP[%d].%c, TEMP[%d].%c%c%c%c", root->reg + off / 4, "xyzw"[off % 4], src,
             "xyzw"[i], "xyzw"[i], "xyzw"[i], "xyzw"[i]);
    }
}
static void io_type(struct compiler *c, uint32_t root_id, uint32_t type, int offset,
                    struct decoration d)
{
    struct id *root = get(c, root_id);
    const uint32_t *p = get(c, type)->ins;
    SpvOp op = opcode(p);
    if (op == SpvOpTypeStruct || op == SpvOpTypeArray || op == SpvOpTypeMatrix) {
        int count = op == SpvOpTypeStruct  ? wc(p) - 2
                    : op == SpvOpTypeArray ? (int)constant(c, p[3])
                                           : (int)p[3],
            off = 0;
        for (int i = 0; i < count; i++) {
            uint32_t element = op == SpvOpTypeStruct ? p[2 + i] : p[2];
            struct decoration sub = d;
            if (op == SpvOpTypeStruct && get(c, type)->members) {
                struct decoration m = get(c, type)->members[i];
                if (m.builtin >= 0)
                    sub.builtin = m.builtin;
                if (m.location >= 0)
                    sub.location = m.location;
                sub.component = m.component;
                sub.flat |= m.flat;
                sub.linear |= m.linear;
                sub.centroid |= m.centroid;
                sub.invariant |= m.invariant;
            }
            if (d.location >= 0 && !(op == SpvOpTypeStruct && get(c, type)->members &&
                                     get(c, type)->members[i].location >= 0))
                sub.location = d.location + off;
            io_type(c, root_id, element, offset + off, sub);
            off += registers(c, element);
        }
        return;
    }
    if (offset >= 64)
        fail(c,"interface exceeds register limit");
    if (!(root->used & ((uint64_t)1 << offset)) &&
        !(root->entry_interface && root->ins[3] == SpvStorageClassOutput && d.builtin < 0))
        return;
    int input = root->ins[3] == SpvStorageClassInput, io = 0;
    if(!input && c->shader->early_fragment_tests) {
        if(d.builtin==SpvBuiltInFragDepth)return;
        if(d.builtin==SpvBuiltInSampleMask)fail(c,"early tests with shader coverage output are not supported");
    }
    if (d.builtin < 0 && input && c->stage == VK_SHADER_STAGE_VERTEX_BIT) {
        int lanes=width(c,type);
        if (d.location<0 || d.location>=ZVK_ATTRS || d.component+lanes>4)
            fail(c,"vertex input location/component range");
        if (c->shader->vertex_pulling) {
            c->shader->inputs|=1u<<d.location;
            c->out=&c->prolog;
            emit(c,"CAL : %u",0x5c0600u|(unsigned)d.location);
            for (int k=0;k<lanes;k++) {
                char lane="xyzw"[d.component+k];
                emit(c,"MOV TEMP[%d].%c, TEMP[0].%c%c%c%c",root->reg+offset,"xyzw"[k],lane,lane,lane,lane);
            }
            return;
        }
        if (c->shader->inputs & (1u<<d.location))
            io=c->shader->input_map[d.location];
        else {
            io=c->nin++;
            c->shader->inputs|=1u<<d.location;
            c->shader->input_map[d.location]=io;
            textf(c,&c->declarations,"DCL IN[%d]\n",io);
        }
        c->out=&c->prolog;
        for (int k=0;k<lanes;k++) {
            char lane="xyzw"[d.component+k];
            emit(c,"MOV TEMP[%d].%c, IN[%d].%c%c%c%c",root->reg+offset,"xyzw"[k],io,lane,lane,lane,lane);
        }
        return;
    }
    if (d.builtin < 0 && !input && c->stage == VK_SHADER_STAGE_FRAGMENT_BIT) {
        int lanes=width(c,type);
        if (d.location<0 || d.location>=4 || d.component+lanes>4)
            fail(c,"fragment output location/component range");
        io=c->color_io[d.location];
        if (io<0) {
            io=c->color_io[d.location]=c->nout++;
            textf(c,&c->declarations,"DCL OUT[%d], COLOR[%d]\n",io,d.location);
        }
        c->out=&c->epilog;
        for (int k=0;k<lanes;k++) {
            char lane="xyzw"[k];
            emit(c,"MOV OUT[%d].%c, TEMP[%d].%c%c%c%c",io,"xyzw"[d.component+k],root->reg+offset,lane,lane,lane,lane);
        }
        return;
    }
    if (d.builtin < 0 && ((input && c->stage == VK_SHADER_STAGE_FRAGMENT_BIT) ||
                          (!input && c->stage == VK_SHADER_STAGE_VERTEX_BIT))) {
        int lanes=width(c,type);
        if (d.location < 0 || d.location >= 16 || d.component + lanes > 4)
            fail(c,"varying location/component range");
        /* One scalar varying per Vulkan component gives both stages an exact
         * interface despite different vector grouping and interpolation. */
        c->shader->native_intrinsics=1;
        for (int k=0;k<lanes;k++) {
            if (d.location*4+d.component+k>=32)c->shader->wide_varyings=1;
            io=input?c->nin++:c->nout++;
            textf(c,&c->declarations,"DCL %s[%d].x, GENERIC[%d]",input?"IN":"OUT",io,
                  d.location*4+d.component+k);
            if (input)
                textf(c,&c->declarations,", %s%s",d.flat?"CONSTANT":d.linear?"LINEAR":"PERSPECTIVE",
                      d.centroid?", CENTROID":"");
            if (d.invariant)
                textf(c,&c->declarations,", INVARIANT");
            textf(c,&c->declarations,"\n");
            c->out=input?&c->prolog:&c->epilog;
            if (input)
                emit(c,"MOV TEMP[%d].%c, IN[%d].xxxx",root->reg+offset,"xyzw"[k],io);
            else
                emit(c,"MOV OUT[%d].x, TEMP[%d].%c%c%c%c",io,root->reg+offset,
                     "xyzw"[k],"xyzw"[k],"xyzw"[k],"xyzw"[k]);
        }
        return;
    }
    const char *semantic = "GENERIC", *file = input ? "IN" : "OUT";
    int semantic_index = d.location;
    if (d.builtin >= 0) {
        switch ((SpvBuiltIn)d.builtin) {
        case SpvBuiltInPosition:
            semantic = "POSITION";
            semantic_index = -1;
            break;
        case SpvBuiltInPointSize:
            semantic = "PSIZE";
            semantic_index = -1;
            break;
        case SpvBuiltInFragCoord:
            semantic = "POSITION";
            semantic_index = -1;
            break;
        case SpvBuiltInFragDepth:
            semantic = "POSITION";
            semantic_index = -1;
            break;
        case SpvBuiltInSampleId:
        case SpvBuiltInSamplePosition:
            if (c->stage != VK_SHADER_STAGE_FRAGMENT_BIT || !input)
                fail(c, "sample builtin requires a fragment input");
            semantic = d.builtin == SpvBuiltInSampleId ? "SAMPLEID" : "SAMPLEPOS";
            semantic_index = -1;
            file = "SV";
            break;
        case SpvBuiltInSampleMask:
            if (c->stage != VK_SHADER_STAGE_FRAGMENT_BIT || offset)
                fail(c, "sample mask exceeds the supported fragment mask word");
            semantic = "SAMPLEMASK";
            semantic_index = -1;
            if (input)
                file = "SV";
            break;
        case SpvBuiltInPointCoord:
            semantic="PCOORD";semantic_index=-1;
            break;
        case SpvBuiltInHelperInvocation:
            semantic="SAMPLEMASK";semantic_index=-1;file="SV";
            break;
        case SpvBuiltInFrontFacing:
            semantic = "FACE";
            semantic_index = -1;
            break;
        case SpvBuiltInVertexIndex:
        case SpvBuiltInVertexId:
            semantic = "VERTEXID";
            file = "SV";
            semantic_index = -1;
            break;
        case SpvBuiltInInstanceIndex:
        case SpvBuiltInInstanceId:
            semantic = "INSTANCEID";
            file = "SV";
            semantic_index = -1;
            break;
        default:
            fail(c, "unsupported used builtin %u", d.builtin);
        }
    }
    if (file[0] == 'S') {
        io = c->nsv++;
        textf(c, &c->declarations, "DCL SV[%d], %s\n", io, semantic);
    } else if (input && c->stage == VK_SHADER_STAGE_VERTEX_BIT) {
        if (d.location < 0 || d.location >= ZVK_ATTRS)
            fail(c, "vertex location missing/out of range");
        c->shader->inputs |= 1u << d.location;
        io = c->nin++;
        c->shader->input_map[d.location] = io;
        textf(c, &c->declarations, "DCL IN[%d]\n", io);
    } else {
        int reuse = input && d.builtin == SpvBuiltInFragCoord && c->fragcoord >= 0;
        if (reuse)
            io = c->fragcoord;
        else {
            io = input ? c->nin++ : c->nout++;
            textf(c, &c->declarations, "DCL %s[%d], %s", file, io,
                  (!input && c->stage == VK_SHADER_STAGE_FRAGMENT_BIT && d.builtin < 0) ? "COLOR"
                                                                                        : semantic);
            if (semantic_index >= 0)
                textf(c, &c->declarations, "[%d]", semantic_index);
            if (input && c->stage == VK_SHADER_STAGE_FRAGMENT_BIT)
                textf(c, &c->declarations, ", %s%s",
                      d.flat || d.builtin == SpvBuiltInFrontFacing   ? "CONSTANT"
                      : d.linear || d.builtin == SpvBuiltInFragCoord ? "LINEAR"
                                                                     : "PERSPECTIVE",
                      d.centroid ? ", CENTROID" : "");
            textf(c, &c->declarations, "\n");
        }
        if (input && d.builtin == SpvBuiltInFragCoord)
            c->fragcoord = io;
    }
    root->io[offset] = io;
    root->semantic[offset] = d.builtin;
    if (input) {
        c->out = &c->prolog;
        if (d.builtin==SpvBuiltInHelperInvocation)
            emit(c,"USEQ TEMP[%d].x, SV[%d].xxxx, IMM[%d]",root->reg+offset,io,scalar_imm(c,0));
        else if (d.builtin == SpvBuiltInFrontFacing)
            emit(c, "FSLT TEMP[%d].x, IMM[%d], IN[%d].xxxx", root->reg + offset, scalar_imm(c, 0),
                 io);
        else if (d.builtin == SpvBuiltInInstanceIndex || d.builtin == SpvBuiltInInstanceId) {
            c->shader->instance_index = 1;
            emit(c, "UADD TEMP[%d], SV[%d], CONST[0][%d].xxxx", root->reg + offset, io,
                 ZVK_PUSH / 16);
        } else
            emit(c, "MOV TEMP[%d], %s[%d]", root->reg + offset, file, io);
        if(d.builtin==SpvBuiltInFragCoord && c->bias_depth>=0)
            emit(c,"MOV TEMP[%d].z, TEMP[%d].xxxx",root->reg+offset,c->bias_depth);
        if (d.builtin==SpvBuiltInSampleMask) {
            c->shader->sample_mask_in=1;
            emit(c,"AND TEMP[%d].x, TEMP[%d].xxxx, CONST[0][%d].yyyy",root->reg+offset,
                 root->reg+offset,ZVK_PUSH/16);
        }
        if (d.builtin==SpvBuiltInPointCoord)
            emit(c,"ADD TEMP[%d].y, IMM[%d], -TEMP[%d].yyyy",root->reg+offset,
                 scalar_imm(c,bits(1.0f)),root->reg+offset);
    } else {
        c->out = &c->epilog;
        if (d.builtin==SpvBuiltInSampleMask) {
            c->shader->sample_mask_out=1;
            /* OpenGL ignores SampleMask on a single-sample framebuffer.
             * Kill fragments with no surviving valid sample bits explicitly;
             * native sample-mask output handles the remaining per-sample ROP. */
            int live=temporary(c,1);
            emit(c,"AND TEMP[%d].x, TEMP[%d].xxxx, CONST[0][%d].yyyy",live,
                 root->reg+offset,ZVK_PUSH/16);
            emit(c,"USEQ TEMP[%d].x, TEMP[%d].xxxx, IMM[%d]",live,live,scalar_imm(c,0));
            emit(c,"UIF TEMP[%d].xxxx",live);emit(c,"KILL");emit(c,"ENDIF");
        }
        if (c->stage == VK_SHADER_STAGE_VERTEX_BIT && d.builtin == SpvBuiltInPosition) {
            int t = temporary(c, 1);
            emit(c, "MUL TEMP[%d].x, TEMP[%d].zzzz, IMM[%d]", t, root->reg + offset,
                 scalar_imm(c, bits(2)));
            emit(c, "ADD TEMP[%d].z, TEMP[%d].xxxx, -TEMP[%d].wwww", root->reg + offset, t,
                 root->reg + offset);
        }
        if (d.builtin == SpvBuiltInFragDepth) {
            c->writes_depth=1;
            emit(c, "MOV OUT[%d].z, TEMP[%d].xxxx", io, root->reg + offset);
        } else
            emit(c, "MOV OUT[%d].%s, TEMP[%d]", io, mask(width(c, type)), root->reg + offset);
    }
}
static void constants(struct compiler *c)
{
    c->out = &c->prolog;
    for (size_t word = 5; word < c->words; word += wc(c->code + word)) {
        const uint32_t *p = c->code + word;
        SpvOp op = opcode(p);
        bool has_result, has_type;
        SpvHasResultAndType(op, &has_result, &has_type);
        if (!has_result || !has_type)
            continue;
        struct id *n = get(c, p[2]);
        if (n->reg < 0)
            continue;
        if (n->literal_valid) {
            emit(c, "MOV TEMP[%d], IMM[%d]", n->reg, scalar_imm(c, n->literal));
            continue;
        }
        if (op==SpvOpSpecConstantOp) {
            for (int reg=0;reg<registers(c,n->type);reg++) {
                uint32_t values[4];
                for(unsigned lane=0;lane<4;lane++)values[lane]=graphics_constant_word(c,p[2],reg,lane,0);
                emit(c,"MOV TEMP[%d], IMM[%d]",n->reg+reg,imm(c,values[0],values[1],values[2],values[3]));
            }
            continue;
        }
        if (op == SpvOpConstantNull || op == SpvOpUndef) {
            for (int i = 0; i < registers(c, n->type); i++)
                emit(c, "MOV TEMP[%d], IMM[%d]", n->reg + i, scalar_imm(c, 0));
        }
        if (op == SpvOpConstantComposite || op == SpvOpSpecConstantComposite) {
            SpvOp type = opcode(get(c, n->type)->ins);
            int off = 0;
            for (int k = 3; k < wc(p); k++) {
                struct id *child = get(c, p[k]);
                if (child->reg < 0)
                    fail(c, "invalid composite constituent");
                if (type == SpvOpTypeVector &&
                    (k - 3 >= width(c, n->type) || width(c, child->type) != 1))
                    fail(c, "invalid vector constant constituent");
                if (type != SpvOpTypeVector &&
                    off + registers(c, child->type) > registers(c, n->type))
                    fail(c, "composite constant overflow");
                if (type == SpvOpTypeVector)
                    emit(c, "MOV TEMP[%d].%c, TEMP[%d].xxxx", n->reg, "xyzw"[k - 3], child->reg);
                else {
                    copy(c, n->reg + off, child->reg, child->type);
                    off += registers(c, child->type);
                }
            }
        }
        if (op == SpvOpVariable &&
            (p[3] == SpvStorageClassPrivate || p[3] == SpvStorageClassOutput) && wc(p) > 4) {
            copy(c, n->reg, get(c, p[4])->reg, n->ref.type);
            if(p[3]==SpvStorageClassOutput) {
                int count=registers(c,n->ref.type);
                if(count>64)fail(c,"initialized output exceeds interface register limit");
                n->used|=count==64?UINT64_MAX:(1ull<<count)-1;
            }
        }
    }
}
static void composite_extract(struct compiler *c, uint32_t result, uint32_t base,
                              const uint32_t *indices, int n)
{
    struct id *a = get(c, base), *r = get(c, result);
    struct reference ref = {
        .root = base, .type = a->type, .dynamic = -1, .component_stride = 1, .matrix_stride = 4};
    for (int i = 0; i < n; i++)
        ref = child_ref(c, ref, indices[i], 1);
    int count = registers(c, r->type);
    if (ref.offset % 4) {
        if (count != 1)
            fail(c, "invalid composite offset");
        for (int i = 0; i < width(c, r->type); i++)
            emit(c, "MOV TEMP[%d].%c, TEMP[%d].%c%c%c%c", r->reg, "xyzw"[i],
                 a->reg + ref.offset / 4, "xyzw"[(ref.offset % 4) + i],
                 "xyzw"[(ref.offset % 4) + i], "xyzw"[(ref.offset % 4) + i],
                 "xyzw"[(ref.offset % 4) + i]);
    } else
        copy(c, r->reg, a->reg + ref.offset / 4, r->type);
}
static int native_extinst(struct compiler *c, const uint32_t *p)
{
    uint32_t op = p[4];
    if (!op || op > 81 || op == 47 || op == 59 || op == 65 || (op >= 76 && op <= 78))
        return 0;
    unsigned nargs = (op >= 37 && op <= 42) || op == 25 || op == 26 || op == 35 || op == 48 ||
                             op == 51 || op == 53 || op == 67 || op == 68 || op == 71 || op == 79 || op == 80
                         ? 2
                     : (op >= 43 && op <= 46) || op == 49 || op == 50 || op == 70 || op == 72 || op == 81
                         ? 3 : 1;
    if ((unsigned)wc(p) != 5 + nargs)
        fail(c, "native extended instruction operand count");
    struct id *input = get(c, p[5]), *result = get(c, p[2]);
    const uint32_t *type = get(c, input->type)->ins;
    unsigned n = width(c, input->type);
    if (opcode(type) == SpvOpTypeMatrix) {
        n = type[3];
        if ((op != 33 && op != 34) || n != (unsigned)width(c, type[2]))
            fail(c, "matrix intrinsic requires a square matrix");
    } else if (op == 33 || op == 34)
        fail(c, "matrix intrinsic requires a matrix operand");
    if (!n || n > 4)
        fail(c, "intrinsic operand width");
    for (unsigned a = 0; a < nargs; a++) {
        if (a == 1 && (op == 35 || op == 51))
            continue;
        struct id *value = get(c, p[5 + a]);
        unsigned count = registers(c, value->type);
        if (count > 4)
            fail(c, "intrinsic aggregate shape");
        for (unsigned reg = 0; reg < count; reg++) {
            char operand[64];
            source(c, p[5 + a], reg, operand);
            emit(c, "MOV TEMP[%u], %s", 4 + a * 4 + reg, operand);
        }
    }
    emit(c, "CAL : %u", 0x5a0000u | (n << 8) | op);
    c->shader->native_intrinsics = 1;
    if (op == 36 || op == 52) {
        const uint32_t *structure = get(c, result->type)->ins;
        if (opcode(structure) != SpvOpTypeStruct || wc(structure) != 4)
            fail(c, "intrinsic result structure");
        copy(c, result->reg, 0, structure[2]);
        copy(c, result->reg + registers(c, structure[2]), 1, structure[3]);
    } else {
        copy(c, result->reg, 0, result->type);
        if (op == 35 || op == 51) {
            struct reference output = pointer(c, p[6]);
            store_value(c, output, 1, output.type);
        }
    }
    return 1;
}
static void extinst(struct compiler *c, const uint32_t *p)
{
    minimum(c, p, 6);
    uint32_t result = p[2], instruction = p[4];
    const uint32_t *import = get(c, p[3])->ins;
    if (opcode(import) != SpvOpExtInstImport ||
        !memchr(import + 2, 0, (size_t)(wc(import) - 2) * 4) ||
        strcmp((const char *)(import + 2), "GLSL.std.450"))
        fail(c, "unsupported extended instruction set");
    const char *op = NULL;
    switch (instruction) {
    case GLSLstd450FAbs:
        op = "ABS";
        break;
    case GLSLstd450SAbs:
        op = "IABS";
        break;
    case GLSLstd450Floor:
        op = "FLR";
        break;
    case GLSLstd450Ceil:
        op = "CEIL";
        break;
    case GLSLstd450Fract:
        op = "FRC";
        break;
    case GLSLstd450Trunc:
        op = "TRUNC";
        break;
    case GLSLstd450RoundEven:
        op = "ROUND";
        break;
    case GLSLstd450Sqrt:
        op = "SQRT";
        break;
    case GLSLstd450InverseSqrt:
        op = "RSQ";
        break;
    case GLSLstd450Sin:
        op = "SIN";
        break;
    case GLSLstd450Cos:
        op = "COS";
        break;
    case GLSLstd450Exp2:
        op = "EX2";
        break;
    case GLSLstd450Log2:
        op = "LG2";
        break;
    default:
        break;
    }
    if (op) {
        unary(c, op, result, p[5]);
        return;
    }
    switch (instruction) {
    case GLSLstd450FMin:
        op = "MIN";
        break;
    case GLSLstd450FMax:
        op = "MAX";
        break;
    case GLSLstd450SMin:
        op = "IMIN";
        break;
    case GLSLstd450SMax:
        op = "IMAX";
        break;
    case GLSLstd450UMin:
        op = "UMIN";
        break;
    case GLSLstd450UMax:
        op = "UMAX";
        break;
    default:
        break;
    }
    if (op) {
        minimum(c, p, 7);
        binary(c, op, result, p[5], p[6]);
        return;
    }
    struct id *r = get(c, result);
    char a[64], b[64], d[64];
    source(c, p[5], 0, a);
    if (instruction == GLSLstd450FClamp || instruction == GLSLstd450SClamp ||
        instruction == GLSLstd450UClamp) {
        minimum(c, p, 8);
        source(c, p[6], 0, b);
        source(c, p[7], 0, d);
        emit(c, "%s TEMP[%d].%s, %s, %s",
             instruction == GLSLstd450FClamp   ? "MAX"
             : instruction == GLSLstd450SClamp ? "IMAX"
                                               : "UMAX",
             r->reg, mask(width(c, r->type)), a, b);
        emit(c, "%s TEMP[%d].%s, TEMP[%d], %s",
             instruction == GLSLstd450FClamp   ? "MIN"
             : instruction == GLSLstd450SClamp ? "IMIN"
                                               : "UMIN",
             r->reg, mask(width(c, r->type)), r->reg, d);
        return;
    }
    if (instruction == GLSLstd450FMix) {
        minimum(c, p, 8);
        source(c, p[6], 0, b);
        source(c, p[7], 0, d);
        int t = temporary(c, 1);
        emit(c, "ADD TEMP[%d], %s, -%s", t, b, a);
        emit(c, "MAD TEMP[%d].%s, TEMP[%d], %s, %s", r->reg, mask(width(c, r->type)), t, d, a);
        return;
    }
    if (instruction == GLSLstd450Normalize || instruction == GLSLstd450Length) {
        int n = width(c, get(c, p[5])->type), t = temporary(c, 1);
        if (n == 1 && native_extinst(c, p))
            return;
        if (n < 2 || n > 4)
            fail(c, "normalize/length vector width");
        emit(c, "DP%d TEMP[%d].x, %s, %s", n, t, a, a);
        if (instruction == GLSLstd450Normalize) {
            emit(c, "RSQ TEMP[%d].x, TEMP[%d].xxxx", t, t);
            emit(c, "MUL TEMP[%d].%s, %s, TEMP[%d].xxxx", r->reg, mask(n), a, t);
        } else
            emit(c, "SQRT TEMP[%d].x, TEMP[%d].xxxx", r->reg, t);
        return;
    }
    if (!native_extinst(c, p))
        fail(c, "unsupported GLSL.std.450 instruction %u", instruction);
}
static void float_compare(struct compiler *c, SpvOp op, uint32_t result, uint32_t a, uint32_t b)
{
    struct id *r = get(c, result);
    char sa[64], sb[64];
    source(c, a, 0, sa);
    source(c, b, 0, sb);
    int unordered = op == SpvOpFUnordEqual || op == SpvOpFUnordNotEqual ||
                    op == SpvOpFUnordLessThan || op == SpvOpFUnordGreaterThan ||
                    op == SpvOpFUnordLessThanEqual || op == SpvOpFUnordGreaterThanEqual;
    const char *operation;
    switch (op) {
    case SpvOpFOrdEqual:
    case SpvOpFUnordEqual:
        operation = "FSEQ";
        break;
    case SpvOpFOrdNotEqual:
    case SpvOpFUnordNotEqual:
        operation = "FSNE";
        break;
    case SpvOpFOrdLessThan:
    case SpvOpFUnordLessThan:
        operation = "FSLT";
        break;
    case SpvOpFOrdGreaterThan:
    case SpvOpFUnordGreaterThan:
        operation = "FSLT";
        break;
    default:
        operation = "FSGE";
        break;
    }
    int swap = op == SpvOpFOrdGreaterThan || op == SpvOpFUnordGreaterThan ||
               op == SpvOpFOrdLessThanEqual || op == SpvOpFUnordLessThanEqual;
    emit(c, "%s TEMP[%d].%s, %s, %s", operation, r->reg, mask(width(c, r->type)), swap ? sb : sa,
         swap ? sa : sb);
    if (unordered || op == SpvOpFOrdNotEqual) {
        int t = temporary(c, 2);
        emit(c, "FSEQ TEMP[%d], %s, %s", t, sa, sa);
        emit(c, "FSEQ TEMP[%d], %s, %s", t + 1, sb, sb);
        emit(c, "AND TEMP[%d], TEMP[%d], TEMP[%d]", t, t, t + 1);
        if (unordered)
            emit(c, "NOT TEMP[%d], TEMP[%d]", t, t);
        emit(c, "%s TEMP[%d].%s, TEMP[%d], TEMP[%d]", unordered ? "OR" : "AND", r->reg,
             mask(width(c, r->type)), r->reg, t);
    }
}
static void image_offset(struct compiler *c, struct id *image, uint32_t id, char text[64])
{
    unsigned dims = image->sampler == PIPE_TEXTURE_1D || image->sampler == PIPE_TEXTURE_1D_ARRAY ? 1
                    : image->sampler == PIPE_TEXTURE_3D ? 3
                                                        : 2;
    if (image->sampler == PIPE_TEXTURE_CUBE || image->image_ms)
        fail(c, "offset is not valid for this image target");
    struct id *n = get(c, id);
    uint32_t values[4] = {0};
    if (width(c, n->type) != (int)dims)
        fail(c, "image offset dimensionality");
    for (unsigned i = 0; i < dims; i++) {
        uint32_t value;
        if (dims == 1)
            value = constant(c, id);
        else {
            if (opcode(n->ins) != SpvOpConstantComposite &&
                opcode(n->ins) != SpvOpSpecConstantComposite)
                fail(c, "image offset must be constant");
            minimum(c, n->ins, 3 + dims);
            value = constant(c, n->ins[3 + i]);
        }
        if ((int32_t)value < -8 || (int32_t)value > 7)
            fail(c, "image offset exceeds -8..7");
        values[i] = value;
    }
    snprintf(text, 64, ", IMM[%d].xyz", imm(c, values[0], values[1], values[2], 0));
}
/* Buffer descriptors use a tightly packed 4096-wide 2D texture snapshot. Native
 * GL texture buffers crash in the pinned Apple driver when their storage is
 * destroyed and another buffer is subsequently mapped. Fetches and descriptor
 * bounds checks still execute entirely on the GPU. */
static void texture_used(struct compiler *c,struct id *image,int sampling)
{
    for (unsigned i=0;i<c->shader->nbindings;i++) {
        zvk_shader_binding *b=c->shader->bindings+i;
        if (!constant_binding(b->type) && b->slot==(unsigned)image->slot) {
            b->texture_use=1;
            if (!c->shader->resource_atlas || image->sampler_slot==image->slot)b->uses_sampling|=sampling;
        }
    }
    if (sampling && c->shader->resource_atlas)
        for (unsigned i=0;i<c->shader->nsamplers;i++)
            if (c->shader->samplers[i].slot==(unsigned)image->sampler_slot)
                c->shader->samplers[i].uses_sampling=1;
}
static void buffer_fetch(struct compiler *c, struct id *result, struct id *image, uint32_t index)
{
    texture_used(c,image,0);
    char source_index[64];
    source(c, index, 0, source_index);
    int coord = temporary(c, 1), valid = temporary(c, 1);
    int word = (ZVK_PUSH + 16) / 4 + image->slot;
    c->shader->sample_queries |= 1u << image->slot;
    emit(c, "MOV TEMP[%d], IMM[%d]", coord, scalar_imm(c, 0));
    emit(c, "AND TEMP[%d].x, %s, IMM[%d]", coord, source_index, scalar_imm(c, 4095));
    emit(c, "USHR TEMP[%d].y, %s, IMM[%d]", coord, source_index, scalar_imm(c, 12));
    emit(c, "USLT TEMP[%d].x, %s, CONST[0][%d].%c%c%c%c", valid, source_index, word / 4,
         "xyzw"[word % 4], "xyzw"[word % 4], "xyzw"[word % 4], "xyzw"[word % 4]);
    emit(c, "TXF TEMP[%d], TEMP[%d], SAMP[%d], 2D", result->reg, coord, image->slot);
    emit(c, "UCMP TEMP[%d], TEMP[%d].xxxx, TEMP[%d], IMM[%d]", result->reg, valid,
         result->reg, scalar_imm(c, 0));
}
/* Unsigned MS views use a sample-preserving integer array snapshot. This
 * also makes stencil readable on GL hosts without stencil texturing. */
static void flatten_uint_ms(struct compiler *c, struct id *image, int coord)
{
    if (!image->image_ms || image->image_kind != 0)
        return;
    c->shader->sample_queries |= 1u << image->slot;
    int word = (ZVK_PUSH + 16) / 4 + 2 * ZVK_SAMPLERS + image->slot;
    int offset = temporary(c, 1);
    emit(c, "UMUL TEMP[%d].x, TEMP[%d].wwww, CONST[0][%d].%c%c%c%c", offset, coord, word / 4,
         "xyzw"[word % 4], "xyzw"[word % 4], "xyzw"[word % 4], "xyzw"[word % 4]);
    emit(c, "UADD TEMP[%d].y, TEMP[%d].yyyy, TEMP[%d].xxxx", coord, coord, offset);
    if (image->sampler != PIPE_TEXTURE_2D_ARRAY)
        emit(c, "MOV TEMP[%d].z, IMM[%d]", coord, scalar_imm(c, 0));
    emit(c, "MOV TEMP[%d].w, IMM[%d]", coord, scalar_imm(c, 0));
}
/* Multisample texture targets cannot carry GL texture parameter swizzles on
 * the pinned Mac host. Apply the Vulkan view mapping explicitly on the GPU. */
static void image_mapping(struct compiler *c, struct id *result, struct id *image)
{
    if (!image->image_ms || image->image_kind == 0)
        return;
    c->shader->sample_queries |= 1u << image->slot;
    int raw = temporary(c, 1), mapping = temporary(c, 1), index = temporary(c, 1),
        condition = temporary(c, 1);
    int word = (ZVK_PUSH + 16) / 4 + ZVK_SAMPLERS + image->slot;
    emit(c, "MOV TEMP[%d], TEMP[%d]", raw, result->reg);
    emit(c, "MOV TEMP[%d].x, CONST[0][%d].%c%c%c%c", mapping, word / 4, "xyzw"[word % 4],
         "xyzw"[word % 4], "xyzw"[word % 4], "xyzw"[word % 4]);
    const uint32_t *scalar = get(c, result->type)->ins;
    if (opcode(scalar) == SpvOpTypeVector)
        scalar = get(c, scalar[2])->ins;
    uint32_t one = opcode(scalar) == SpvOpTypeFloat ? 0x3f800000u : 1u;
    for (int i = 0; i < width(c, result->type); i++) {
        char lane = "xyzw"[i];
        emit(c, "USHR TEMP[%d].x, TEMP[%d].xxxx, IMM[%d]", index, mapping, scalar_imm(c, i * 3));
        emit(c, "AND TEMP[%d].x, TEMP[%d].xxxx, IMM[%d]", index, index, scalar_imm(c, 7));
        emit(c, "MOV TEMP[%d].%c, IMM[%d]", result->reg, lane, scalar_imm(c, 0));
        for (int j = 0; j < 4; j++) {
            char source = "xyzw"[j];
            emit(c, "USEQ TEMP[%d].x, TEMP[%d].xxxx, IMM[%d]", condition, index, scalar_imm(c, j));
            emit(c, "UCMP TEMP[%d].%c, TEMP[%d].xxxx, TEMP[%d].%c%c%c%c, TEMP[%d].%c%c%c%c",
                 result->reg, lane, condition, raw, source, source, source, source, result->reg,
                 lane, lane, lane, lane);
        }
        emit(c, "USEQ TEMP[%d].x, TEMP[%d].xxxx, IMM[%d]", condition, index, scalar_imm(c, 5));
        emit(c, "UCMP TEMP[%d].%c, TEMP[%d].xxxx, IMM[%d], TEMP[%d].%c%c%c%c", result->reg, lane,
             condition, scalar_imm(c, one), result->reg, lane, lane, lane, lane);
    }
}
static void function_emit(struct compiler *c, uint32_t function, int return_reg, int depth);
#include "spirv_atlas.h"
static void instruction_emit(struct compiler *c, const uint32_t *p, int depth)
{
    SpvOp op = opcode(p);
    int result = wc(p) > 2 ? (int)p[2] : 0;
    bool has_result, has_type;
    SpvHasResultAndType(op, &has_result, &has_type);
    c->precise = has_result && has_type && get(c, p[2])->d.precise;
    if (atlas_image(c,p))return;
    const char *operation = NULL;
    switch (op) {
    case SpvOpUndef:
        if(get(c,p[2])->reg<0)return;
        for (int i=0;i<registers(c,get(c,p[2])->type);i++)
            emit(c,"MOV TEMP[%d], IMM[%d]",get(c,p[2])->reg+i,scalar_imm(c,0));
        return;
    case SpvOpLine:
    case SpvOpNoLine:
    case SpvOpSelectionMerge:
    case SpvOpLoopMerge:
    case SpvOpNop:
    case SpvOpPhi:
        return;
    case SpvOpVariable: {
        minimum(c, p, 4);
        struct id *r = get(c, p[2]);
        if (wc(p) > 4)
            copy(c, r->reg, get(c, p[4])->reg, r->ref.type);
        return;
    }
    case SpvOpAccessChain:
    case SpvOpInBoundsAccessChain: {
        minimum(c, p, 4);
        struct reference r = pointer(c, p[3]);
        for (int i = 4; i < wc(p); i++)
            r = child_ref(c, r, p[i], 0);
        get(c, p[2])->ref = r;
        return;
    }
    case SpvOpArrayLength: {
        minimum(c,p,5);
        struct reference r=pointer(c,p[3]);
        struct id *root=get(c,r.root),*structure=get(c,r.type);
        if (!atlas_root(c,root) || opcode(structure->ins)!=SpvOpTypeStruct ||
            p[4]>=(uint32_t)wc(structure->ins)-2 || !structure->members ||
            structure->members[p[4]].offset<0)
            fail(c,"array length requires a storage-buffer runtime array");
        uint32_t type=structure->ins[2+p[4]];
        if (opcode(get(c,type)->ins)!=SpvOpTypeRuntimeArray || get(c,type)->d.stride<=0 ||
            (get(c,type)->d.stride&3))fail(c,"runtime array stride");
        root->accessed|=1u<<r.descriptor;
        emit(c,"MOV TEMP[4].xy, IMM[%d]",imm(c,r.offset+structure->members[p[4]].offset/4,
             get(c,type)->d.stride/4,0,0));
        emit(c,"CAL : %u",0x5c0100u+root->slot+r.descriptor-1);
        emit(c,"MOV TEMP[%d].x, TEMP[0].xxxx",get(c,p[2])->reg);
        return;
    }
    case SpvOpLoad: {
        minimum(c, p, 4);
        struct id *r = get(c, p[2]);
        struct reference ptr = pointer(c, p[3]);
        if (opcode(get(c, r->type)->ins) == SpvOpTypeSampler) {
            r->ref = ptr;
            return;
        }
        if (opcode(get(c, r->type)->ins) == SpvOpTypeSampledImage ||
            opcode(get(c, r->type)->ins) == SpvOpTypeImage) {
            struct id *root = get(c, ptr.root);
            if (!root->sampler)
                fail(c, "invalid combined sampler load");
            root->accessed |= 1u << ptr.descriptor;
            r->slot = root->slot + (int)ptr.descriptor;
            r->sampler = root->sampler;
            r->sampler_slot=root->sampler_slot+ptr.descriptor;
            r->image_ms = root->image_ms;
            r->image_kind = root->image_kind;
        } else
            load_value(c, r->reg, r->type, ptr);
        return;
    }
    case SpvOpStore: {
        minimum(c, p, 3);
        struct reference r = pointer(c, p[1]);
        struct id *value = get(c, p[2]);
        store_value(c, r, value->reg, value->type);
        return;
    }
    case SpvOpCopyMemory: {
        minimum(c, p, 3);
        struct reference src = pointer(c, p[2]), dst = pointer(c, p[1]);
        if (src.type != dst.type)
            fail(c, "copy memory type mismatch");
        int t = temporary(c, registers(c, src.type));
        load_value(c, t, src.type, src);
        store_value(c, dst, t, src.type);
        return;
    }
    case SpvOpCopyObject: {
        minimum(c, p, 4);
        struct id *r = get(c, p[2]), *a = get(c, p[3]);
        if (opcode(get(c, r->type)->ins) == SpvOpTypePointer ||
            opcode(get(c, r->type)->ins) == SpvOpTypeSampler)
            r->ref = a->ref;
        else if (a->sampler) {
            r->slot = a->slot;
            r->sampler = a->sampler;
            r->sampler_slot=a->sampler_slot;
            r->image_ms = a->image_ms;
            r->image_kind = a->image_kind;
        } else
            copy(c, r->reg, a->reg, r->type);
        return;
    }
    case SpvOpCompositeConstruct: {
        minimum(c, p, 4);
        struct id *r = get(c, p[2]);
        SpvOp type = opcode(get(c, r->type)->ins);
        int off = 0;
        for (int i = 3; i < wc(p); i++) {
            struct id *a = get(c, p[i]);
            if (type == SpvOpTypeVector) {
                for (int k = 0; k < width(c, a->type); k++) {
                    if (off >= width(c, r->type))
                        fail(c, "too many vector constituents");
                    emit(c, "MOV TEMP[%d].%c, TEMP[%d].%c%c%c%c", r->reg, "xyzw"[off++], a->reg,
                         "xyzw"[k], "xyzw"[k], "xyzw"[k], "xyzw"[k]);
                }
            } else {
                copy(c, r->reg + off, a->reg, a->type);
                off += registers(c, a->type);
            }
        }
        return;
    }
    case SpvOpCompositeExtract:
        minimum(c, p, 5);
        composite_extract(c, p[2], p[3], p + 4, wc(p) - 4);
        return;
    case SpvOpCompositeInsert: {
        minimum(c, p, 6);
        struct id *r = get(c, p[2]), *a = get(c, p[3]);
        copy(c, r->reg, get(c, p[4])->reg, r->type);
        struct reference ref = {.root = p[2],
                                .type = r->type,
                                .dynamic = -1,
                                .component_stride = 1,
                                .matrix_stride = 4};
        for (int i = 5; i < wc(p); i++)
            ref = child_ref(c, ref, p[i], 1);
        if (ref.type != a->type)
            fail(c, "composite insertion type mismatch");
        SpvOp selected = opcode(get(c, ref.type)->ins);
        if (selected == SpvOpTypeBool || selected == SpvOpTypeInt || selected == SpvOpTypeFloat) {
            emit(c, "MOV TEMP[%d].%c, TEMP[%d].xxxx", r->reg + ref.offset / 4,
                 "xyzw"[ref.offset % 4], a->reg);
        } else
            copy(c, r->reg + ref.offset / 4, a->reg, a->type);
        return;
    }
    case SpvOpVectorShuffle: {
        minimum(c, p, 5);
        struct id *r = get(c, p[2]), *a = get(c, p[3]), *b = get(c, p[4]);
        if (wc(p) - 5 != width(c, r->type))
            fail(c, "invalid shuffle width");
        int n = width(c, a->type), m = width(c, b->type);
        for (int i = 5; i < wc(p); i++) {
            uint32_t lane = p[i];
            if (lane == UINT32_MAX)
                lane = 0;
            if (lane >= (uint32_t)(n + m))
                fail(c, "shuffle selector out of range");
            int reg = lane < (uint32_t)n ? a->reg : b->reg;
            int k = lane < (uint32_t)n ? (int)lane : (int)lane - n;
            emit(c, "MOV TEMP[%d].%c, TEMP[%d].%c%c%c%c", r->reg, "xyzw"[i - 5], reg, "xyzw"[k],
                 "xyzw"[k], "xyzw"[k], "xyzw"[k]);
        }
        return;
    }
    case SpvOpAll:
    case SpvOpAny: {
        minimum(c, p, 4);
        struct id *r = get(c, p[2]), *a = get(c, p[3]);
        int count = width(c, a->type);
        emit(c, "MOV TEMP[%d].x, TEMP[%d].xxxx", r->reg, a->reg);
        for (int i = 1; i < count; i++)
            emit(c, "%s TEMP[%d].x, TEMP[%d].xxxx, TEMP[%d].%c%c%c%c",
                 op == SpvOpAll ? "AND" : "OR", r->reg, r->reg, a->reg, "xyzw"[i], "xyzw"[i],
                 "xyzw"[i], "xyzw"[i]);
        return;
    }
    case SpvOpVectorInsertDynamic: {
        minimum(c,p,6);
        struct id *r=get(c,p[2]),*a=get(c,p[3]);
        char component[64],index[64];source(c,p[4],0,component);source(c,p[5],0,index);
        copy(c,r->reg,a->reg,r->type);
        int condition=temporary(c,1);
        for (int i=0;i<width(c,r->type);i++) {
            emit(c,"USEQ TEMP[%d].x, %s, IMM[%d]",condition,index,scalar_imm(c,(uint32_t)i));
            emit(c,"UIF TEMP[%d].xxxx",condition);
            emit(c,"MOV TEMP[%d].%c, %s",r->reg,"xyzw"[i],component);
            emit(c,"ENDIF");
        }
        return;
    }
    case SpvOpQuantizeToF16: {
        minimum(c,p,4);
        struct id *r=get(c,p[2]);char value[64];source(c,p[3],0,value);
        emit(c,"MOV TEMP[4], %s",value);
        emit(c,"CAL : %u",0x5a0000u|(width(c,r->type)<<8)|82u);
        emit(c,"MOV TEMP[%d].%s, TEMP[0]",r->reg,mask(width(c,r->type)));
        c->shader->native_intrinsics=1;
        return;
    }
    case SpvOpVectorExtractDynamic: {
        minimum(c, p, 5);
        struct id *r = get(c, p[2]), *a = get(c, p[3]), *ix = get(c, p[4]);
        int t = temporary(c, 1);
        for (int i = 0; i < width(c, a->type); i++) {
            emit(c, "USEQ TEMP[%d].x, TEMP[%d].xxxx, IMM[%d]", t, ix->reg,
                 scalar_imm(c, (uint32_t)i));
            emit(c, "UIF TEMP[%d].xxxx", t);
            emit(c, "MOV TEMP[%d].x, TEMP[%d].%c%c%c%c", r->reg, a->reg, "xyzw"[i], "xyzw"[i],
                 "xyzw"[i], "xyzw"[i]);
            emit(c, "ENDIF");
        }
        return;
    }
    case SpvOpFAdd:
        operation = "ADD";
        break;
    case SpvOpFMul:
    case SpvOpVectorTimesScalar:
        operation = "MUL";
        break;
    case SpvOpFDiv:
        operation = "DIV";
        break;
    case SpvOpFRem:
    case SpvOpFMod: {
        minimum(c, p, 5);
        struct id *r = get(c, p[2]);
        char a[64], b[64];
        source(c, p[3], 0, a);
        source(c, p[4], 0, b);
        int t = temporary(c, 1);
        emit(c, "DIV TEMP[%d], %s, %s", t, a, b);
        emit(c, "%s TEMP[%d], TEMP[%d]", op == SpvOpFMod ? "FLR" : "TRUNC", t, t);
        emit(c, "MUL TEMP[%d], TEMP[%d], %s", t, t, b);
        emit(c, "ADD TEMP[%d].%s, %s, -TEMP[%d]", r->reg, mask(width(c, r->type)), a, t);
        return;
    }
    case SpvOpDPdx:
    case SpvOpDPdxFine:
    case SpvOpDPdxCoarse:
        operation = "DDX";
        minimum(c, p, 4);
        unary(c, operation, p[2], p[3]);
        return;
    case SpvOpDPdy:
    case SpvOpDPdyFine:
    case SpvOpDPdyCoarse:
        minimum(c, p, 4);
        unary(c, "DDY", p[2], p[3]);
        return;
    case SpvOpFwidth:
    case SpvOpFwidthFine:
    case SpvOpFwidthCoarse: {
        minimum(c, p, 4);
        struct id *r = get(c, p[2]);
        char a[64];
        source(c, p[3], 0, a);
        int t = temporary(c, 2);
        emit(c, "DDX TEMP[%d], %s", t, a);
        emit(c, "DDY TEMP[%d], %s", t + 1, a);
        emit(c, "ABS TEMP[%d], TEMP[%d]", t, t);
        emit(c, "ABS TEMP[%d], TEMP[%d]", t + 1, t + 1);
        emit(c, "ADD TEMP[%d].%s, TEMP[%d], TEMP[%d]", r->reg, mask(width(c, r->type)), t, t + 1);
        return;
    }
    case SpvOpIAdd:
        operation = "UADD";
        break;
    case SpvOpIMul:
        operation = "UMUL";
        break;
    case SpvOpSDiv:
        operation = "IDIV";
        break;
    case SpvOpUDiv:
        operation = "UDIV";
        break;
    case SpvOpSMod: {
        minimum(c, p, 5);
        struct id *r = get(c, p[2]);
        char b[64];
        source(c, p[4], 0, b);
        binary(c, "MOD", p[2], p[3], p[4]);
        int t = temporary(c, 2);
        emit(c, "XOR TEMP[%d], TEMP[%d], %s", t, r->reg, b);
        emit(c, "ISLT TEMP[%d], TEMP[%d], IMM[%d]", t, t, scalar_imm(c, 0));
        emit(c, "USNE TEMP[%d], TEMP[%d], IMM[%d]", t + 1, r->reg, scalar_imm(c, 0));
        emit(c, "AND TEMP[%d], TEMP[%d], TEMP[%d]", t, t, t + 1);
        emit(c, "UADD TEMP[%d], TEMP[%d], %s", t + 1, r->reg, b);
        emit(c, "UCMP TEMP[%d].%s, TEMP[%d], TEMP[%d], TEMP[%d]", r->reg, mask(width(c, r->type)),
             t, t + 1, r->reg);
        return;
    }
    case SpvOpSRem:
        operation = "MOD";
        break;
    case SpvOpUMod:
        operation = "UMOD";
        break;
    case SpvOpLogicalAnd:
    case SpvOpLogicalOr:
    case SpvOpLogicalEqual:
    case SpvOpLogicalNotEqual: {
        int a=temporary(c,1),b=temporary(c,1);
        struct id *r=get(c,p[2]);
        char x[64],y[64];source(c,p[3],0,x);source(c,p[4],0,y);
        emit(c,"USNE TEMP[%d], %s, IMM[%d]",a,x,scalar_imm(c,0));
        emit(c,"USNE TEMP[%d], %s, IMM[%d]",b,y,scalar_imm(c,0));
        emit(c,"%s TEMP[%d], TEMP[%d], TEMP[%d]",op==SpvOpLogicalAnd?"AND":op==SpvOpLogicalOr?"OR":
             op==SpvOpLogicalEqual?"USEQ":"USNE",r->reg,a,b);
        return;
    }
    case SpvOpBitwiseAnd:
        operation = "AND";
        break;
    case SpvOpBitwiseOr:
        operation = "OR";
        break;
    case SpvOpBitwiseXor:
        operation = "XOR";
        break;
    case SpvOpShiftLeftLogical:
    case SpvOpShiftRightLogical:
    case SpvOpShiftRightArithmetic: {
        minimum(c,p,5);
        char value[64],amount[64];source(c,p[3],0,value);source(c,p[4],0,amount);
        int shift=temporary(c,1);
        emit(c,"AND TEMP[%d], %s, IMM[%d]",shift,amount,scalar_imm(c,31));
        struct id *r=get(c,p[2]);
        emit(c,"%s TEMP[%d].%s, %s, TEMP[%d]",op==SpvOpShiftLeftLogical?"SHL":
             op==SpvOpShiftRightLogical?"USHR":"ISHR",r->reg,mask(width(c,r->type)),value,shift);
        return;
    }
    case SpvOpIEqual:
        operation = "USEQ";
        break;
    case SpvOpINotEqual:
        operation = "USNE";
        break;
    case SpvOpFOrdEqual:
    case SpvOpFUnordEqual:
    case SpvOpFOrdNotEqual:
    case SpvOpFUnordNotEqual:
    case SpvOpFOrdLessThan:
    case SpvOpFUnordLessThan:
    case SpvOpFOrdGreaterThan:
    case SpvOpFUnordGreaterThan:
    case SpvOpFOrdLessThanEqual:
    case SpvOpFUnordLessThanEqual:
    case SpvOpFOrdGreaterThanEqual:
    case SpvOpFUnordGreaterThanEqual:
        minimum(c, p, 5);
        float_compare(c, op, p[2], p[3], p[4]);
        return;
    case SpvOpIsNan: {
        minimum(c, p, 4);
        struct id *r = get(c, p[2]);
        char a[64];
        source(c, p[3], 0, a);
        emit(c, "FSEQ TEMP[%d].%s, %s, %s", r->reg, mask(width(c, r->type)), a, a);
        emit(c, "NOT TEMP[%d].%s, TEMP[%d]", r->reg, mask(width(c, r->type)), r->reg);
        return;
    }
    case SpvOpIsInf: {
        minimum(c, p, 4);
        struct id *r = get(c, p[2]);
        char a[64];
        source(c, p[3], 0, a);
        int t = temporary(c, 1);
        emit(c, "AND TEMP[%d], %s, IMM[%d]", t, a, scalar_imm(c, 0x7fffffff));
        emit(c, "USEQ TEMP[%d].%s, TEMP[%d], IMM[%d]", r->reg, mask(width(c, r->type)), t,
             scalar_imm(c, 0x7f800000));
        return;
    }
    case SpvOpBitCount:
    case SpvOpBitReverse:
        minimum(c, p, 4);
        unary(c, op == SpvOpBitCount ? "POPC" : "BREV", p[2], p[3]);
        return;
    case SpvOpBitFieldInsert:
    case SpvOpBitFieldSExtract:
    case SpvOpBitFieldUExtract: {
        int insert = op == SpvOpBitFieldInsert;
        minimum(c, p, insert ? 7 : 6);
        struct id *r = get(c, p[2]);
        char a[64], b[64], off[64], count[64];
        source(c, p[3], 0, a);
        source(c, p[insert ? 5 : 4], 0, off);
        source(c, p[insert ? 6 : 5], 0, count);
        /* Undefined SPIR-V result values must not become undefined native
         * control flow. Keep the GLSL bitfield operands in its valid range. */
        int safe_offset=temporary(c,1),safe_count=temporary(c,1),remaining=temporary(c,1);
        emit(c,"UMIN TEMP[%d], %s, IMM[%d]",safe_offset,off,scalar_imm(c,32));
        emit(c,"INEG TEMP[%d], TEMP[%d]",remaining,safe_offset);
        emit(c,"UADD TEMP[%d], TEMP[%d], IMM[%d]",remaining,remaining,scalar_imm(c,32));
        emit(c,"UMIN TEMP[%d], %s, TEMP[%d]",safe_count,count,remaining);
        snprintf(off,sizeof off,"TEMP[%d]",safe_offset);
        snprintf(count,sizeof count,"TEMP[%d]",safe_count);
        if (insert) {
            source(c, p[4], 0, b);
            emit(c, "BFI TEMP[%d].%s, %s, %s, %s, %s", r->reg, mask(width(c, r->type)), a, b, off, count);
        } else
            emit(c, "%s TEMP[%d].%s, %s, %s, %s", op == SpvOpBitFieldSExtract ? "IBFE" : "UBFE",
                 r->reg, mask(width(c, r->type)), a, off, count);
        return;
    }
    case SpvOpSLessThan:
        operation = "ISLT";
        break;
    case SpvOpSGreaterThanEqual:
        operation = "ISGE";
        break;
    case SpvOpULessThan:
        operation = "USLT";
        break;
    case SpvOpUGreaterThanEqual:
        operation = "USGE";
        break;
    case SpvOpFSub:
    case SpvOpISub: {
        minimum(c, p, 5);
        struct id *r = get(c, p[2]);
        char a[64], b[64];
        source(c, p[3], 0, a);
        source(c, p[4], 0, b);
        if (op == SpvOpFSub)
            emit(c, "ADD TEMP[%d].%s, %s, -%s", r->reg, mask(width(c, r->type)), a, b);
        else {
            int t = temporary(c, 1);
            emit(c, "INEG TEMP[%d], %s", t, b);
            emit(c, "UADD TEMP[%d].%s, %s, TEMP[%d]", r->reg, mask(width(c, r->type)), a, t);
        }
        return;
    }
    case SpvOpFNegate: {
        minimum(c, p, 4);
        struct id *r = get(c, p[2]);
        char a[64];
        source(c, p[3], 0, a);
        emit(c, "MOV TEMP[%d].%s, -%s", r->reg, mask(width(c, r->type)), a);
        return;
    }
    case SpvOpSNegate:
        minimum(c, p, 4);
        unary(c, "INEG", p[2], p[3]);
        return;
    case SpvOpLogicalNot: {
        struct id *r=get(c,p[2]);char a[64];source(c,p[3],0,a);
        emit(c,"USEQ TEMP[%d], %s, IMM[%d]",r->reg,a,scalar_imm(c,0));
        return;
    }
    case SpvOpNot:
        minimum(c, p, 4);
        unary(c, "NOT", p[2], p[3]);
        return;
    case SpvOpConvertFToS:
        minimum(c, p, 4);
        unary(c, "F2I", p[2], p[3]);
        return;
    case SpvOpConvertFToU:
        minimum(c, p, 4);
        unary(c, "F2U", p[2], p[3]);
        return;
    case SpvOpConvertSToF:
        minimum(c, p, 4);
        unary(c, "I2F", p[2], p[3]);
        return;
    case SpvOpConvertUToF:
        minimum(c, p, 4);
        unary(c, "U2F", p[2], p[3]);
        return;
    case SpvOpBitcast:
        minimum(c, p, 4);
        copy(c, get(c, p[2])->reg, get(c, p[3])->reg, get(c, p[2])->type);
        return;
    case SpvOpDot: {
        minimum(c, p, 5);
        int n = width(c, get(c, p[3])->type);
        if (n < 2 || n > 4)
            fail(c, "dot width");
        char name[8];
        snprintf(name, sizeof name, "DP%d", n);
        binary(c, name, p[2], p[3], p[4]);
        return;
    }
    case SpvOpTranspose: {
        struct id *r=get(c,p[2]),*m=get(c,p[3]);
        const uint32_t *mt=get(c,m->type)->ins;
        if (opcode(mt)!=SpvOpTypeMatrix)fail(c,"transpose operand");
        int rows=width(c,mt[2]);
        for (unsigned col=0;col<mt[3];col++)
            for (int row=0;row<rows;row++) {
                char lane="xyzw"[row];
                emit(c,"MOV TEMP[%d].%c, TEMP[%d].%c%c%c%c",r->reg+row,"xyzw"[col],m->reg+col,lane,lane,lane,lane);
            }
        return;
    }
    case SpvOpVectorTimesMatrix: {
        struct id *r=get(c,p[2]),*v=get(c,p[3]),*m=get(c,p[4]);
        const uint32_t *mt=get(c,m->type)->ins;
        int rows=width(c,mt[2]);
        for (unsigned col=0;col<mt[3];col++)
            emit(c,"DP%d TEMP[%d].%c, TEMP[%d], TEMP[%d]",rows,r->reg,"xyzw"[col],v->reg,m->reg+col);
        return;
    }
    case SpvOpMatrixTimesMatrix: {
        struct id *r=get(c,p[2]),*a=get(c,p[3]),*b=get(c,p[4]);
        const uint32_t *at=get(c,a->type)->ins,*bt=get(c,b->type)->ins;
        if (opcode(at)!=SpvOpTypeMatrix || opcode(bt)!=SpvOpTypeMatrix)
            fail(c,"matrix multiplication operands");
        int rows=width(c,at[2]);
        for (unsigned col=0;col<bt[3];col++) {
            emit(c,"MUL TEMP[%d].%s, TEMP[%d], TEMP[%d].xxxx",r->reg+col,mask(rows),a->reg,b->reg+col);
            for (unsigned k=1;k<at[3];k++) {
                char lane="xyzw"[k];
                emit(c,"MAD TEMP[%d].%s, TEMP[%d], TEMP[%d].%c%c%c%c, TEMP[%d]",r->reg+col,
                     mask(rows),a->reg+k,b->reg+col,lane,lane,lane,lane,r->reg+col);
            }
        }
        return;
    }
    case SpvOpOuterProduct: {
        struct id *r=get(c,p[2]),*a=get(c,p[3]),*b=get(c,p[4]);
        for (int col=0;col<width(c,b->type);col++) {
            char lane="xyzw"[col];
            emit(c,"MUL TEMP[%d].%s, TEMP[%d], TEMP[%d].%c%c%c%c",r->reg+col,mask(width(c,a->type)),
                 a->reg,b->reg,lane,lane,lane,lane);
        }
        return;
    }
    case SpvOpIAddCarry:
    case SpvOpISubBorrow:
    case SpvOpUMulExtended:
    case SpvOpSMulExtended: {
        struct id *r=get(c,p[2]),*a=get(c,p[3]),*b=get(c,p[4]);
        const char *low=op==SpvOpIAddCarry?"UADD":op==SpvOpISubBorrow?"UADD":"UMUL";
        if (op==SpvOpISubBorrow) {
            int neg=temporary(c,1);
            emit(c,"INEG TEMP[%d], TEMP[%d]",neg,b->reg);
            emit(c,"UADD TEMP[%d], TEMP[%d], TEMP[%d]",r->reg,a->reg,neg);
        } else emit(c,"%s TEMP[%d], TEMP[%d], TEMP[%d]",low,r->reg,a->reg,b->reg);
        if (op==SpvOpIAddCarry || op==SpvOpISubBorrow) {
            emit(c,"USLT TEMP[%d], TEMP[%d], TEMP[%d]",r->reg+1,
                 op==SpvOpIAddCarry?r->reg:a->reg,op==SpvOpIAddCarry?a->reg:b->reg);
            emit(c,"AND TEMP[%d], TEMP[%d], IMM[%d]",r->reg+1,r->reg+1,scalar_imm(c,1));
        } else emit(c,"%s TEMP[%d], TEMP[%d], TEMP[%d]",op==SpvOpSMulExtended?"IMUL_HI":"UMUL_HI",r->reg+1,a->reg,b->reg);
        return;
    }
    case SpvOpMatrixTimesVector: {
        minimum(c, p, 5);
        struct id *r = get(c, p[2]), *m = get(c, p[3]), *v = get(c, p[4]);
        const uint32_t *mt = get(c, m->type)->ins;
        emit(c, "MUL TEMP[%d].%s, TEMP[%d], TEMP[%d].xxxx", r->reg, mask(width(c, r->type)), m->reg,
             v->reg);
        for (uint32_t i = 1; i < mt[3]; i++)
            emit(c, "MAD TEMP[%d].%s, TEMP[%d], TEMP[%d].%c%c%c%c, TEMP[%d]", r->reg,
                 mask(width(c, r->type)), m->reg + (int)i, v->reg, "xyzw"[i], "xyzw"[i], "xyzw"[i],
                 "xyzw"[i], r->reg);
        return;
    }
    case SpvOpMatrixTimesScalar: {
        minimum(c, p, 5);
        struct id *r = get(c, p[2]), *m = get(c, p[3]);
        char a[64];
        source(c, p[4], 0, a);
        for (int i = 0; i < registers(c, r->type); i++)
            emit(c, "MUL TEMP[%d], TEMP[%d], %s", r->reg + i, m->reg + i, a);
        return;
    }
    case SpvOpSelect: {
        minimum(c, p, 6);
        struct id *r = get(c, p[2]);
        char a[64], b[64], d[64];
        source(c, p[3], 0, a);
        source(c, p[4], 0, b);
        source(c, p[5], 0, d);
        emit(c, "UCMP TEMP[%d].%s, %s, %s, %s", r->reg, mask(width(c, r->type)), a, b, d);
        return;
    }
    case SpvOpSGreaterThan:
    case SpvOpSLessThanEqual:
    case SpvOpUGreaterThan:
    case SpvOpULessThanEqual:
        minimum(c, p, 5);
        binary(c,
               op == SpvOpSGreaterThan     ? "ISLT"
               : op == SpvOpSLessThanEqual ? "ISGE"
               : op == SpvOpUGreaterThan   ? "USLT"
                                           : "USGE",
               p[2], p[4], p[3]);
        return;
    case SpvOpExtInst:
        extinst(c, p);
        return;
    case SpvOpSampledImage: {
        minimum(c, p, 5);
        struct id *r = get(c, p[2]), *image = get(c, p[3]), *sampler = get(c, p[4]);
        if (!image->sampler || !sampler->ref.root ||
            opcode(get(c, sampler->ref.type)->ins) != SpvOpTypeSampler)
            fail(c, "image/sampler construction requires separate descriptors");
        struct id *root = get(c, sampler->ref.root);
        uint32_t element = sampler->ref.descriptor;
        if (c->shader->resource_atlas) {
            r->slot=image->slot;r->sampler=image->sampler;r->sampler_slot=root->slot+element;
            r->image_ms=image->image_ms;r->image_kind=image->image_kind;
            root->accessed|=1u<<element;
            return;
        }
        zvk_shader_binding *binding = NULL;
        unsigned next = 0;
        for (unsigned i = 0; i < c->shader->nbindings; i++) {
            zvk_shader_binding *b = c->shader->bindings + i;
            if (b->type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && b->slot >= next)
                next = b->slot + 1;
            if (b->slot == (uint32_t)image->slot && b->type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE)
                binding = b;
        }
        if (!binding)
            fail(c, "separate image binding is missing");
        zvk_shader_binding pair = *binding;
        pair.sampler_set = root->d.set;
        pair.sampler_binding = root->d.binding;
        pair.sampler_element = element;
        pair.separate_sampler = 1;
        if (binding->separate_sampler) {
            binding = NULL;
            for (unsigned i = 0; i < c->shader->nbindings; i++) {
                zvk_shader_binding *b = c->shader->bindings + i;
                if (b->type == pair.type && b->set == pair.set && b->binding == pair.binding &&
                    b->element == pair.element && b->separate_sampler &&
                    b->sampler_set == pair.sampler_set &&
                    b->sampler_binding == pair.sampler_binding &&
                    b->sampler_element == pair.sampler_element)
                    binding = b;
            }
            if (!binding) {
                if (next >= ZVK_SAMPLERS || c->shader->nbindings >= ZVK_RESOURCES)
                    fail(c, "image/sampler pair limit");
                pair.slot = next;
                binding = c->shader->bindings + c->shader->nbindings++;
                binding->slot = next;
            }
        }
        pair.slot = binding->slot;
        *binding = pair;
        r->slot = binding->slot;
        r->sampler = image->sampler;
        r->image_ms = image->image_ms;
        r->image_kind = image->image_kind;
        root->accessed |= 1u << element;
        return;
    }
    case SpvOpImage: {
        minimum(c, p, 4);
        struct id *r = get(c, p[2]), *a = get(c, p[3]);
        if (!a->sampler)
            fail(c, "image extraction requires a sampled image");
        r->slot = a->slot;
        r->sampler = a->sampler;
            r->sampler_slot=a->sampler_slot;
        r->image_ms = a->image_ms;
        r->image_kind = a->image_kind;
        return;
    }
    case SpvOpImageQueryLod: {
        minimum(c,p,5);
        struct id *r=get(c,p[2]),*image=get(c,p[3]);
        if (!image->sampler || image->image_ms)fail(c,"image LOD query target");
        texture_used(c,image,1);
        const char *target=image->sampler==PIPE_TEXTURE_1D?"1D":image->sampler==PIPE_TEXTURE_1D_ARRAY?"1D_ARRAY":
            image->sampler==PIPE_TEXTURE_2D_ARRAY?"2D_ARRAY":image->sampler==PIPE_TEXTURE_3D?"3D":
            image->sampler==PIPE_TEXTURE_CUBE?"CUBE":"2D";
        char coord[64];source(c,p[4],0,coord);
        emit(c,"LODQ TEMP[%d].xy, %s, SAMP[%d], %s",r->reg,coord,image->slot,target);
        return;
    }
    case SpvOpImageQuerySize:
    case SpvOpImageQuerySizeLod:
    case SpvOpImageQueryLevels:
    case SpvOpImageQuerySamples: {
        minimum(c, p, op == SpvOpImageQuerySizeLod ? 5 : 4);
        struct id *r = get(c, p[2]), *a = get(c, p[3]);
        if (!a->sampler)
            fail(c, "image query requires an image descriptor");
        if (a->sampler == ZVK_BUFFER_SAMPLER && op == SpvOpImageQuerySize) {
            c->shader->sample_queries |= 1u << a->slot;
            int word = (ZVK_PUSH + 16) / 4 + a->slot;
            emit(c, "MOV TEMP[%d].x, CONST[0][%d].%c%c%c%c", r->reg, word / 4,
                 "xyzw"[word % 4], "xyzw"[word % 4], "xyzw"[word % 4], "xyzw"[word % 4]);
            return;
        }
        if (op == SpvOpImageQueryLevels) {
            c->shader->sample_queries |= 1u << a->slot;
            int word = (ZVK_PUSH + 16) / 4 + ZVK_SAMPLERS + a->slot;
            emit(c, "USHR TEMP[%d].x, CONST[0][%d].%c%c%c%c, IMM[%d]", r->reg, word / 4,
                 "xyzw"[word % 4], "xyzw"[word % 4], "xyzw"[word % 4], "xyzw"[word % 4],
                 scalar_imm(c, 16));
            return;
        }
        if (op == SpvOpImageQuerySamples) {
            if (!a->image_ms)
                fail(c, "sample count query requires a multisample image");
            c->shader->sample_queries |= 1u << a->slot;
            int word = (ZVK_PUSH + 16) / 4 + a->slot;
            emit(c, "MOV TEMP[%d].x, CONST[0][%d].%c%c%c%c", r->reg, word / 4, "xyzw"[word % 4],
                 "xyzw"[word % 4], "xyzw"[word % 4], "xyzw"[word % 4]);
            return;
        }
        const char *target =
            (a->image_ms && a->image_kind == 0) ? "2D_ARRAY"
            : a->image_ms ? (a->sampler == PIPE_TEXTURE_2D_ARRAY ? "2D_ARRAY_MSAA" : "2D_MSAA")
            : a->sampler == ZVK_BUFFER_SAMPLER    ? "BUFFER"
            : a->sampler == PIPE_TEXTURE_1D       ? "1D"
            : a->sampler == PIPE_TEXTURE_1D_ARRAY ? "1D_ARRAY"
            : a->sampler == PIPE_TEXTURE_3D       ? "3D"
            : a->sampler == PIPE_TEXTURE_CUBE     ? "CUBE"
            : a->sampler == PIPE_TEXTURE_2D_ARRAY ? "2D_ARRAY"
                                                  : "2D";
        texture_used(c,a,0);
        int t = temporary(c, 1);
        emit(c, "MOV TEMP[%d], IMM[%d]", t, scalar_imm(c, 0));
        if (op == SpvOpImageQuerySizeLod) {
            char lod[64];
            source(c, p[4], 0, lod);
            emit(c, "MOV TEMP[%d].x, %s", t, lod);
        }
        emit(c, "TXQ TEMP[%d].%s, TEMP[%d], SAMP[%d], %s", t, mask(width(c, r->type)), t, a->slot,
             target);
        if (a->image_ms && a->image_kind == 0) {
            c->shader->sample_queries |= 1u << a->slot;
            int word = (ZVK_PUSH + 16) / 4 + 2 * ZVK_SAMPLERS + a->slot;
            emit(c, "MOV TEMP[%d].y, CONST[0][%d].%c%c%c%c", t, word / 4, "xyzw"[word % 4],
                 "xyzw"[word % 4], "xyzw"[word % 4], "xyzw"[word % 4]);
        }
        emit(c, "MOV TEMP[%d].%s, TEMP[%d]%s", r->reg, mask(width(c, r->type)), t,
             op == SpvOpImageQueryLevels ? ".wwww" : "");
        return;
    }
    case SpvOpImageFetch: {
        minimum(c, p, 5);
        struct id *r = get(c, p[2]), *a = get(c, p[3]);
        if (a->sampler == ZVK_BUFFER_SAMPLER) {
            if (wc(p) != 5 && !(wc(p) == 6 && p[5] == 0))
                fail(c, "buffer fetch operands");
            buffer_fetch(c, r, a, p[4]);
            return;
        }
        minimum(c, p, 7);
        texture_used(c,a,0);
        uint32_t operands = p[5],
                 base = a->image_ms ? SpvImageOperandsSampleMask : SpvImageOperandsLodMask;
        int has_offset = !!(operands & SpvImageOperandsConstOffsetMask);
        if (!a->sampler || wc(p) != 7 + has_offset ||
            (operands & ~SpvImageOperandsConstOffsetMask) != base || (a->image_ms && has_offset))
            fail(c, "unsupported image fetch operands");
        char offset[64] = "";
        if (has_offset)
            image_offset(c, a, p[7], offset);
        const char *target =
            (a->image_ms && a->image_kind == 0) ? "2D_ARRAY"
            : a->image_ms ? (a->sampler == PIPE_TEXTURE_2D_ARRAY ? "2D_ARRAY_MSAA" : "2D_MSAA")
            : a->sampler == ZVK_BUFFER_SAMPLER    ? "BUFFER"
            : a->sampler == PIPE_TEXTURE_1D       ? "1D"
            : a->sampler == PIPE_TEXTURE_1D_ARRAY ? "1D_ARRAY"
            : a->sampler == PIPE_TEXTURE_3D       ? "3D"
            : a->sampler == PIPE_TEXTURE_2D_ARRAY ? "2D_ARRAY"
                                                  : "2D";
        int t = temporary(c, 1);
        char coord[64], extra[64];
        source(c, p[4], 0, coord);
        source(c, p[6], 0, extra);
        emit(c, "MOV TEMP[%d], %s", t, coord);
        emit(c, "MOV TEMP[%d].w, %s", t, extra);
        flatten_uint_ms(c, a, t);
        emit(c, "TXF TEMP[%d], TEMP[%d], SAMP[%d], %s%s", r->reg, t, a->slot, target, offset);
        image_mapping(c, r, a);
        return;
    }
    case SpvOpImageRead: {
        minimum(c, p, 5);
        struct id *r = get(c, p[2]), *image = get(c, p[3]);
        texture_used(c,image,0);
        const uint32_t *type = get(c, image->type)->ins;
        if (image->sampler == ZVK_BUFFER_SAMPLER && wc(p) == 5) {
            buffer_fetch(c, r, image, p[4]);
            return;
        }
        if (opcode(type)==SpvOpTypeImage && type[3]!=SpvDimSubpassData && type[7]==2 &&
            wc(p)==(image->image_ms?7:5)) {
            if (image->image_ms && p[5]!=SpvImageOperandsSampleMask)fail(c,"storage image sample operand");
            int coord=temporary(c,1);char source_coord[64];source(c,p[4],0,source_coord);
            emit(c,"MOV TEMP[%d], IMM[%d]",coord,scalar_imm(c,0));
            emit(c,"MOV TEMP[%d].%s, %s",coord,mask(width(c,get(c,p[4])->type)),source_coord);
            if (image->image_ms) {
                char sample[64];source(c,p[6],0,sample);emit(c,"MOV TEMP[%d].w, %s",coord,sample);
            }
            flatten_uint_ms(c,image,coord);
            const char *target=image->image_ms?(image->image_kind==0?"2D_ARRAY":
                image->sampler==PIPE_TEXTURE_2D_ARRAY?"2D_ARRAY_MSAA":"2D_MSAA"):
                image->sampler==PIPE_TEXTURE_1D?"1D":image->sampler==PIPE_TEXTURE_1D_ARRAY?"1D_ARRAY":
                image->sampler==PIPE_TEXTURE_2D_ARRAY?"2D_ARRAY":image->sampler==PIPE_TEXTURE_3D?"3D":"2D";
            emit(c,"TXF TEMP[%d], TEMP[%d], SAMP[%d], %s",r->reg,coord,image->slot,target);
            return;
        }
        if ((wc(p) != (image->image_ms ? 7 : 5)) ||
            (image->image_ms && p[5] != SpvImageOperandsSampleMask) ||
            opcode(type) != SpvOpTypeImage || type[3] != SpvDimSubpassData || !image->sampler ||
            c->stage != VK_SHADER_STAGE_FRAGMENT_BIT)
            fail(c, "unsupported image read");
        if (c->fragcoord < 0) {
            c->fragcoord = c->nin++;
            textf(c, &c->declarations, "DCL IN[%d], POSITION, LINEAR\n", c->fragcoord);
        }
        int coord = temporary(c, 1);
        emit(c, "MOV TEMP[%d], IMM[%d]", coord, scalar_imm(c, 0));
        emit(c, "F2I TEMP[%d].xy, IN[%d].xyxy", coord, c->fragcoord);
        if (image->image_ms) {
            char sample[64];
            source(c, p[6], 0, sample);
            emit(c, "MOV TEMP[%d].w, %s", coord, sample);
        }
        flatten_uint_ms(c, image, coord);
        emit(c, "TXF TEMP[%d], TEMP[%d], SAMP[%d], %s", r->reg, coord, image->slot,
             image->image_ms ? (image->image_kind == 0 ? "2D_ARRAY" : "2D_MSAA") : "2D");
        image_mapping(c, r, image);
        return;
    }
    case SpvOpImageGather:
    case SpvOpImageDrefGather: {
        minimum(c,p,6);
        struct id *r=get(c,p[2]),*sampler=get(c,p[3]);
        texture_used(c,sampler,1);
        int shadow=op==SpvOpImageDrefGather;
        const char *target=sampler->sampler==PIPE_TEXTURE_CUBE?"CUBE":
                           sampler->sampler==PIPE_TEXTURE_2D_ARRAY?"2D_ARRAY":"2D";
        if (sampler->sampler!=PIPE_TEXTURE_CUBE && sampler->sampler!=PIPE_TEXTURE_2D_ARRAY &&
            sampler->sampler!=PIPE_TEXTURE_2D)fail(c,"gather image target");
        char shadow_target[32];
        if (shadow){snprintf(shadow_target,sizeof shadow_target,"SHADOW%s",target);target=shadow_target;}
        char raw[64],component[64],offset[64]="";source(c,p[4],0,raw);
        int coord=temporary(c,1);emit(c,"MOV TEMP[%d], %s",coord,raw);
        if (shadow) {
            char ref[64];source(c,p[5],0,ref);
            emit(c,"MOV TEMP[%d].%c, %s",coord,sampler->sampler==PIPE_TEXTURE_2D?'z':'w',ref);
            snprintf(component,sizeof component,"IMM[%d]",scalar_imm(c,0));
        } else {
            if (constant(c,p[5])>3)fail(c,"gather component out of range");
            snprintf(component,sizeof component,"IMM[%d]",scalar_imm(c,constant(c,p[5])));
        }
        if (wc(p)>6) {
            if (wc(p)!=8 || p[6]!=SpvImageOperandsConstOffsetMask)
                fail(c,"unsupported gather image operands");
            image_offset(c,sampler,p[7],offset);
        }
        emit(c,"TG4 TEMP[%d], TEMP[%d], %s, SAMP[%d], %s%s",r->reg,coord,component,sampler->slot,target,offset);
        return;
    }
    case SpvOpImageSampleImplicitLod:
    case SpvOpImageSampleExplicitLod:
    case SpvOpImageSampleDrefImplicitLod:
    case SpvOpImageSampleDrefExplicitLod:
    case SpvOpImageSampleProjImplicitLod:
    case SpvOpImageSampleProjExplicitLod:
    case SpvOpImageSampleProjDrefImplicitLod:
    case SpvOpImageSampleProjDrefExplicitLod: {
        int shadow=op==SpvOpImageSampleDrefImplicitLod || op==SpvOpImageSampleDrefExplicitLod ||
                   op==SpvOpImageSampleProjDrefImplicitLod || op==SpvOpImageSampleProjDrefExplicitLod;
        int projected=op==SpvOpImageSampleProjImplicitLod || op==SpvOpImageSampleProjExplicitLod ||
                      op==SpvOpImageSampleProjDrefImplicitLod || op==SpvOpImageSampleProjDrefExplicitLod;
        int explicit_lod=op==SpvOpImageSampleExplicitLod || op==SpvOpImageSampleDrefExplicitLod ||
                         op==SpvOpImageSampleProjExplicitLod || op==SpvOpImageSampleProjDrefExplicitLod;
        int first=shadow?6:5;
        minimum(c,p,first);
        uint32_t operands=wc(p)>first?p[first]:0;
        uint32_t allowed = SpvImageOperandsBiasMask | SpvImageOperandsLodMask |
                           SpvImageOperandsGradMask | SpvImageOperandsConstOffsetMask;
        if (operands & ~allowed)
            fail(c, "unsupported image operands");
        int extra = first+1, lod = -1, bias = -1, dx = -1, dy = -1;
        if (operands & SpvImageOperandsBiasMask) {
            minimum(c, p, extra + 1);
            bias = p[extra++];
        }
        if (operands & SpvImageOperandsLodMask) {
            minimum(c, p, extra + 1);
            lod = p[extra++];
        }
        if (operands & SpvImageOperandsGradMask) {
            minimum(c, p, extra + 2);
            dx = p[extra++];
            dy = p[extra++];
        }
        struct id *r = get(c, p[2]), *sampler = get(c, p[3]);
        texture_used(c,sampler,1);
        if (!sampler->sampler || sampler->image_ms)
            fail(c, "invalid sampled image");
        char offset[64] = "";
        if (operands & SpvImageOperandsConstOffsetMask) {
            minimum(c, p, extra + 1);
            image_offset(c, sampler, p[extra++], offset);
        }
        if ((wc(p) > first && wc(p) != extra) ||
            (explicit_lod ? ((lod < 0) == (dx < 0) || bias >= 0) : lod >= 0 || dx >= 0))
            fail(c, "invalid image LOD/gradient operands");
        const char *target = sampler->sampler == PIPE_TEXTURE_1D         ? "1D"
                             : sampler->sampler == PIPE_TEXTURE_1D_ARRAY ? "1D_ARRAY"
                             : sampler->sampler == PIPE_TEXTURE_3D       ? "3D"
                             : sampler->sampler == PIPE_TEXTURE_CUBE     ? "CUBE"
                             : sampler->sampler == PIPE_TEXTURE_2D_ARRAY ? "2D_ARRAY"
                                                                         : "2D";
        char shadow_target[32];
        if (shadow) {
            snprintf(shadow_target,sizeof shadow_target,"SHADOW%s",target);
            target=shadow_target;
        }
        int coordinate=temporary(c,1);
        char coord[64],raw[64];
        source(c,p[4],0,raw);
        emit(c,"MOV TEMP[%d], %s",coordinate,raw);
        if (projected) {
            int q=width(c,get(c,p[4])->type)-1;
            emit(c,"DIV TEMP[%d], TEMP[%d], TEMP[%d].%c%c%c%c",coordinate,coordinate,coordinate,
                 "xyzw"[q],"xyzw"[q],"xyzw"[q],"xyzw"[q]);
        }
        if (!shadow && (sampler->sampler==PIPE_TEXTURE_1D || sampler->sampler==PIPE_TEXTURE_2D)) {
            c->shader->sample_queries|=1u<<sampler->slot;
            int flag=temporary(c,1),size=temporary(c,1);
            int word=(ZVK_PUSH+16)/4+3*ZVK_SAMPLERS+sampler->slot;
            char lane="xyzw"[word%4];
            emit(c,"USNE TEMP[%d].x, CONST[0][%d].%c%c%c%c, IMM[%d]",flag,word/4,lane,lane,lane,lane,scalar_imm(c,0));
            emit(c,"UIF TEMP[%d].xxxx",flag);
            const char *dimensions=sampler->sampler==PIPE_TEXTURE_1D?"x":"xy";
            emit(c,"TXQ TEMP[%d].%s, IMM[%d], SAMP[%d], %s",size,dimensions,scalar_imm(c,0),sampler->slot,target);
            emit(c,"U2F TEMP[%d].%s, TEMP[%d]",size,dimensions,size);
            emit(c,"DIV TEMP[%d].%s, TEMP[%d], TEMP[%d]",coordinate,
                 sampler->sampler==PIPE_TEXTURE_1D?"x":"xy",coordinate,size);
            emit(c,"ENDIF");
        }
        if (shadow) {
            char reference[64];source(c,p[5],0,reference);
            char dest=sampler->sampler==PIPE_TEXTURE_2D_ARRAY || sampler->sampler==PIPE_TEXTURE_CUBE?'w':'z';
            if (projected) {
                unsigned q=width(c,get(c,p[4])->type)-1;char lane="xyzw"[q];
                emit(c,"DIV TEMP[%d].%c, %s, TEMP[%d].%c%c%c%c",coordinate,dest,reference,
                     get(c,p[4])->reg,lane,lane,lane,lane);
            } else emit(c,"MOV TEMP[%d].%c, %s",coordinate,dest,reference);
        }
        snprintf(coord,sizeof coord,"TEMP[%d]",coordinate);
        if (dx >= 0) {
            char x[64], y[64];
            source(c, dx, 0, x);
            source(c, dy, 0, y);
            emit(c, "TXD TEMP[%d], %s, %s, %s, SAMP[%d], %s%s", r->reg, coord, x, y, sampler->slot,
                 target, offset);
        } else if (lod >= 0 || bias >= 0) {
            int t = temporary(c, 1);
            char value[64];
            source(c, lod >= 0 ? lod : bias, 0, value);
            emit(c, "MOV TEMP[%d], %s", t, coord);
            if (shadow && (sampler->sampler==PIPE_TEXTURE_2D_ARRAY || sampler->sampler==PIPE_TEXTURE_CUBE)) {
                emit(c,"%s TEMP[%d], TEMP[%d], %s, SAMP[%d], %s%s",lod>=0?"TXL2":"TXB2",r->reg,t,value,sampler->slot,target,offset);
            } else {
                emit(c,"MOV TEMP[%d].w, %s",t,value);
                emit(c,"%s TEMP[%d], TEMP[%d], SAMP[%d], %s%s",lod>=0?"TXL":"TXB",r->reg,t,sampler->slot,target,offset);
            }
        } else
            emit(c, "TEX TEMP[%d], %s, SAMP[%d], %s%s", r->reg, coord, sampler->slot, target,
                 offset);
        return;
    }
    case SpvOpFunctionCall: {
        minimum(c, p, 4);
        struct id *fn = get(c, p[3]);
        const uint32_t *arg = fn->ins + wc(fn->ins);
        int index = 4;
        for (;arg<c->code+c->words;arg+=wc(arg)) {
            if (opcode(arg)==SpvOpLine || opcode(arg)==SpvOpNoLine)continue;
            if (opcode(arg)!=SpvOpFunctionParameter)break;
            if (index >= wc(p))
                fail(c, "missing function argument");
            struct id *dst = get(c, arg[2]), *src = get(c, p[index++]);
            SpvOp type=opcode(get(c,dst->type)->ins);
            if (type==SpvOpTypePointer || type==SpvOpTypeSampler)
                dst->ref=src->ref;
            else if (type==SpvOpTypeImage || type==SpvOpTypeSampledImage) {
                dst->slot=src->slot;dst->sampler=src->sampler;dst->sampler_slot=src->sampler_slot;
                dst->image_ms=src->image_ms;dst->image_kind=src->image_kind;
            } else
                copy(c, dst->reg, src->reg, dst->type);
        }
        if (index != wc(p))
            fail(c, "too many function arguments");
        function_emit(c, p[3], get(c, p[2])->reg, depth + 1);
        return;
    }
    default:
        fail(c, "unsupported instruction %s (%u)", SpvOpToString(op), (unsigned)op);
    }
    if (operation) {
        minimum(c, p, 5);
        binary(c, operation, (uint32_t)result, p[3], p[4]);
    }
}
#include "spirv_flow.h"
void zvk_shader_finish(zvk_shader *shader)
{
    free(shader->tgsi);
    free(shader->metal);
    memset(shader, 0, sizeof *shader);
}
VkResult zvk_spirv(const uint32_t *code, size_t words, const char *entry,
                   VkShaderStageFlagBits stage, const VkSpecializationInfo *specialization,
                   zvk_shader *shader)
{
    return zvk_spirv_graphics(code,words,entry,stage,specialization,0,shader);
}
VkResult zvk_spirv_graphics(const uint32_t *code, size_t words, const char *entry,
                   VkShaderStageFlagBits stage, const VkSpecializationInfo *specialization,
                   unsigned fixed_depth_bias,zvk_shader *shader)
{
    memset(shader, 0, sizeof *shader);
    if (zvk_shader_check(code, words) != VK_SUCCESS)
        return VK_ERROR_INVALID_SHADER_NV;
    if (stage != VK_SHADER_STAGE_VERTEX_BIT && stage != VK_SHADER_STAGE_FRAGMENT_BIT) {
        zvk_error("SPIR-V: only vertex and fragment stages are supported");
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    struct compiler *c = calloc(1, sizeof *c);
    if (!c)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    VkResult group_result=zvk_spirv_groups(code,words,&c->expanded,&words);
    if (group_result!=VK_SUCCESS){free(c);return group_result;}
    if (c->expanded)code=c->expanded;
    c->code = code;
    c->words = words;
    c->bound = code[3];
    c->stage = stage;
    c->fragcoord = -1;
    c->bias_depth = -1;
    shader->fixed_depth_bias=!!fixed_depth_bias && stage==VK_SHADER_STAGE_FRAGMENT_BIT;
    for (unsigned i=0;i<4;i++) c->color_io[i]=-1;
    c->shader = shader;
    c->temps = 16; /* private intrinsic result and argument registers */
    VkResult ids_result=zvk_spirv_ids(code,words,&c->map);
    if (ids_result!=VK_SUCCESS){free(c->expanded);free(c);return ids_result;}
    c->ids=calloc(c->map.count,sizeof *c->ids);
    if (!c->ids) {
        free(c->map.ids);
        free(c->expanded);
        free(c);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    for (uint32_t i=0;i<c->map.count;i++) {
        c->ids[i].d = decoration();
        c->ids[i].reg = -1;
    }
    VkResult result = VK_SUCCESS;
    if (setjmp(c->error)) {
        result = VK_ERROR_INVALID_SHADER_NV;
        goto done;
    }
    parse(c, entry ? entry : "main", specialization);
    constants(c);
    c->out = &c->body;
    function_emit(c, c->entry, -1, 0);
    if(shader->fixed_depth_bias) {
        c->out=&c->prolog;
        c->bias_depth=temporary(c,2);
        if(c->fragcoord<0) {
            c->fragcoord=c->nin++;
            textf(c,&c->declarations,"DCL IN[%d], POSITION, LINEAR\n",c->fragcoord);
        }
        int t=c->bias_depth;
        emit(c,"DDX TEMP[%d].x, IN[%d].zzzz",t+1,c->fragcoord);
        emit(c,"DDY TEMP[%d].y, IN[%d].zzzz",t+1,c->fragcoord);
        emit(c,"ABS TEMP[%d].xy, TEMP[%d]",t+1,t+1);
        emit(c,"MAX TEMP[%d].x, TEMP[%d].xxxx, TEMP[%d].yyyy",t+1,t+1,t+1);
        emit(c,"MAD TEMP[%d].x, TEMP[%d].xxxx, CONST[0][%d].wwww, CONST[0][%d].zzzz",t,t+1,ZVK_PUSH/16,ZVK_PUSH/16);
        emit(c,"ADD_SAT TEMP[%d].x, IN[%d].zzzz, TEMP[%d].xxxx",t,c->fragcoord,t);
    }
    for (uint32_t dense=0;dense<c->map.count;dense++) {
        uint32_t id=c->map.ids[dense];
        struct id *n = &(*lookup(c,id));
        if (n->ins && opcode(n->ins) == SpvOpVariable &&
            (n->used || (n->entry_interface && n->ins[3] == SpvStorageClassOutput)))
            io_type(c, id, n->ref.type, 0, n->d);
    }
    if(shader->fixed_depth_bias && !c->writes_depth) {
        int io=c->nout++;
        textf(c,&c->declarations,"DCL OUT[%d], POSITION\n",io);
        c->out=&c->epilog;
        emit(c,"MOV OUT[%d].z, TEMP[%d].xxxx",io,c->bias_depth);
    }
    /* Only statically accessed resources impose descriptor requirements. */
    uint32_t used = 0;
    for (uint32_t i = 0; i < shader->nbindings; i++) {
        zvk_shader_binding b = shader->bindings[i];
        int accessed = 0;
        for (uint32_t dense=0;dense<c->map.count;dense++) {
        uint32_t id=c->map.ids[dense];
            struct id *n = &(*lookup(c,id));
            if (n->ins && opcode(n->ins) == SpvOpVariable && n->accessed &&
                n->d.set == (int)b.set && n->d.binding == (int)b.binding &&
                (n->accessed & (1u << b.element)))
                accessed = 1;
        }
        if (accessed)
            shader->bindings[used++] = b;
    }
    shader->nbindings = used;
    textf(c, &c->output, "%s\n", stage == VK_SHADER_STAGE_VERTEX_BIT ? "VERT" : "FRAG");
    if (stage == VK_SHADER_STAGE_FRAGMENT_BIT)
        textf(c, &c->output, "PROPERTY FS_COORD_ORIGIN UPPER_LEFT\n");
    if (c->declarations.data)
        textf(c, &c->output, "%s", c->declarations.data);
    if (c->temps) {
        int cursor=0,array=1;
        for (uint32_t i=0;i<c->map.count;i++) {
            struct id *n=c->ids+i;
            if (!n->indirect)continue;
            if (cursor<n->reg)textf(c,&c->output,"DCL TEMP[%d..%d]\n",cursor,n->reg-1);
            int end=n->reg+registers(c,n->ref.type)-1;
            textf(c,&c->output,"DCL TEMP[%d..%d], ARRAY(%d)\n",n->reg,end,array++);
            cursor=end+1;
        }
        if (cursor<c->temps)textf(c,&c->output,"DCL TEMP[%d..%d]\n",cursor,c->temps-1);
        if (array>1)textf(c,&c->output,"DCL ADDR[0]\n");
    }
    if (shader->push_bytes || shader->instance_index || shader->sample_mask_in || shader->sample_mask_out || shader->sample_queries || shader->fixed_depth_bias)
        textf(c, &c->output, "DCL CONST[0][0..%u]\n",
              shader->sample_queries   ? (ZVK_PUSH + 16 + ZVK_SAMPLERS * 16) / 16 - 1
              : shader->instance_index || shader->sample_mask_in || shader->sample_mask_out || shader->fixed_depth_bias ? ZVK_PUSH / 16
                                       : (shader->push_bytes + 15) / 16 - 1);
    for (uint32_t i = 0; i < shader->nbindings; i++) {
        zvk_shader_binding *b = shader->bindings + i;
        if (!shader->resource_atlas && !constant_binding(b->type) && b->texture_use)
            textf(c, &c->output, "DCL SAMP[%u]\nDCL SVIEW[%u], %s, %s\n", b->slot, b->slot,
                  (b->image_ms && b->image_kind == 0) ? "2D_ARRAY"
                  : b->image_ms ? (b->target == PIPE_TEXTURE_2D_ARRAY ? "2D_ARRAY_MSAA" : "2D_MSAA")
                  : b->target == ZVK_BUFFER_SAMPLER    ? "2D"
                  : b->target == PIPE_TEXTURE_1D       ? "1D"
                  : b->target == PIPE_TEXTURE_1D_ARRAY ? "1D_ARRAY"
                  : b->target == PIPE_TEXTURE_3D       ? "3D"
                  : b->target == PIPE_TEXTURE_CUBE     ? "CUBE"
                  : b->target == PIPE_TEXTURE_2D_ARRAY ? "2D_ARRAY"
                                                       : "2D",
                  b->image_kind == 0   ? "UINT"
                  : b->image_kind == 1 ? "SINT"
                                       : "FLOAT");
        else if (constant_binding(b->type) && !shader->resource_atlas)
            textf(c, &c->output, "DCL CONST[%u][0..%u]\n", b->slot, (b->bytes + 15) / 16 - 1);
    }
    if (shader->resource_atlas) {
        textf(c,&c->output,"DCL SAMP[15]\nDCL SVIEW[15], 2D_ARRAY, UINT\n");
    }
    for (int i = 0; i < c->nimms; i++)
        textf(c, &c->output, "IMM[%d] UINT32 {%u, %u, %u, %u}\n", i, c->imm[i][0], c->imm[i][1],
              c->imm[i][2], c->imm[i][3]);
    textf(c, &c->output, "%s%s%sEND\n", c->prolog.data ? c->prolog.data : "",
          c->body.data ? c->body.data : "", c->epilog.data ? c->epilog.data : "");
    shader->tgsi = c->output.data;
    c->output.data = NULL;
    if (getenv("ZVK_DEBUG_SHADERS"))
        fprintf(stderr, "zvk SPIR-V %s:\n%s",
                stage == VK_SHADER_STAGE_VERTEX_BIT ? "vertex" : "fragment", shader->tgsi);
done:
    for (unsigned i=0;i<ZVK_ARRAY_SIZE(c->flow_alloc);i++)free(c->flow_alloc[i]);
    free(c->output.data);
    for (uint32_t i=0;i<c->map.count;i++)
        free(c->ids[i].members);
    free(c->ids);
    free(c->map.ids);
    free(c->body.data);
    free(c->prolog.data);
    free(c->epilog.data);
    free(c->declarations.data);
    free(c->expanded);
    free(c);
    if (result != VK_SUCCESS)
        zvk_shader_finish(shader);
    return result;
}
