/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Packed image and sampler metadata shared by the independent Metal compiler. */
struct compute_metadata {
    uint32_t images[ZVK_RESOURCES][8];
    uint32_t levels[ZVK_RESOURCES][ZVK_MIPS][8];
    struct {
        uint32_t modes[4];
        float lod[4];
        uint32_t border[4], swizzle[4];
    } samplers[ZVK_RESOURCES + ZVK_SAMPLERS];
    uint32_t ranges[ZVK_RESOURCES][2];
};
_Static_assert(offsetof(struct compute_metadata,ranges)==ZVK_META_RANGES*32,"Metal metadata ABI");
static void compute_format(uint32_t out[8],const zvk_format_info *f)
{
    out[4]=f->bpp;out[5]=f->type;out[6]=out[7]=0;
    for (unsigned k=0;k<4;k++) {
        out[6]|=(uint32_t)f->bits[k]<<(k*8);
        out[7]|=(uint32_t)f->shift[k]<<(k*8);
    }
}
static void compute_sampler(struct compute_metadata *meta,unsigned slot,VkSampler sampler,
                            VkImageView view)
{
    if (sampler) {
        const VkSamplerCreateInfo *s=&sampler->create;
        meta->samplers[slot].modes[0]=s->addressModeU|(s->addressModeV<<4)|(s->addressModeW<<8)|
            (s->unnormalizedCoordinates<<12)|(s->compareEnable<<13)|(s->compareOp<<16)|
            (s->minFilter<<20)|(s->magFilter<<21)|(s->mipmapMode<<22);
        meta->samplers[slot].lod[0]=s->mipLodBias;
        meta->samplers[slot].lod[1]=s->minLod;
        meta->samplers[slot].lod[2]=s->maxLod;
        for (unsigned k=0;k<4;k++) {
            unsigned one=s->borderColor==VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE ||
                         s->borderColor==VK_BORDER_COLOR_INT_OPAQUE_WHITE ||
                         (k==3 && (s->borderColor==VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK ||
                                   s->borderColor==VK_BORDER_COLOR_INT_OPAQUE_BLACK));
            meta->samplers[slot].border[k]=one?(s->borderColor&1?1u:0x3f800000u):0;
        }
    }
    for (unsigned k=0;k<4;k++) {
        VkComponentSwizzle sw=view?(&view->swizzle.r)[k]:VK_COMPONENT_SWIZZLE_IDENTITY;
        meta->samplers[slot].swizzle[k]=sw==VK_COMPONENT_SWIZZLE_IDENTITY?k:
            sw==VK_COMPONENT_SWIZZLE_ZERO?4:sw==VK_COMPONENT_SWIZZLE_ONE?5:sw-VK_COMPONENT_SWIZZLE_R;
    }
}
static VkResult compute_sampled_image(VkImageView view,unsigned slot,struct compute_metadata *meta,
                                      VkDeviceMemory *memory,uint64_t *offset,uint64_t *range)
{
    VkImage image=view->image;
    VkResult result=zvk_image_gpu(image);
    if (result!=VK_SUCCESS) return result;
    int compressed=zvk_block(view->format)==4;
    const zvk_format_info *f=zvk_get_format_info(view->format);
    unsigned present=0;
    for (unsigned k=0;k<4;k++)if (f->bits[k])present|=1u<<k;
    if (compressed) {
        present=view->format==VK_FORMAT_BC1_RGB_UNORM_BLOCK || view->format==VK_FORMAT_BC1_RGB_SRGB_BLOCK ||
                view->format==VK_FORMAT_BC6H_UFLOAT_BLOCK || view->format==VK_FORMAT_BC6H_SFLOAT_BLOCK?7:
                view->format==VK_FORMAT_BC4_UNORM_BLOCK || view->format==VK_FORMAT_BC4_SNORM_BLOCK?1:
                view->format==VK_FORMAT_BC5_UNORM_BLOCK || view->format==VK_FORMAT_BC5_SNORM_BLOCK?3:15;
    }
    zvk_format_info depth;
    if (view->range.aspectMask==VK_IMAGE_ASPECT_STENCIL_BIT) {
        present=1;
        depth=(zvk_format_info){.type=ZVK_FMT_UINT,.bpp=image->bpp,.bits={8},.shift={24}};
        f=&depth;
    } else if (view->range.aspectMask==VK_IMAGE_ASPECT_DEPTH_BIT) {
        present=1;
        depth=*f;depth.bits[1]=depth.bits[2]=depth.bits[3]=0;
        depth.type=view->format==VK_FORMAT_D32_SFLOAT || view->format==VK_FORMAT_D32_SFLOAT_S8_UINT?
            ZVK_FMT_FLOAT:ZVK_FMT_UNORM;
        f=&depth;
    }
    unsigned decoded_bpp=0;
    if (compressed) {
        int hdr=view->format>=VK_FORMAT_BC4_UNORM_BLOCK && view->format<=VK_FORMAT_BC6H_SFLOAT_BLOCK;
        VkFormat decoded=hdr?VK_FORMAT_R32G32B32A32_SFLOAT:
            (view->format==VK_FORMAT_BC1_RGB_SRGB_BLOCK || view->format==VK_FORMAT_BC1_RGBA_SRGB_BLOCK ||
             view->format==VK_FORMAT_BC2_SRGB_BLOCK || view->format==VK_FORMAT_BC3_SRGB_BLOCK ||
             view->format==VK_FORMAT_BC7_SRGB_BLOCK)?VK_FORMAT_R8G8B8A8_SRGB:VK_FORMAT_R8G8B8A8_UNORM;
        f=zvk_get_format_info(decoded);decoded_bpp=f->bpp;
    }
    uint64_t decoded_size=0;
    for (unsigned level=0;level<view->range.levelCount;level++) {
        unsigned mip=view->range.baseMipLevel+level;
        unsigned w=zvk_mip(image->info.extent.width,mip),h=zvk_mip(image->info.extent.height,mip);
        unsigned d=zvk_mip(image->info.extent.depth,mip);
        for (unsigned layer=view->range.baseArrayLayer;
             layer<view->range.baseArrayLayer+view->range.layerCount;layer++) {
            if (!zvk_layout_is(image,mip,layer,VK_IMAGE_LAYOUT_GENERAL) &&
                !zvk_layout_is(image,mip,layer,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) &&
                !zvk_layout_is(image,mip,layer,VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL))
                return VK_ERROR_INITIALIZATION_FAILED;
            result=image_read(image,mip,layer);
            if (result!=VK_SUCCESS) return result;
        }
        uint32_t *m=meta->levels[slot][level];
        m[0]=w;m[1]=h;m[2]=image->info.imageType==VK_IMAGE_TYPE_3D?d:view->range.layerCount;
        m[3]=compressed?decoded_size:zvk_image_offset(image,mip,view->range.baseArrayLayer);
        compute_format(m,f);
        m[5]|=view->range.levelCount<<16|present<<8;
        decoded_size+=(uint64_t)w*h*d*view->range.layerCount*decoded_bpp;
    }
    memcpy(meta->images[slot],meta->levels[slot][0],32);
    if (!compressed) {
        *memory=image->memory;*offset=image->offset;*range=image->size;
        memcpy(image->memory->data+image->offset,zvk_image_map(image),image->size);
        return VK_SUCCESS;
    }
    if (decoded_size>UINT32_MAX) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    if (!view->compute_image) {
        view->compute_image=virgl_res_create(image->device->gpu,PIPE_BUFFER,VIRGL_FORMAT_R8_UNORM,0,
                                              decoded_size,1,1,0,decoded_size);
        if (!view->compute_image) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    if (!view->compute_memory) {
        view->compute_memory=zvk_alloc(&view->alloc,sizeof *view->compute_memory);
        if (!view->compute_memory) return VK_ERROR_OUT_OF_HOST_MEMORY;
        view->compute_memory->device=view->device;view->compute_memory->data=view->compute_image->map;
        view->compute_memory->size=decoded_size;
    }
    for (unsigned level=0;level<view->range.levelCount;level++) {
        unsigned mip=view->range.baseMipLevel+level;
        uint32_t *m=meta->levels[slot][level];
        unsigned d=zvk_mip(image->info.extent.depth,mip);
        uint64_t plane=(uint64_t)m[0]*m[1]*decoded_bpp;
        uint64_t block_plane=(uint64_t)((m[0]+3)/4)*((m[1]+3)/4)*image->bpp;
        for (unsigned layer=0;layer<view->range.layerCount;layer++)
            for (unsigned z=0;z<d;z++)
                zvk_decode_bc(view->format,(uint8_t *)zvk_image_map(image)+
                    zvk_image_offset(image,mip,view->range.baseArrayLayer+layer)+z*block_plane,
                    view->compute_memory->data+m[3]+(layer*d+z)*plane,m[0],m[1]);
    }
    *memory=view->compute_memory;*offset=0;*range=decoded_size;
    return VK_SUCCESS;
}
