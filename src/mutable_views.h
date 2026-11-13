/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Each compatible view has typed GPU storage. Canonical texel bytes are
 * synchronized at rendering boundaries; multisample conversions preserve
 * individual samples on the GPU, without resolving or CPU shader execution. */
static VkImage view_image(VkImageView view)
{
    return view->reinterpret ? view->reinterpret : view->image;
}
static void mutable_format(uint32_t out[4], VkFormat format)
{
    const zvk_format_info *f=zvk_get_format_info(format);
    out[0]=f->bpp;out[1]=f->type;out[2]=out[3]=0;
    for (unsigned c=0;c<4;c++) {
        out[2]|=(uint32_t)f->bits[c]<<(8*c);
        out[3]|=(uint32_t)f->shift[c]<<(8*c);
    }
}
static VkResult mutable_ms_copy(struct execution *e, VkImage src, VkImage dst,
                                 const VkImageSubresourceRange *range)
{
    virgl *gpu=src->device->gpu;
    VkResult result=VK_SUCCESS;
    int paused=e->active_query && !e->active_query->queries[e->active_query_index].suspended;
    if (paused) {
        result=zvk_query_pause(e->active_query,e->active_query_index);
        if (result!=VK_SUCCESS)return result;
    }
    virgl_res *vertices=virgl_res_create(gpu,PIPE_BUFFER,VIRGL_FORMAT_R8_UNORM,
        VIRGL_BIND_VERTEX_BUFFER,64,1,1,0,64);
    if (!vertices) {result=VK_ERROR_OUT_OF_DEVICE_MEMORY;goto resume;}
    const float positions[4][4]={{-1,-1,0,1},{1,-1,0,1},{-1,1,0,1},{1,1,0,1}};
    memcpy(vertices->map,positions,sizeof positions);
    if (virgl_res_to_host(gpu,vertices,0,0,0,0,64,1,1,0,0,0)) {
        result=VK_ERROR_DEVICE_LOST;goto free_vertices;
    }
    unsigned vs=virgl_handle(gpu),fs=virgl_handle(gpu),ve=virgl_handle(gpu),
        blend=virgl_handle(gpu),rs=virgl_handle(gpu),ds=virgl_handle(gpu),
        sampler=virgl_handle(gpu);
    const char *target=src->target==PIPE_TEXTURE_2D_ARRAY?"2D_ARRAY_MSAA":"2D_MSAA";
    int kind=zvk_format_integer(src->info.format);
    char shader[1600];
    snprintf(shader,sizeof shader,
        "FRAG\nDCL IN[0], POSITION\nDCL OUT[0], COLOR[0]\nDCL CONST[0][0..2]\n"
        "DCL SAMP[0]\nDCL SVIEW[0], %s, %s\n"
        "DCL SAMP[15]\nDCL SVIEW[15], 2D_ARRAY, UINT\nDCL TEMP[0..15]\n"
        "0: F2U TEMP[1].xy, IN[0]\n1: MOV TEMP[1].z, CONST[0][2].yyyy\n"
        "2: MOV TEMP[1].w, CONST[0][2].xxxx\n3: TXF TEMP[4], TEMP[1], SAMP[0], %s\n"
        "4: MOV TEMP[8], CONST[0][0]\n5: MOV TEMP[12], CONST[0][1]\n"
        "6: CAL : 6031104\n7: MOV OUT[0], TEMP[0]\n8: END\n",
        target,kind==1?"UINT":kind==2?"SINT":"FLOAT",target);
    virgl_create_shader(gpu,vs,PIPE_SHADER_VERTEX,
        "VERT\nDCL IN[0]\nDCL OUT[0], POSITION\n0: MOV OUT[0], IN[0]\n1: END\n");
    virgl_create_shader(gpu,fs,PIPE_SHADER_FRAGMENT,shader);
    unsigned zero=0,vformat=VIRGL_FORMAT_R32G32B32A32_FLOAT;
    virgl_create_vertex_elements(gpu,ve,1,&zero,&zero,&vformat);
    uint32_t b[VIRGL_OBJ_BLEND_SIZE-1]={0},r[VIRGL_OBJ_RS_SIZE-1]={0},d[VIRGL_OBJ_DSA_SIZE-1]={0};
    b[2]=VIRGL_OBJ_BLEND_S2_RT_COLORMASK(15);
    r[0]=VIRGL_OBJ_RS_S0_HALF_PIXEL_CENTER(1)|VIRGL_OBJ_RS_S0_MULTISAMPLE(1);
    virgl_create_blend(gpu,blend,b);virgl_create_rasterizer(gpu,rs,r);virgl_create_dsa(gpu,ds,d);
    virgl_bind_shader(gpu,vs,PIPE_SHADER_VERTEX);virgl_bind_shader(gpu,fs,PIPE_SHADER_FRAGMENT);
    virgl_bind_shader(gpu,0,PIPE_SHADER_GEOMETRY);
    virgl_bind_object(gpu,ve,VIRGL_OBJECT_VERTEX_ELEMENTS);
    virgl_bind_object(gpu,blend,VIRGL_OBJECT_BLEND);virgl_bind_object(gpu,rs,VIRGL_OBJECT_RASTERIZER);
    virgl_bind_object(gpu,ds,VIRGL_OBJECT_DSA);
    unsigned stride=16,resource=vertices->id;
    virgl_set_vertex_buffers(gpu,1,&stride,&zero,&resource);
    virgl_set_min_samples(gpu,0);
    uint32_t constants[12]={0};
    mutable_format(constants,src->info.format);mutable_format(constants+4,dst->info.format);
    virgl_create_sampler_view_ex(gpu,sampler,src->gpu->id,src->target,src->format,
        0,src->info.arrayLayers-1,0,0,VIRGL_SWIZZLE_IDENTITY);
    virgl_set_sampler_views(gpu,PIPE_SHADER_FRAGMENT,0,&sampler,1);
    for (unsigned mip=range->baseMipLevel;mip<range->baseMipLevel+range->levelCount;mip++) {
        unsigned w=zvk_mip(src->info.extent.width,mip),h=zvk_mip(src->info.extent.height,mip);
        virgl_set_viewport(gpu,0,0,w,h,0,1,0);virgl_set_scissor(gpu,0,0,w,h);
        for (unsigned layer=range->baseArrayLayer;layer<range->baseArrayLayer+range->layerCount;layer++) {
            unsigned surface=virgl_handle(gpu);
            virgl_create_surface(gpu,surface,dst->gpu->id,dst->format,mip,layer|(layer<<16));
            virgl_set_framebuffer(gpu,0,&surface,1);
            constants[9]=layer;
            for (unsigned sample=0;sample<src->info.samples;sample++) {
                constants[8]=sample;
                virgl_set_constants(gpu,PIPE_SHADER_FRAGMENT,(const float *)constants,12);
                virgl_set_sample_mask(gpu,1u<<sample);
                virgl_draw(gpu,PIPE_PRIM_TRIANGLE_STRIP,0,4,0,0,3);
            }
            dst->transfer_valid[mip*dst->info.arrayLayers+layer]=0;
            virgl_destroy_object(gpu,surface,VIRGL_OBJECT_SURFACE);
        }
    }
    if (virgl_flush_fenced(gpu))result=VK_ERROR_DEVICE_LOST;
    virgl_destroy_object(gpu,sampler,VIRGL_OBJECT_SAMPLER_VIEW);
    virgl_destroy_object(gpu,vs,VIRGL_OBJECT_SHADER);virgl_destroy_object(gpu,fs,VIRGL_OBJECT_SHADER);
    virgl_destroy_object(gpu,ve,VIRGL_OBJECT_VERTEX_ELEMENTS);virgl_destroy_object(gpu,blend,VIRGL_OBJECT_BLEND);
    virgl_destroy_object(gpu,rs,VIRGL_OBJECT_RASTERIZER);virgl_destroy_object(gpu,ds,VIRGL_OBJECT_DSA);
free_vertices:
    virgl_res_destroy(gpu,vertices);
resume:
    if (paused)zvk_query_resume(e->active_query,e->active_query_index);
    return result;
}
static VkResult mutable_copy(struct execution *e, VkImage src, VkImage dst,
                              const VkImageSubresourceRange *range)
{
    if (src->info.samples!=VK_SAMPLE_COUNT_1_BIT)
        return mutable_ms_copy(e,src,dst,range);
    for (unsigned mip=range->baseMipLevel;mip<range->baseMipLevel+range->levelCount;mip++)
        for (unsigned layer=range->baseArrayLayer;layer<range->baseArrayLayer+range->layerCount;layer++) {
            VkResult r=image_read(src,mip,layer);
            if (r!=VK_SUCCESS)return r;
            memcpy((uint8_t *)zvk_image_map(dst)+zvk_image_offset(dst,mip,layer),
                   (uint8_t *)zvk_image_map(src)+zvk_image_offset(src,mip,layer),zvk_level_size(src,mip));
            r=image_write(dst,mip,layer);
            if (r!=VK_SUCCESS)return r;
        }
    return VK_SUCCESS;
}
static VkResult mutable_prepare(struct execution *e, VkImageView view)
{
    VkResult r=zvk_image_gpu(view->image);
    if (r!=VK_SUCCESS || view->format==view->image->info.format)return r;
    if (!view->reinterpret) {
        VkImageCreateInfo ci=view->image->info;
        ci.format=view->format;ci.flags&=~VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
        r=vkCreateImage(view->device,&ci,view->alloc.custom?&view->alloc.cb:NULL,&view->reinterpret);
        if (r!=VK_SUCCESS)return r;
        view->reinterpret->memory=view->image->memory;
        view->reinterpret->offset=view->image->offset;
    }
    r=zvk_image_gpu(view->reinterpret);
    return r==VK_SUCCESS?mutable_copy(e,view->image,view->reinterpret,&view->range):r;
}
static VkResult mutable_commit(struct execution *e, VkImageView view)
{
    return view->reinterpret?mutable_copy(e,view->reinterpret,view->image,&view->range):VK_SUCCESS;
}
