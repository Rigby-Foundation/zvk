/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Private native GPU image ABI. TEMP[4..11] carry coordinates, LOD,
 * offsets/gradients, and independent image/sampler metadata indices. */
static int atlas_image(struct compiler *c,const uint32_t *p)
{
    SpvOp op=opcode(p);
    int fetch=op==SpvOpImageFetch || op==SpvOpImageRead;
    int query=op==SpvOpImageQuerySize || op==SpvOpImageQuerySizeLod ||
        op==SpvOpImageQueryLevels || op==SpvOpImageQuerySamples || op==SpvOpImageQueryLod;
    int gather=op==SpvOpImageGather || op==SpvOpImageDrefGather;
    int sample=op>=SpvOpImageSampleImplicitLod && op<=SpvOpImageSampleProjDrefExplicitLod;
    if (!c->shader->resource_atlas || (!fetch && !query && !gather && !sample))return 0;
    minimum(c,p,query?4:5);
    struct id *image=get(c,p[3]),*result=get(c,p[2]);
    const uint32_t *type=get(c,image->type)->ins;
    if (opcode(type)==SpvOpTypeSampledImage)type=get(c,type[2])->ins;
    if (opcode(type)!=SpvOpTypeImage || !image->sampler)fail(c,"atlas image operand");
    uint32_t dim=type[3]==SpvDim1D || type[3]==SpvDimBuffer?0:
        type[3]==SpvDim2D || type[3]==SpvDimSubpassData?1:type[3]==SpvDim3D?2:3;
    uint32_t shape=dim|(type[5]?16u:0u)|(type[6]?32u:0u);
    int comparison=op==SpvOpImageDrefGather || op==SpvOpImageSampleDrefImplicitLod ||
        op==SpvOpImageSampleDrefExplicitLod || op==SpvOpImageSampleProjDrefImplicitLod ||
        op==SpvOpImageSampleProjDrefExplicitLod;
    int projected=op==SpvOpImageSampleProjImplicitLod || op==SpvOpImageSampleProjExplicitLod ||
        op==SpvOpImageSampleProjDrefImplicitLod || op==SpvOpImageSampleProjDrefExplicitLod;
    texture_used(c,image,sample || gather || op==SpvOpImageQueryLod);
    for (unsigned k=4;k<=11;k++)emit(c,"MOV TEMP[%u], IMM[%d]",k,scalar_imm(c,0));
    emit(c,"MOV TEMP[6].w, IMM[%d]",scalar_imm(c,shape));
    emit(c,"MOV TEMP[11].x, IMM[%d]",scalar_imm(c,image->sampler_slot));
    /* Any negative gather selector disables gathering. Use finite -1.0f
     * bits: 0xffffffff is a NaN, whose floatBitsToInt result is unspecified. */
    emit(c,"MOV TEMP[11].y, IMM[%d]",scalar_imm(c,0xbf800000u));
    unsigned call;
    if (query) {
        call=op==SpvOpImageQueryLod?5:4;
        if (op==SpvOpImageQuerySizeLod || op==SpvOpImageQueryLod) {
            minimum(c,p,5);char value[64];source(c,p[4],0,value);
            emit(c,"MOV TEMP[%u], %s",op==SpvOpImageQueryLod?4:5,value);
        }
        unsigned which=op==SpvOpImageQueryLevels?1:op==SpvOpImageQuerySamples?2:0;
        emit(c,"MOV TEMP[5].w, IMM[%d]",scalar_imm(c,which));
    } else {
        call=fetch?2:3;
        char coord[64];source(c,p[4],0,coord);
        emit(c,"MOV TEMP[4].%s, %s",mask(width(c,get(c,p[4])->type)),coord);
        if (fetch && type[3]==SpvDimSubpassData) {
            if (c->fragcoord<0) {
                c->fragcoord=c->nin++;
                textf(c,&c->declarations,"DCL IN[%d], POSITION, LINEAR\n",c->fragcoord);
            }
            emit(c,"F2I TEMP[4].xy, IN[%d].xyxy",c->fragcoord);
        }
        unsigned operand=(comparison||gather)?6:5;
        if (comparison) {
            minimum(c,p,6);char reference[64];source(c,p[5],0,reference);
            emit(c,"MOV TEMP[5].y, %s",reference);
        }
        if (gather) {
            minimum(c,p,6);
            unsigned component=comparison?0:constant(c,p[5]);
            if (component>3)fail(c,"atlas gather component");
            emit(c,"MOV TEMP[11].y, IMM[%d]",scalar_imm(c,component));
        }
        if (projected) {
            unsigned q=width(c,get(c,p[4])->type)-1;char lane="xyzw"[q];
            if (comparison)emit(c,"DIV TEMP[5].y, TEMP[5].yyyy, TEMP[4].%c%c%c%c",lane,lane,lane,lane);
            emit(c,"DIV TEMP[4].xyz, TEMP[4], TEMP[4].%c%c%c%c",lane,lane,lane,lane);
        }
        unsigned operands=wc(p)>(int)operand?p[operand++]:0;
        unsigned allowed=SpvImageOperandsBiasMask|SpvImageOperandsLodMask|SpvImageOperandsGradMask|
            SpvImageOperandsConstOffsetMask|SpvImageOperandsOffsetMask|SpvImageOperandsConstOffsetsMask|
            SpvImageOperandsSampleMask;
        if (operands&~allowed)fail(c,"unsupported atlas image operands");
        unsigned flags=fetch?(op==SpvOpImageFetch?1:0):8u|(comparison?16u:0u);
        if (sample || gather) {
            if (c->stage==VK_SHADER_STAGE_FRAGMENT_BIT)flags|=2u;
        }
        for (unsigned bit=1;bit<=SpvImageOperandsSampleMask;bit<<=1) {
            if (!(operands&bit))continue;
            if (operand>=(unsigned)wc(p))fail(c,"missing atlas image operand");
            unsigned id=p[operand++];char value[64];source(c,id,0,value);
            if (bit==SpvImageOperandsLodMask) {
                emit(c,"MOV TEMP[5].x, %s",value);
                /* Explicit Lod replaces derivatives, but retains sampler bias. */
                if (!fetch)flags&=~2u;
            } else if (bit==SpvImageOperandsBiasMask)emit(c,"MOV TEMP[5].z, %s",value);
            else if (bit==SpvImageOperandsGradMask) {
                emit(c,"MOV TEMP[7].%s, %s",mask(width(c,get(c,id)->type)),value);
                if (operand>=(unsigned)wc(p))fail(c,"missing atlas image gradient");
                id=p[operand++];source(c,id,0,value);
                emit(c,"MOV TEMP[8].%s, %s",mask(width(c,get(c,id)->type)),value);flags=(flags&~2u)|4u;
            } else if (bit==SpvImageOperandsConstOffsetMask || bit==SpvImageOperandsOffsetMask)
                emit(c,"MOV TEMP[6].%s, %s",mask(width(c,get(c,id)->type)),value);
            else if (bit==SpvImageOperandsSampleMask)emit(c,"MOV TEMP[5].z, %s",value);
            else if (bit==SpvImageOperandsConstOffsetsMask) {
                if (registers(c,get(c,id)->type)!=4)fail(c,"atlas gather offsets shape");
                for (unsigned k=0;k<4;k++) {
                    char lane="xyzw"[k];
                    emit(c,"MOV TEMP[9].%c, TEMP[%d].xxxx",lane,get(c,id)->reg+k);
                    emit(c,"MOV TEMP[10].%c, TEMP[%d].yyyy",lane,get(c,id)->reg+k);
                }
            }
        }
        if (operand!=(unsigned)wc(p))fail(c,"atlas image operand count");
        emit(c,"MOV TEMP[5].w, IMM[%d]",scalar_imm(c,flags));
    }
    emit(c,"CAL : %u",0x5c0000u|(call<<8)|(unsigned)image->slot);
    emit(c,"MOV TEMP[%d].%s, TEMP[0]",result->reg,mask(width(c,result->type)));
    return 1;
}
