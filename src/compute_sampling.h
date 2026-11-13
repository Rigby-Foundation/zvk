/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Project cube-map gradients onto the chosen face before measuring footprint.
 * Direction-length changes alone must not select a coarser mip level. */
static void sampling_derivatives(struct cc *c,unsigned dim)
{
    if(dim==3) {
        /* Cube derivatives are taken after projection onto the major face.
         * A change in direction length alone must not increase the footprint. */
        emit(c,"float3 a=abs(coord);uint axis=a.x>=a.y&&a.x>=a.z?0u:a.y>=a.z?1u:2u;"
            "float ma=a[axis],mx=sign(coord[axis])*dx[axis],my=sign(coord[axis])*dy[axis];"
            "float face_sign=coord[axis]>=0?1.0f:-1.0f;float2 st,sx,sy;\n"
            "if(axis==0){st=float2(-face_sign*coord.z,-coord.y);sx=float2(-face_sign*dx.z,-dx.y);sy=float2(-face_sign*dy.z,-dy.y);}\n"
            "else if(axis==1){st=float2(coord.x,face_sign*coord.z);sx=float2(dx.x,face_sign*dx.z);sy=float2(dy.x,face_sign*dy.z);}\n"
            "else{st=float2(face_sign*coord.x,-coord.y);sx=float2(face_sign*dx.x,-dx.y);sy=float2(face_sign*dy.x,-dy.y);}\n"
            "if(ma>0){sx=(sx*ma-st*mx)/(2*ma*ma);sy=(sy*ma-st*my)/(2*ma*ma);}else{sx=0;sy=0;}"
            "dx=float3(sx*float2(imageinfo[s].extent.xy),0);dy=float3(sy*float2(imageinfo[s].extent.xy),0);\n");
    } else {
        if(dim==0)emit(c,"dx.yz=0;dy.yz=0;\n");
        if(dim==1)emit(c,"dx.z=0;dy.z=0;\n");
        emit(c,"dx*=float3(imageinfo[s].extent.xyz);dy*=float3(imageinfo[s].extent.xyz);\n");
    }
}
static void sample_op(struct cc *c,const uint32_t *p)
{
    c->out->sampling=1;
    unsigned op=cop(p),id=p[2],image=p[3];
    int implicit=c->stage==VK_SHADER_STAGE_FRAGMENT_BIT &&
        (op==SpvOpImageSampleImplicitLod || op==SpvOpImageSampleDrefImplicitLod ||
         op==SpvOpImageSampleProjImplicitLod || op==SpvOpImageSampleProjDrefImplicitLod);
    const uint32_t *im=image_type(c,image);
    int compare=op==SpvOpImageSampleDrefImplicitLod || op==SpvOpImageSampleDrefExplicitLod ||
                op==SpvOpImageSampleProjDrefImplicitLod || op==SpvOpImageSampleProjDrefExplicitLod ||
                op==SpvOpImageDrefGather;
    int gather=op==SpvOpImageGather || op==SpvOpImageDrefGather;
    int projected=op==SpvOpImageSampleProjImplicitLod || op==SpvOpImageSampleProjExplicitLod ||
                  op==SpvOpImageSampleProjDrefImplicitLod || op==SpvOpImageSampleProjDrefExplicitLod;
    unsigned coord=p[4],width=components(c,node(c,coord)->type);
    unsigned dim=im[3]==SpvDim1D?0:im[3]==SpvDim2D?1:im[3]==SpvDim3D?2:im[3]==SpvDimCube?3:99;
    if (dim==99 || im[6] || im[7]!=1 || width<1 || width>4)
        cfail(c,"sampled image operand shape");
    unsigned operand=compare||gather?6:5;
    if (compare||gather)node(c,p[5]);
    unsigned mask=cnw(p)>operand?p[operand++]:0;
    if (mask&~(SpvImageOperandsBiasMask|SpvImageOperandsLodMask|SpvImageOperandsGradMask|
               SpvImageOperandsConstOffsetMask|SpvImageOperandsOffsetMask|SpvImageOperandsConstOffsetsMask))
        cfail(c,"unsupported sampling image operands");
    uint32_t bias=0,lod=0,dx=0,dy=0,offset=0,offsets=0;
    for (unsigned bit=1;bit<=SpvImageOperandsConstOffsetsMask;bit<<=1)
        if (mask&bit) {
            if (operand>=cnw(p))cfail(c,"sampling operand missing");
            uint32_t value=p[operand++];node(c,value);
            if (bit==SpvImageOperandsBiasMask)bias=value;
            if (bit==SpvImageOperandsLodMask)lod=value;
            if (bit==SpvImageOperandsGradMask) {
                dx=value;
                if (operand>=cnw(p))cfail(c,"sampling gradient missing");
                dy=p[operand++];node(c,dy);
            }
            if (bit==SpvImageOperandsConstOffsetMask || bit==SpvImageOperandsOffsetMask)offset=value;
            if (bit==SpvImageOperandsConstOffsetsMask)offsets=value;
        }
    if (operand!=cnw(p))cfail(c,"sampling operand count");
    emit(c,"{uint s=h%u.x;float4 coord=as_type<float4>(v%u);float lod=0,shader_bias=0;int3 offset=int3(0);uint4 ox=uint4(0),oy=uint4(0);\n",image,coord);
    if (projected)emit(c,"coord.xyz/=coord[%u];\n",width-1);
    if (lod)emit(c,"lod=as_type<float>(v%u.x);\n",lod);
    if (dx || implicit) {
        unsigned n=dim==0?1:dim==1?2:3;
        if (dx && (components(c,node(c,dx)->type)!=n || components(c,node(c,dy)->type)!=n))
            cfail(c,"sampling gradient shape");
        if (dx)emit(c,"{float3 dx=as_type<float4>(v%u).xyz,dy=as_type<float4>(v%u).xyz;\n",dx,dy);
        else emit(c,"{float3 dx=dfdx(coord.xyz),dy=dfdy(coord.xyz);\n");
        sampling_derivatives(c,dim);
        emit(c,"lod=0.5f*log2(max(dot(dx,dx),dot(dy,dy)));}\n");
    }
    if (bias)emit(c,"shader_bias=as_type<float>(v%u.x);\n",bias);
    if (offset) {
        unsigned n=components(c,node(c,offset)->type);
        for (unsigned k=0;k<n && k<3;k++)emit(c,"offset[%u]=as_type<int>(v%u[%u]);\n",k,offset,k);
    }
    if (offsets) {
        if (value_registers(c,node(c,offsets)->type,0)!=4)cfail(c,"gather offsets shape");
        for (unsigned k=0;k<4;k++)
            emit(c,"ox[%u]=v%u[%u].x;oy[%u]=v%u[%u].y;\n",k,offsets,k,k,offsets,k);
    }
    int component=gather&&!compare?(int)literal(c,p[5]):0;
    if (component<0 || component>3)cfail(c,"gather component");
    emit(c,"v%u=zvk_sample(buffers[s],sizes[s],imageinfo,s,h%u.y,%uu,coord,lod,shader_bias,%s,",
         id,image,dim+(im[5]?16:0),compare?"true":"false");
    if (compare) {
        emit(c,"as_type<float>(v%u.x)",p[5]);
        if (projected)emit(c,"/as_type<float>(v%u[%u])",coord,width-1);
    } else emit(c,"0.0f");
    emit(c,",offset,%d,ox,oy);}\n",gather?component:-1);
}
/* Query the sampled level and unclamped lambda. Both values are relative to
 * the image view's base level; sampler bias precedes min/max-LOD clamps.
 * Vulkan Sampling: Cube Map Derivative Transformation, Image Level Selection.
 * https://docs.vulkan.org/spec/latest/chapters/textures.html */
static void query_lod(struct cc *c,const uint32_t *p)
{
    if(c->stage!=VK_SHADER_STAGE_FRAGMENT_BIT || cnw(p)!=5)
        cfail(c,"image LOD query stage/operands");
    unsigned id=p[2],image=p[3],coordinate=p[4];
    const uint32_t *im=image_type(c,image);
    unsigned dim=im[3]==SpvDim1D?0:im[3]==SpvDim2D?1:im[3]==SpvDim3D?2:im[3]==SpvDimCube?3:99;
    const uint32_t *result=node(c,node(c,id)->type)->p;
    if(dim==99 || im[6] || im[7]!=1 || components(c,node(c,id)->type)!=2 ||
        cop(node(c,result[2])->p)!=SpvOpTypeFloat ||
        components(c,node(c,coordinate)->type)<(dim==0?1:dim==1?2:3))
        cfail(c,"image LOD query shape");
    c->out->sampling=1;
    emit(c,"{uint s=h%u.x;float3 coord=as_type<float4>(v%u).xyz;float3 dx=dfdx(coord),dy=dfdy(coord);\n",image,coordinate);
    sampling_derivatives(c,dim);
    emit(c,"float lod=0.5f*log2(max(dot(dx,dx),dot(dy,dy)));"
        "const device zvk_sampler *samplers=reinterpret_cast<const device zvk_sampler *>(imageinfo+ZVK_META_SAMPLER);"
        "zvk_sampler state=samplers[h%u.y];lod+=state.lod.x;"
        "float level=clamp(clamp(lod,state.lod.y,state.lod.z),0.0f,float((imageinfo[s].format.y>>16u)-1));"
        "if((state.modes.x&(1u<<22u))==0u)level=floor(level+0.5f);"
        "v%u=as_type<uint4>(float4(level,lod,0,0));}\n",image,id);
}
