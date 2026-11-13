/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Typed view rendering, attachment load/partial clears and resolves must all
 * preserve the canonical bytes of a differently formatted base image. */
static void mutable_tests(void)
{
    VkPushConstantRange push={VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,20};
    VkPipelineLayoutCreateInfo li={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pushConstantRangeCount=1,.pPushConstantRanges=&push};
    VkPipelineLayout layout;VK(vkCreatePipelineLayout(device,&li,NULL,&layout));
    VkQueryPoolCreateInfo qi={.sType=VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
        .queryType=VK_QUERY_TYPE_OCCLUSION,.queryCount=1};
    VkQueryPool queries;VK(vkCreateQueryPool(device,&qi,NULL,&queries));
    for (unsigned samples=1;samples<=4;samples*=2) {
        VkImageFormatProperties props;
        VK(vkGetPhysicalDeviceImageFormatProperties(physical,VK_FORMAT_R32_SFLOAT,VK_IMAGE_TYPE_2D,
            VK_IMAGE_TILING_OPTIMAL,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT,VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT,&props));
        EXPECT(props.sampleCounts&samples,"mutable image queries preserve supported attachment sample counts");
        image_create_flags=VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
        struct image src=image_config(32,32,VK_FORMAT_R32_SFLOAT,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            1,1,0,samples);
        struct image dst=image(32,32,VK_FORMAT_R32_SFLOAT,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        image_create_flags=0;
        VkImageViewCreateInfo vi={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .viewType=VK_IMAGE_VIEW_TYPE_2D,.format=VK_FORMAT_R8G8B8A8_UNORM,
            .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1},.image=src.handle};
        VkImageView views[2];VK(vkCreateImageView(device,&vi,NULL,views));
        vi.image=dst.handle;VK(vkCreateImageView(device,&vi,NULL,views+1));
        VkAttachmentDescription attachments[2]={
            {.format=VK_FORMAT_R8G8B8A8_UNORM,.samples=samples,
             .loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR,.storeOp=VK_ATTACHMENT_STORE_OP_STORE,
             .initialLayout=VK_IMAGE_LAYOUT_UNDEFINED,.finalLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL},
            {.format=VK_FORMAT_R8G8B8A8_UNORM,.samples=VK_SAMPLE_COUNT_1_BIT,
             .loadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE,.storeOp=VK_ATTACHMENT_STORE_OP_STORE,
             .initialLayout=VK_IMAGE_LAYOUT_UNDEFINED,.finalLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL}};
        VkAttachmentReference color={0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
            resolve={1,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub={.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS,
            .colorAttachmentCount=1,.pColorAttachments=&color,.pResolveAttachments=samples>1?&resolve:NULL};
        unsigned count=samples>1?2:1;
        VkRenderPassCreateInfo pi={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
            .attachmentCount=count,.pAttachments=attachments,.subpassCount=1,.pSubpasses=&sub};
        VkRenderPass pass;VK(vkCreateRenderPass(device,&pi,NULL,&pass));
        VkFramebufferCreateInfo fi={.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,.renderPass=pass,
            .attachmentCount=count,.pAttachments=views,.width=32,.height=32,.layers=1};
        VkFramebuffer fb;VK(vkCreateFramebuffer(device,&fi,NULL,&fb));
        pipeline_samples=samples;
        VkPipeline render=pipeline_shaders(ms_write_vert,sizeof ms_write_vert,NULL,pass,layout,
            push_frag,sizeof push_frag,0,0,0);
        pipeline_samples=VK_SAMPLE_COUNT_1_BIT;
        struct buffer result=buffer(16+40*32*4,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        for (unsigned draw=0;draw<2;draw++) {
            begin();vkCmdResetQueryPool(command,queries,0,1);
            VkClearValue clear={.color.float32={.25f,.5f,.75f,.25f}};
            VkRenderPassBeginInfo bi={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,.renderPass=pass,
                .framebuffer=fb,.renderArea={{0,0},{32,32}},.clearValueCount=1,.pClearValues=&clear};
            vkCmdBeginRenderPass(command,&bi,VK_SUBPASS_CONTENTS_INLINE);
            VkViewport vp={0,0,32,32,0,1};VkRect2D sc={{0,0},{32,32}};
            vkCmdSetViewport(command,0,1,&vp);vkCmdSetScissor(command,0,1,&sc);
            vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,render);
            float values[]={.75f,.25f,.5f,.25f,.5f};
            vkCmdPushConstants(command,layout,push.stageFlags,0,sizeof values,values);
            vkCmdBeginQuery(command,queries,0,0);
            if (draw)vkCmdDraw(command,3,1,0,0);
            vkCmdEndQuery(command,queries,0);
            VkClearAttachment ca={.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT,.colorAttachment=0,
                .clearValue.color.float32={1,0,0,.25f}};
            VkClearRect cr={.rect={{8,8},{8,8}},.baseArrayLayer=0,.layerCount=1};
            vkCmdClearAttachments(command,1,&ca,1,&cr);
            vkCmdEndRenderPass(command);
            readback(samples>1?dst.handle:src.handle,result);submit();
            const uint8_t *outside=(const uint8_t *)result.map+16;
            const uint8_t *inside=(const uint8_t *)result.map+16+(10*40+10)*4;
            const uint8_t expected[2][4]={{64,128,191,64},{191,64,128,64}};
            EXPECT(!memcmp(outside,expected[draw],4)&&inside[0]==255&&inside[1]==0&&inside[2]==0&&inside[3]==64,
                "mutable view load/draw/partial clear/resolve preserve the original texel bits");
            uint64_t query=UINT64_MAX;
            VK(vkGetQueryPoolResults(device,queries,0,1,sizeof query,&query,sizeof query,
                VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WAIT_BIT));
            EXPECT(query==(draw?1024*samples:0),"private mutable-view transfers stay outside user occlusion queries");
        }
        free_buffer(result);vkDestroyPipeline(device,render,NULL);vkDestroyFramebuffer(device,fb,NULL);
        vkDestroyRenderPass(device,pass,NULL);vkDestroyImageView(device,views[0],NULL);
        vkDestroyImageView(device,views[1],NULL);free_image(src);free_image(dst);
    }
    vkDestroyQueryPool(device,queries,NULL);vkDestroyPipelineLayout(device,layout,NULL);
}
