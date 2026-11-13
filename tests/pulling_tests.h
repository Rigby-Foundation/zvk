/* SPDX-License-Identifier: LGPL-2.1-or-later */
static void pulling_tests(void)
{
    struct buffer vertices=buffer(16*8*16,VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    for (unsigned attribute=0;attribute<16;attribute++)for (unsigned index=0;index<8;index++)
        for (unsigned lane=0;lane<4;lane++)
            ((float *)vertices.map)[attribute*32+index*4+lane]=attribute*100+index;
    struct buffer indices=buffer(6,VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    uint16_t order[3]={2,0,1};memcpy(indices.map,order,sizeof order);
    struct buffer pixels=buffer(8*8*4,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    struct image output=image(8,8,VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    VkAttachmentDescription attachment={.format=VK_FORMAT_R8G8B8A8_UNORM,.samples=VK_SAMPLE_COUNT_1_BIT,
        .loadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE,.storeOp=VK_ATTACHMENT_STORE_OP_STORE,
        .initialLayout=VK_IMAGE_LAYOUT_UNDEFINED,.finalLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
    VkAttachmentReference color={0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass={.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount=1,.pColorAttachments=&color};
    VkSubpassDependency dependency={.srcSubpass=0,.dstSubpass=VK_SUBPASS_EXTERNAL,
        .srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,.dstStageMask=VK_PIPELINE_STAGE_TRANSFER_BIT,
        .srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT};
    VkRenderPassCreateInfo ri={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,.attachmentCount=1,
        .pAttachments=&attachment,.subpassCount=1,.pSubpasses=&subpass,.dependencyCount=1,.pDependencies=&dependency};
    VkRenderPass pass;VK(vkCreateRenderPass(device,&ri,NULL,&pass));
    VkFramebufferCreateInfo fi={.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,.renderPass=pass,
        .attachmentCount=1,.pAttachments=&output.view,.width=8,.height=8,.layers=1};
    VkFramebuffer framebuffer;VK(vkCreateFramebuffer(device,&fi,NULL,&framebuffer));
    VkPipelineLayoutCreateInfo li={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    VkPipelineLayout layout;VK(vkCreatePipelineLayout(device,&li,NULL,&layout));
    VkVertexInputBindingDescription bindings[16];VkVertexInputAttributeDescription attributes[16];
    VkBuffer buffers[16];VkDeviceSize offsets[16];
    for (unsigned i=0;i<16;i++) {
        bindings[i]=(VkVertexInputBindingDescription){i,16,i&1?VK_VERTEX_INPUT_RATE_INSTANCE:VK_VERTEX_INPUT_RATE_VERTEX};
        attributes[i]=(VkVertexInputAttributeDescription){i,i,VK_FORMAT_R32G32B32A32_SFLOAT,0};
        buffers[i]=vertices.handle;offsets[i]=i*128;
    }
    VkPipelineVertexInputStateCreateInfo vertex={.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount=16,.pVertexBindingDescriptions=bindings,
        .vertexAttributeDescriptionCount=16,.pVertexAttributeDescriptions=attributes};
    pipeline_vertex=&vertex;
    VkPipeline pipeline=pipeline_shaders(pulling_vert,sizeof pulling_vert,NULL,pass,layout,triangle_frag,sizeof triangle_frag,0,0,0);
    pipeline_vertex=NULL;
    for (unsigned indexed=0;indexed<2;indexed++) {
        begin();
        VkRenderPassBeginInfo bi={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,.renderPass=pass,
            .framebuffer=framebuffer,.renderArea={{0,0},{8,8}}};
        vkCmdBeginRenderPass(command,&bi,VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);
        vkCmdBindVertexBuffers(command,0,16,buffers,offsets);
        VkViewport vp={0,0,8,8,0,1};VkRect2D sc={{0,0},{8,8}};
        vkCmdSetViewport(command,0,1,&vp);vkCmdSetScissor(command,0,1,&sc);
        if (indexed) {
            vkCmdBindIndexBuffer(command,indices.handle,0,VK_INDEX_TYPE_UINT16);
            vkCmdDrawIndexed(command,3,1,0,1,5);
        } else vkCmdDraw(command,3,1,1,5);
        vkCmdEndRenderPass(command);
        VkBufferImageCopy copy={.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageExtent={8,8,1}};
        vkCmdCopyImageToBuffer(command,output.handle,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,pixels.handle,1,&copy);
        submit();
        int good=1;
        for (unsigned i=0;i<64;i++) {
            uint8_t *p=(uint8_t *)pixels.map+4*i;
            good&=p[0]==0 && p[1]==255 && p[2]==0 && p[3]==255;
        }
        EXPECT(good,indexed?"GPU pulls sixteen attributes with indexed base vertex and first instance":
            "GPU pulls sixteen attributes while preserving vertex and instance builtins");
    }
    vkDestroyPipeline(device,pipeline,NULL);vkDestroyPipelineLayout(device,layout,NULL);
    vkDestroyFramebuffer(device,framebuffer,NULL);vkDestroyRenderPass(device,pass,NULL);
    free_image(output);free_buffer(vertices);free_buffer(indices);free_buffer(pixels);
}
