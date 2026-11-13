/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Simultaneous advertised graphics resource counts, large runtime SSBOs,
 * high binding numbers, and independent image/sampler cross-pairing. */
static void atlas_tests(void)
{
    const unsigned storage_bytes=24576*4;
    unsigned uniform_stride=properties.limits.minUniformBufferOffsetAlignment;
    struct buffer uniforms=buffer(12*uniform_stride,VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    struct buffer storage=buffer(8*storage_bytes,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    struct buffer pixels=buffer(8*8*16,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    for (unsigned i=0;i<12;i++)((uint32_t *)uniforms.map)[i*uniform_stride/4]=i+1;
    for (unsigned i=0;i<8;i++) {
        ((uint32_t *)storage.map)[i*24576]=i+1;
        ((uint32_t *)storage.map)[i*24576+24575]=i+1;
    }
    struct image sampled=image(8,8,VK_FORMAT_R32_UINT,VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    struct image readable=image(8,8,VK_FORMAT_R32_UINT,VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    struct image inputs[8];
    for (unsigned i=0;i<8;i++)inputs[i]=image(8,8,VK_FORMAT_R32_UINT,
        VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    struct image target=image(8,8,VK_FORMAT_R32G32B32A32_UINT,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    VkSampler samplers[16];
    for (unsigned i=0;i<16;i++) {
        VkSamplerCreateInfo si={.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .addressModeU=i%4,.addressModeV=(i/4)%4,.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
        VK(vkCreateSampler(device,&si,NULL,samplers+i));
    }
    VkDescriptorSetLayoutBinding bindings[]={
        {0,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,12,VK_SHADER_STAGE_FRAGMENT_BIT,NULL},
        {65537,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,8,VK_SHADER_STAGE_FRAGMENT_BIT,NULL},
        {2,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,16,VK_SHADER_STAGE_FRAGMENT_BIT,NULL},
        {3,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,4,VK_SHADER_STAGE_FRAGMENT_BIT,NULL},
        {4,VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,8,VK_SHADER_STAGE_FRAGMENT_BIT,NULL},
        {5,VK_DESCRIPTOR_TYPE_SAMPLER,16,VK_SHADER_STAGE_FRAGMENT_BIT,NULL},
        {UINT32_MAX,VK_DESCRIPTOR_TYPE_SAMPLER,0,VK_SHADER_STAGE_FRAGMENT_BIT,NULL}};
    VkDescriptorSetLayoutCreateInfo si={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount=7,.pBindings=bindings};
    VkDescriptorSetLayout set_layout;VK(vkCreateDescriptorSetLayout(device,&si,NULL,&set_layout));
    VkPipelineLayoutCreateInfo li={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount=1,.pSetLayouts=&set_layout};
    VkPipelineLayout layout;VK(vkCreatePipelineLayout(device,&li,NULL,&layout));
    VkDescriptorPoolSize sizes[6];
    for (unsigned i=0;i<6;i++)sizes[i]=(VkDescriptorPoolSize){bindings[i].descriptorType,bindings[i].descriptorCount};
    VkDescriptorPoolCreateInfo pi={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets=1,.poolSizeCount=6,.pPoolSizes=sizes};
    VkDescriptorPool descriptor_pool;VK(vkCreateDescriptorPool(device,&pi,NULL,&descriptor_pool));
    VkDescriptorSetAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool=descriptor_pool,.descriptorSetCount=1,.pSetLayouts=&set_layout};
    VkDescriptorSet set;VK(vkAllocateDescriptorSets(device,&ai,&set));
    VkDescriptorBufferInfo ub[12],sb[8];VkDescriptorImageInfo images[16],stores[4],attachments[8],sampler_info[16];
    for (unsigned i=0;i<12;i++)ub[i]=(VkDescriptorBufferInfo){uniforms.handle,i*uniform_stride,16};
    for (unsigned i=0;i<8;i++)sb[i]=(VkDescriptorBufferInfo){storage.handle,i*storage_bytes,storage_bytes};
    for (unsigned i=0;i<16;i++) {
        images[i]=(VkDescriptorImageInfo){0,sampled.view,VK_IMAGE_LAYOUT_GENERAL};
        sampler_info[i]=(VkDescriptorImageInfo){samplers[i],0,0};
    }
    for (unsigned i=0;i<4;i++)stores[i]=(VkDescriptorImageInfo){0,readable.view,VK_IMAGE_LAYOUT_GENERAL};
    for (unsigned i=0;i<8;i++)attachments[i]=(VkDescriptorImageInfo){0,inputs[i].view,VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet writes[6];
    for (unsigned i=0;i<6;i++)writes[i]=(VkWriteDescriptorSet){.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet=set,.dstBinding=bindings[i].binding,.descriptorType=bindings[i].descriptorType,
        .descriptorCount=bindings[i].descriptorCount,.pBufferInfo=i==0?ub:i==1?sb:NULL,
        .pImageInfo=i==2?images:i==3?stores:i==4?attachments:i==5?sampler_info:NULL};
    vkUpdateDescriptorSets(device,6,writes,0,NULL);
    VkAttachmentDescription descriptions[9];VkAttachmentReference refs[8];VkImageView views[9];
    for (unsigned i=0;i<9;i++) {
        descriptions[i]=(VkAttachmentDescription){.format=i==8?VK_FORMAT_R32G32B32A32_UINT:VK_FORMAT_R32_UINT,
            .samples=VK_SAMPLE_COUNT_1_BIT,.loadOp=i==8?VK_ATTACHMENT_LOAD_OP_DONT_CARE:VK_ATTACHMENT_LOAD_OP_LOAD,
            .storeOp=VK_ATTACHMENT_STORE_OP_STORE,.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE,.initialLayout=i==8?VK_IMAGE_LAYOUT_UNDEFINED:VK_IMAGE_LAYOUT_GENERAL,
            .finalLayout=i==8?VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:VK_IMAGE_LAYOUT_GENERAL};
        views[i]=i==8?target.view:inputs[i].view;
        if (i<8)refs[i]=(VkAttachmentReference){i,VK_IMAGE_LAYOUT_GENERAL};
    }
    VkAttachmentReference color={8,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass={.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS,
        .inputAttachmentCount=8,.pInputAttachments=refs,.colorAttachmentCount=1,.pColorAttachments=&color};
    VkSubpassDependency dependency={.srcSubpass=0,.dstSubpass=VK_SUBPASS_EXTERNAL,
        .srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,.dstStageMask=VK_PIPELINE_STAGE_TRANSFER_BIT,
        .srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT};
    VkRenderPassCreateInfo ri={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,.attachmentCount=9,
        .pAttachments=descriptions,.subpassCount=1,.pSubpasses=&subpass,.dependencyCount=1,.pDependencies=&dependency};
    VkRenderPass pass;VK(vkCreateRenderPass(device,&ri,NULL,&pass));
    VkFramebufferCreateInfo fi={.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,.renderPass=pass,
        .attachmentCount=9,.pAttachments=views,.width=8,.height=8,.layers=1};
    VkFramebuffer framebuffer;VK(vkCreateFramebuffer(device,&fi,NULL,&framebuffer));
    uint32_t *fragment=malloc(sizeof atlas_frag);
    if (!fragment)exit(2);
    memcpy(fragment,atlas_frag,sizeof atlas_frag);
    for (unsigned i=5;i<sizeof atlas_frag/4;i+=fragment[i]>>16)
        if ((fragment[i]&65535)==SpvOpDecorate && fragment[i]>>16==4 &&
            fragment[i+2]==SpvDecorationBinding && fragment[i+3]==1)
            fragment[i+3]=65537;
    VkPipelineVertexInputStateCreateInfo vertex={.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineCacheCreateInfo cache_info={.sType=VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    VkPipelineCache cache;VK(vkCreatePipelineCache(device,&cache_info,NULL,&cache));
    pipeline_cache=cache;
    pipeline_vertex=&vertex;
    VkPipeline pipeline=pipeline_shaders(atlas_vert,sizeof atlas_vert,NULL,pass,layout,fragment,sizeof atlas_frag,0,0,0);
    VkPipeline replacement=pipeline_shaders(atlas_vert,sizeof atlas_vert,NULL,pass,layout,fragment,sizeof atlas_frag,0,0,0);
    vkDestroyPipeline(device,pipeline,NULL);pipeline=replacement;
    pipeline_cache=VK_NULL_HANDLE;vkDestroyPipelineCache(device,cache,NULL);
    pipeline_vertex=NULL;
    free(fragment);
    begin();
    VkImageSubresourceRange range={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    for (unsigned i=0;i<10;i++) {
        VkImage image=i==0?sampled.handle:i==1?readable.handle:inputs[i-2].handle;
        barrier(image,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkClearColorValue clear={.uint32={i==0?3:i==1?7:11,0,0,0}};
        vkCmdClearColorImage(command,image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&clear,1,&range);
        barrier(image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL);
    }
    VkRenderPassBeginInfo bi={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,.renderPass=pass,
        .framebuffer=framebuffer,.renderArea={{0,0},{8,8}}};
    vkCmdBeginRenderPass(command,&bi,VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);
    vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,layout,0,1,&set,0,NULL);
    VkViewport viewport={0,0,8,8,0,1};VkRect2D scissor={{0,0},{8,8}};
    vkCmdSetViewport(command,0,1,&viewport);vkCmdSetScissor(command,0,1,&scissor);
    vkCmdDraw(command,3,1,0,0);vkCmdEndRenderPass(command);
    VkBufferImageCopy copy={.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageExtent={8,8,1}};
    vkCmdCopyImageToBuffer(command,target.handle,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,pixels.handle,1,&copy);
    submit();
    int buffers_good=1,images_good=1;
    for (unsigned i=0;i<64;i++) {
        uint32_t *p=(uint32_t *)pixels.map+4*i;
        buffers_good&=p[0]==78 && p[1]==836;images_good&=p[2]==96 && p[3]==116;
        if ((!buffers_good || !images_good) && i==0)printf("atlas got %u,%u,%u,%u expected 78,836,96,116\n",p[0],p[1],p[2],p[3]);
    }
    EXPECT(buffers_good,"twelve UBOs and eight large runtime SSBOs execute together at binding 65537");
    EXPECT(images_good,"sixteen images and samplers, four storage images and eight input attachments execute together");
    vkDestroyPipeline(device,pipeline,NULL);vkDestroyFramebuffer(device,framebuffer,NULL);
    vkDestroyRenderPass(device,pass,NULL);vkDestroyDescriptorPool(device,descriptor_pool,NULL);
    vkDestroyPipelineLayout(device,layout,NULL);vkDestroyDescriptorSetLayout(device,set_layout,NULL);
    for (unsigned i=0;i<16;i++)vkDestroySampler(device,samplers[i],NULL);
    for (unsigned i=0;i<8;i++)free_image(inputs[i]);
    free_image(sampled);free_image(readable);free_image(target);
    free_buffer(uniforms);free_buffer(storage);free_buffer(pixels);
}
