/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Native Metal stage interfaces for the independent SPIR-V compiler. The
 * graphics entry point is not advertised until its transport is integrated. */
struct metal_io {
    unsigned root,word,type,storage;
    int location,component,builtin,flat,noperspective,centroid;
};
/* Location aliases share a numerical type and interpolation qualification:
 * https://docs.vulkan.org/glsl/latest/chapters/variables.html */
struct metal_io_slots {
    unsigned masks[16],qualifiers[16];
    const char *scalars[16];
};
static const char *metal_stage_name(struct cc *c)
{
    return c->stage==VK_SHADER_STAGE_VERTEX_BIT?"Vertex":"Fragment";
}
static const char *metal_scalar(struct cc *c,unsigned type)
{
    const uint32_t *p=node(c,type)->p;
    if (cop(p)==SpvOpTypeVector)p=node(c,p[2])->p;
    if (cop(p)==SpvOpTypeFloat)return "float";
    if (cop(p)==SpvOpTypeInt)return p[3]?"int":"uint";
    if (cop(p)==SpvOpTypeBool)return "bool";
    cfail(c,"graphics interface scalar type");return NULL;
}
static const char *metal_builtin(struct cc *c,int builtin)
{
    switch (builtin) {
    case SpvBuiltInVertexIndex:
        if (c->stage==VK_SHADER_STAGE_VERTEX_BIT)return "vertex_index";
        break;
    case SpvBuiltInInstanceIndex:
        if (c->stage==VK_SHADER_STAGE_VERTEX_BIT)return "instance_index";
        break;
    case SpvBuiltInFragCoord:return "fragment_coord";
    case SpvBuiltInFrontFacing:return "front_facing";
    case SpvBuiltInPointCoord:return "point_coord";
    case SpvBuiltInSampleId:return "sample_index";
    case SpvBuiltInSampleMask:return "sample_mask";
    default:break;
    }
    cfail(c,"unsupported graphics input builtin %d",builtin);return NULL;
}
static void metal_io_leaf(struct cc *c,struct metal_io io,unsigned mode,unsigned *count,struct metal_io_slots *slots)
{
    int input=io.storage==SpvStorageClassInput;
    int fragment=c->stage==VK_SHADER_STAGE_FRAGMENT_BIT;
    if ((mode==0||mode==2||mode==4)!=input)return;
    if (io.builtin==SpvBuiltInClipDistance || io.builtin==SpvBuiltInCullDistance)return;
    if (!input && c->early_tests && io.builtin==SpvBuiltInFragDepth)return;
    unsigned width=components(c,io.type);
    const char *scalar=metal_scalar(c,io.type);
    char suffix[2]={0};if(width>1)suffix[0]='0'+width;
    const char *attribute=NULL;
    if (input && io.builtin>=0) {
        const char *source=metal_builtin(c,io.builtin);
        if (mode==4) {
            if (fragment && io.builtin!=SpvBuiltInFragCoord) {
                const char *type=io.builtin==SpvBuiltInFrontFacing?"bool":
                    io.builtin==SpvBuiltInFragCoord?"float4":io.builtin==SpvBuiltInPointCoord?"float2":"uint";
                const char *attr=io.builtin==SpvBuiltInFragCoord?"position":
                    io.builtin==SpvBuiltInFrontFacing?"front_facing":
                    io.builtin==SpvBuiltInPointCoord?"point_coord":
                    io.builtin==SpvBuiltInSampleId?"sample_id":"sample_mask";
                emit(c,"%s %s [[%s]],",type,source,attr);
            }
        } else if (mode==2)for(unsigned k=0;k<width;k++) {
            if (io.builtin==SpvBuiltInFrontFacing)
                emit(c,"m%u[%u]=%s?0xffffffffu:0u;\n",io.root,io.word+k,source);
            else
                emit(c,"m%u[%u]=as_type<uint>(%s%s);\n",io.root,io.word+k,source,
                    width>1?(const char *[]) {".x",".y",".z",".w"}[k]:"");
        }
        return;
    }
    if (io.builtin>=0) {
        if (!fragment && io.builtin==SpvBuiltInPosition)attribute="position";
        else if (!fragment && io.builtin==SpvBuiltInPointSize)attribute="point_size";
        else if (fragment && io.builtin==SpvBuiltInFragDepth)attribute="depth(any)";
        else if (fragment && io.builtin==SpvBuiltInSampleMask) {attribute="sample_mask";scalar="uint";}
        else cfail(c,"unsupported graphics output builtin %d",io.builtin);
    } else if (io.location<0 || io.location>=16)
        cfail(c,"graphics interface location missing or out of range");
    int user=io.builtin<0;
    char field[48];
    if(user) {
        if(io.component<0 || io.component+(int)width>4)cfail(c,"graphics interface component range");
        snprintf(field,sizeof field,"loc%d",io.location);
    } else snprintf(field,sizeof field,"io%u_%u",io.root,io.word);
    if (mode==0||mode==1) {
        if (input && !fragment)c->out->inputs|=1u<<io.location;
        if(user) {
            unsigned mask=((1u<<width)-1)<<io.component;
            unsigned qualifier=io.flat|(io.noperspective<<1)|(io.centroid<<2);
            unsigned old=slots->masks[io.location];
            if(old&mask)cfail(c,"overlapping graphics interface components");
            if(old && (strcmp(slots->scalars[io.location],scalar) || slots->qualifiers[io.location]!=qualifier))
                cfail(c,"inconsistent graphics location type/interpolation");
            slots->masks[io.location]|=mask;
            slots->scalars[io.location]=scalar;slots->qualifiers[io.location]=qualifier;
            if(old)return;
        }
        emit(c,"%s%s %s [[",scalar,user?"4":suffix,field);
        if (attribute)emit(c,"%s",attribute);
        else if (input&&!fragment)emit(c,"attribute(%d)",io.location);
        else if (!input&&fragment)emit(c,"color(%d)",io.location);
        else emit(c,"user(locn%d)",io.location);
        if (input&&fragment) {
            if (io.flat || strcmp(scalar,"float"))emit(c,",flat");
            else if(io.centroid)emit(c,io.noperspective?",centroid_no_perspective":",centroid_perspective");
            else if(io.noperspective)emit(c,",center_no_perspective");
        }
        emit(c,"]];\n");(*count)++;
    } else if (mode==2) {
        for(unsigned k=0;k<width;k++)
            emit(c,"m%u[%u]=as_type<uint>(stage.%s%s);\n",io.root,io.word+k,
                field,user?(const char *[]) {".x",".y",".z",".w"}[io.component+k]:
                    width>1?(const char *[]) {".x",".y",".z",".w"}[k]:"");
    } else if(mode==3) {
        for(unsigned k=0;k<width;k++)
            emit(c,"result.%s%s=%sas_type<%s>(m%u[%u]);\n",field,
                user?(const char *[]) {".x",".y",".z",".w"}[io.component+k]:
                    width>1?(const char *[]) {".x",".y",".z",".w"}[k]:"",
                !fragment&&io.builtin==SpvBuiltInPosition&&k==1?"-":"",scalar,io.root,io.word+k);
    }
}
static unsigned metal_io_walk(struct cc *c,struct metal_io io,unsigned mode,unsigned *count,unsigned depth,struct metal_io_slots *slots)
{
    if (depth>24)cfail(c,"graphics interface nesting");
    const uint32_t *p=node(c,io.type)->p;
    if (components(c,io.type)) {
        metal_io_leaf(c,io,mode,count,slots);return io.builtin<0?1:0;
    }
    if (io.builtin==SpvBuiltInClipDistance || io.builtin==SpvBuiltInCullDistance)return 0;
    if (cop(p)==SpvOpTypeStruct) {
        struct cn *n=node(c,io.type);n->io_block=1;
        unsigned locations=0;
        for(unsigned i=2;i<cnw(p);i++) {
            struct metal_io part=io;part.type=p[i];
            part.word+=member_offset(c,io.type,i-2,0)/4;
            if(n->member_builtin[i-2]>=0)part.builtin=n->member_builtin[i-2];
            part.location=n->member_location[i-2]>=0?n->member_location[i-2]:
                io.location<0?-1:io.location+locations;
            part.component=n->member_component[i-2];
            part.flat|=(n->flat_members>>(i-2))&1;
            part.noperspective|=(n->noperspective_members>>(i-2))&1;
            part.centroid|=(n->centroid_members>>(i-2))&1;
            unsigned span=metal_io_walk(c,part,mode,count,depth+1,slots);
            if(span) {
                unsigned base=io.location<0?0:(unsigned)io.location;
                if(part.location>=0 && (unsigned)part.location<base)cfail(c,"graphics interface member location range");
                unsigned end=part.location<0?locations+span:(unsigned)part.location-base+span;
                if(end>locations)locations=end;
            }
        }
        return locations;
    }
    if(cop(p)==SpvOpTypeArray || cop(p)==SpvOpTypeMatrix) {
        unsigned length=cop(p)==SpvOpTypeArray?literal(c,p[3]):p[3],locations=0;
        unsigned stride=cop(p)==SpvOpTypeArray&&node(c,io.type)->stride?
            node(c,io.type)->stride:bytes(c,p[2],0);
        if(length>64 || (stride&3))cfail(c,"graphics interface array size/stride");
        for(unsigned i=0;i<length;i++) {
            struct metal_io part=io;part.type=p[2];part.word+=i*stride/4;
            if(io.location>=0)part.location+=locations;
            locations+=metal_io_walk(c,part,mode,count,depth+1,slots);
        }
        return locations;
    }
    cfail(c,"unsupported graphics interface aggregate");return 0;
}
static unsigned metal_io_emit(struct cc *c,unsigned mode)
{
    unsigned count=0;
    struct metal_io_slots slots={0};
    for(unsigned dense=0;dense<c->map.count;dense++) {
        struct cn *n=c->n+dense;
        if(!n->p || cop(n->p)!=SpvOpVariable || !n->active || n->function ||
            (n->p[3]!=SpvStorageClassInput&&n->p[3]!=SpvStorageClassOutput))continue;
        struct metal_io io={.root=c->map.ids[dense],.type=n->pointee,.storage=n->p[3],
            .location=n->location,.component=n->component,.builtin=n->builtin,.flat=n->flat,.noperspective=n->noperspective,.centroid=n->centroid};
        metal_io_walk(c,io,mode,&count,0,&slots);
    }
    return count;
}
static void metal_graphics_signature(struct cc *c,unsigned inputs,unsigned outputs)
{
    int vertex=c->stage==VK_SHADER_STAGE_VERTEX_BIT;
    if(c->early_tests)emit(c,"[[early_fragment_tests]] ");
    emit(c,"%s ",vertex?"vertex":"fragment");
    if(outputs)emit(c,"Zvk%sOut ",metal_stage_name(c));else emit(c,"void ");
    emit(c,"zvk_%s(",vertex?"vertex":"fragment");
    if(inputs)emit(c,"Zvk%sIn stage [[stage_in]],",metal_stage_name(c));
    emit(c,"device uint *arena [[buffer(0)]],const device zvk_info *imageinfo [[buffer(28)]],");
    if(!vertex)emit(c,"float4 fragment_coord [[position]],");
    metal_io_emit(c,4);
    if(vertex)emit(c,"uint vertex_index [[vertex_id]],uint instance_index [[instance_id]],");
    emit(c,"constant uint *physical_sizes [[buffer(29)]],constant uint *push [[buffer(30)]]) {\n");
}
