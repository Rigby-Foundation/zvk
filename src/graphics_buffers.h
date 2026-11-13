/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Descriptor byte snapshots consumed by native GPU loads. Uploads are bounded
 * command packets, with no guest mapping for the potentially large atlas. */
struct atlas_segment {
    VkDeviceMemory memory;
    uint64_t start,end,base;
};
static int atlas_order(const void *a,const void *b)
{
    const struct atlas_segment *x=a,*y=b;
    if (x->memory!=y->memory)return (uintptr_t)x->memory<(uintptr_t)y->memory?-1:1;
    return x->start<y->start?-1:x->start>y->start;
}
static void atlas_upload(virgl *gpu,virgl_res *atlas,uint64_t offset,const void *source,uint64_t bytes)
{
    uint8_t tail[16]={0};
    const uint8_t *data=source;
    while (bytes) {
        uint32_t x=(offset/16)%4096,y=(offset/65536)%4096,z=offset/(65536ull*4096);
        uint64_t count=bytes;
        if (count>16384)count=16384;
        if (count>(4096-x)*16)count=(4096-x)*16;
        const void *p=data;
        uint32_t padded=(count+15)&~15u;
        if (count<16) {memset(tail,0,16);memcpy(tail,data,count);p=tail;}
        else if (count&15) {count&=~15ull;padded=count;}
        virgl_inline_write(gpu,atlas->id,0,0,padded,padded,x,y,z,padded/16,1,1,p,padded);
        offset+=padded;data+=count;bytes-=count;
    }
}
struct graphics_metadata {
    struct compute_metadata resources;
    uint32_t vertices[ZVK_ATTRS][16];
    uint32_t first_instance[4];
};
_Static_assert(offsetof(struct graphics_metadata,vertices)==28064,"graphics atlas vertex ABI");
static VkResult graphics_buffers(struct execution *e,zvk_shader *shader,unsigned stage_index,uint32_t first_instance)
{
    struct atlas_segment segments[ZVK_RESOURCES+ZVK_ATTRS],ranges[ZVK_RESOURCES+ZVK_ATTRS]={0};
    unsigned count=0,merged=0;
    struct graphics_metadata packed={0};
    struct compute_metadata *metadata=&packed.resources;
    packed.first_instance[0]=first_instance;
    for (unsigned i=0;i<shader->nsamplers;i++) {
        const zvk_shader_binding *b=shader->samplers+i;
        if (!b->uses_sampling)continue;
        VkDescriptorSet set=e->sets[b->set];
        if (!set)return VK_ERROR_INITIALIZATION_FAILED;
        compute_sampler(metadata,b->slot,set->descriptors[b->index].sampler,NULL);
    }
    for (unsigned i=0;i<shader->nbindings;i++) {
        const zvk_shader_binding *b=shader->bindings+i;
        VkDescriptorSet set=e->sets[b->set];
        if (!set)return VK_ERROR_INITIALIZATION_FAILED;
        zvk_descriptor *desc=set->descriptors+b->index;
        VkDeviceMemory memory=NULL;uint64_t start=0,length=0;
        unsigned slot=b->slot;
        if (b->type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || b->type==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
            VkBuffer buffer=desc->buffer;
            uint64_t extra=(desc->type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC ||
                            desc->type==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC)?
                           e->dynamic[b->set][b->dynamic_index]:0;
            if (!buffer || !buffer->memory || extra>UINT64_MAX-desc->offset)return VK_ERROR_INITIALIZATION_FAILED;
            uint64_t offset=desc->offset+extra;
            length=desc->range==VK_WHOLE_SIZE?(offset<=buffer->size?buffer->size-offset:0):desc->range;
            if (!zvk_range(buffer->size,offset,length) || !slot || slot>ZVK_RESOURCES)
                return VK_ERROR_INITIALIZATION_FAILED;
            memory=buffer->memory;start=buffer->offset+offset;slot--;
        } else if (b->type==VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER || b->type==VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) {
            VkBufferView view=desc->texel;
            if (!view || !view->buffer->memory)return VK_ERROR_INITIALIZATION_FAILED;
            memory=view->buffer->memory;start=view->buffer->offset+view->offset;length=view->range;
            uint32_t *m=metadata->images[slot];m[0]=length/zvk_bpp(view->format);m[1]=m[2]=1;
            compute_format(m,zvk_get_format_info(view->format));m[5]|=1u<<16;
            memcpy(metadata->levels[slot][0],m,32);compute_sampler(metadata,slot,NULL,NULL);
        } else {
            VkImageView view=desc->image;
            if (!view || !view->image->memory)return VK_ERROR_INITIALIZATION_FAILED;
            VkSampler sampler=b->uses_sampling && !b->separate_sampler?desc->sampler:NULL;
            compute_sampler(metadata,slot,sampler,view);
            VkResult result;
            if (b->image_ms) {
                result=mutable_prepare(e,view);
                if (result==VK_SUCCESS)result=zvk_sample_snapshot(view,&memory);
                if (result!=VK_SUCCESS)return result;
                length=memory->size;
                uint32_t *m=metadata->images[slot];
                m[0]=zvk_mip(view->image->info.extent.width,view->range.baseMipLevel);
                m[1]=zvk_mip(view->image->info.extent.height,view->range.baseMipLevel);
                m[2]=view->range.layerCount*view->image->info.samples;
                VkFormat format=b->image_kind==0?VK_FORMAT_R32G32B32A32_UINT:
                    b->image_kind==1?VK_FORMAT_R32G32B32A32_SINT:VK_FORMAT_R32G32B32A32_SFLOAT;
                compute_format(m,zvk_get_format_info(format));m[5]|=1u<<16|view->image->info.samples<<24;
                memcpy(metadata->levels[slot][0],m,32);
            } else {
                result=compute_sampled_image(view,slot,metadata,&memory,&start,&length);
                if (result!=VK_SUCCESS)return result;
            }
        }
        if (slot>=ZVK_RESOURCES)return VK_ERROR_INITIALIZATION_FAILED;
        ranges[i]=(struct atlas_segment){memory,start,start+length,0};
        segments[count++]=ranges[i];
    }
    if (shader->vertex_pulling) {
        VkPipeline pipeline=e->pipeline;
        for (unsigned i=0;i<pipeline->nattrs;i++) {
            const VkVertexInputAttributeDescription *a=pipeline->attrs+i;
            if (!(shader->inputs&(1u<<a->location)))continue;
            const VkVertexInputBindingDescription *binding=NULL;
            for (unsigned j=0;j<pipeline->nbindings;j++)
                if (pipeline->bindings[j].binding==a->binding)binding=pipeline->bindings+j;
            VkBuffer buffer=e->vertices[a->binding];
            if (!buffer || !buffer->memory || !binding)return VK_ERROR_INITIALIZATION_FAILED;
            unsigned index=ZVK_RESOURCES+a->location;
            ranges[index]=(struct atlas_segment){buffer->memory,buffer->offset,buffer->offset+buffer->size,0};
            segments[count++]=ranges[index];
            uint32_t *v=packed.vertices[a->location];
            v[2]=binding->stride;v[3]=binding->inputRate;
            v[4]=v[5]=v[6]=1;
            uint64_t offset=e->vertex_offsets[a->binding]+a->offset;
            v[7]=offset>UINT32_MAX?UINT32_MAX:offset;
            uint32_t format[8]={0};compute_format(format,zvk_get_format_info(a->format));
            memcpy(v+8,format+4,16);
        }
    }
    qsort(segments,count,sizeof *segments,atlas_order);
    uint64_t bytes=sizeof packed;
    for (unsigned i=0;i<count;i++) {
        struct atlas_segment s=segments[i];
        if (merged && segments[merged-1].memory==s.memory && s.start<=segments[merged-1].end) {
            if (s.end>segments[merged-1].end)segments[merged-1].end=s.end;
        } else segments[merged++]=s;
    }
    for (unsigned i=0;i<merged;i++) {
        segments[i].base=bytes;
        bytes+=(segments[i].end-segments[i].start+15)&~15ull;
    }
    if (bytes/4>UINT32_MAX)return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    for (unsigned i=0;i<shader->nbindings;i++) {
        struct atlas_segment r=ranges[i];
        if (!r.memory)continue;
        const zvk_shader_binding *b=shader->bindings+i;
        unsigned slot=b->slot-(b->type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || b->type==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        for (unsigned j=0;j<merged;j++)
            if (segments[j].memory==r.memory && r.start>=segments[j].start && r.end<=segments[j].end) {
                metadata->ranges[slot][0]=(segments[j].base+r.start-segments[j].start)/4;
                metadata->ranges[slot][1]=r.end-r.start;
                break;
            }
    }
    if(getenv("ZVK_DEBUG_ATLAS"))for(unsigned i=0;i<shader->nbindings;i++) {
        const zvk_shader_binding *b=shader->bindings+i;
        if(b->type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || b->type==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER || b->slot>=ZVK_RESOURCES)continue;
        unsigned slot=b->slot;const uint32_t *m=metadata->images[slot];
        fprintf(stderr,"zvk atlas stage=%u binding=%u slot=%u sampling=%u extent=%u,%u,%u format=%08x range=%u,%u bias=%g lod=%g..%g modes=%08x\n",
            stage_index,b->binding,slot,b->uses_sampling,m[0],m[1],m[2],m[5],metadata->ranges[slot][0],metadata->ranges[slot][1],
            metadata->samplers[slot].lod[0],metadata->samplers[slot].lod[1],metadata->samplers[slot].lod[2],metadata->samplers[slot].modes[0]);
    }
    if (shader->vertex_pulling)for (unsigned i=0;i<ZVK_ATTRS;i++) {
        struct atlas_segment r=ranges[ZVK_RESOURCES+i];
        if (!r.memory)continue;
        for (unsigned j=0;j<merged;j++)
            if (segments[j].memory==r.memory && r.start>=segments[j].start && r.end<=segments[j].end) {
                packed.vertices[i][0]=(segments[j].base+r.start-segments[j].start)/4;
                packed.vertices[i][1]=r.end-r.start;break;
            }
    }
    virgl *gpu=e->queue->device->gpu;
    uint64_t rows=(bytes+65535)/65536;
    uint32_t height=rows>4096?4096:rows,layers=(rows+4095)/4096;
    if (e->atlas[stage_index] &&
        (e->atlas_height[stage_index]<height || e->atlas_layers[stage_index]<layers)) {
        virgl_destroy_object(gpu,e->atlas_view[stage_index],VIRGL_OBJECT_SAMPLER_VIEW);
        virgl_res_destroy(gpu,e->atlas[stage_index]);e->atlas[stage_index]=NULL;
    }
    if (!e->atlas[stage_index]) {
        struct gpu_res_create rc={.target=PIPE_TEXTURE_2D_ARRAY,.format=VIRGL_FORMAT_R32G32B32A32_UINT,
            .bind=VIRGL_BIND_SAMPLER_VIEW,.width=4096,.height=height,.depth=1,.array_size=layers};
        e->atlas[stage_index]=virgl_res_create_ex(gpu,&rc);
        if (!e->atlas[stage_index])return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        e->atlas_height[stage_index]=height;e->atlas_layers[stage_index]=layers;
        e->atlas_view[stage_index]=virgl_handle(gpu);
        virgl_create_sampler_view_ex(gpu,e->atlas_view[stage_index],e->atlas[stage_index]->id,
            PIPE_TEXTURE_2D_ARRAY,VIRGL_FORMAT_R32G32B32A32_UINT,0,layers-1,0,0,VIRGL_SWIZZLE_IDENTITY);
    }
    if (!e->atlas_sampler[stage_index]) {
        e->atlas_sampler[stage_index]=virgl_handle(gpu);
        virgl_create_sampler_state(gpu,e->atlas_sampler[stage_index],PIPE_TEX_WRAP_CLAMP_TO_EDGE,
            PIPE_TEX_WRAP_CLAMP_TO_EDGE,PIPE_TEX_FILTER_NEAREST,PIPE_TEX_MIPFILTER_NONE,PIPE_TEX_FILTER_NEAREST);
    }
    atlas_upload(gpu,e->atlas[stage_index],0,&packed,sizeof packed);
    for (unsigned i=0;i<merged;i++)
        atlas_upload(gpu,e->atlas[stage_index],segments[i].base,
                     segments[i].memory->data+segments[i].start,segments[i].end-segments[i].start);
    return VK_SUCCESS;
}
