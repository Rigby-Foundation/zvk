/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Fixed-point bias must be independent of the primitive's depth exponent.
 * Check static/dynamic state, slope, FragCoord and depth-replacing modes. */
static void fixed_depth_bias_tests(void)
{
    struct image color =
        image(32, 32, VK_FORMAT_R8G8B8A8_UNORM,
              VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    struct buffer pixels = buffer(16 + 40 * 32 * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    struct buffer depths = buffer(32 * 32 * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    VkPushConstantRange range = {VK_SHADER_STAGE_VERTEX_BIT, 0, 8};
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                     .pushConstantRangeCount = 1,
                                     .pPushConstantRanges = &range};
    VkPipelineLayout layout;
    VK(vkCreatePipelineLayout(device, &li, NULL, &layout));
    VkPipelineCacheCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    VkPipelineCache cache;
    VK(vkCreatePipelineCache(device, &ci, NULL, &cache));
    pipeline_cache = cache;
    VkPipelineVertexInputStateCreateInfo vertex = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    pipeline_vertex = &vertex;
    for (unsigned f = 0; f < 2; f++) {
        VkFormat format = f ? VK_FORMAT_D24_UNORM_S8_UINT : VK_FORMAT_D16_UNORM;
        struct image depth =
            image(32, 32, format,
                  VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        VkAttachmentDescription attachments[2] = {
            {.format = VK_FORMAT_R8G8B8A8_UNORM,
             .samples = VK_SAMPLE_COUNT_1_BIT,
             .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
             .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
             .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL},
            {.format = format,
             .samples = VK_SAMPLE_COUNT_1_BIT,
             .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
             .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
             .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
             .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
             .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL}};
        VkAttachmentReference cr = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
                              dr = {1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    .colorAttachmentCount = 1,
                                    .pColorAttachments = &cr,
                                    .pDepthStencilAttachment = &dr};
        VkRenderPassCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                     .attachmentCount = 2,
                                     .pAttachments = attachments,
                                     .subpassCount = 1,
                                     .pSubpasses = &sub};
        VkRenderPass pass;
        VK(vkCreateRenderPass(device, &ri, NULL, &pass));
        VkImageView views[2] = {color.view, depth.view};
        VkFramebufferCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                      .renderPass = pass,
                                      .attachmentCount = 2,
                                      .pAttachments = views,
                                      .width = 32,
                                      .height = 32,
                                      .layers = 1};
        VkFramebuffer fb;
        VK(vkCreateFramebuffer(device, &fi, NULL, &fb));
        for (unsigned test = 0; test < 6; test++) {
            float factor = f ? 4194304 : 16384;
            VkPipelineRasterizationStateCreateInfo raster = {
                .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                .polygonMode = VK_POLYGON_MODE_FILL,
                .lineWidth = 1,
                .depthBiasEnable = test != 0,
                .depthBiasConstantFactor = factor,
                .depthBiasSlopeFactor = test == 3 ? 2 : 0};
            pipeline_raster = &raster;
            pipeline_dynamic_raster = test == 2;
            const uint32_t *frag = test == 4   ? output_initializer_frag_spvasm
                                   : test == 5 ? early_keep_depth_frag
                                               : depth_bias_frag;
            size_t size = test == 4   ? sizeof output_initializer_frag_spvasm
                          : test == 5 ? sizeof early_keep_depth_frag
                                      : sizeof depth_bias_frag;
            VkPipeline p = pipeline_shaders(depth_bias_vert, sizeof depth_bias_vert, NULL, pass,
                                            layout, frag, size, 0, 1, 0);
            begin();
            begin_render(pass, fb, p, (struct buffer){0}, 2);
            float params[2] = {test == 2 ? .6f : .3f, test == 3 ? .2f : 0};
            vkCmdPushConstants(command, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof params,
                               params);
            if (test == 2) {
                vkCmdSetLineWidth(command, 1);
                vkCmdSetDepthBias(command, -factor, 0, 0);
            }
            vkCmdDraw(command, 3, 1, 0, 0);
            vkCmdEndRenderPass(command);
            readback(color.handle, pixels);
            barrier_aspect(depth.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           VK_IMAGE_ASPECT_DEPTH_BIT | (f ? VK_IMAGE_ASPECT_STENCIL_BIT : 0));
            VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1},
                                      .imageExtent = {32, 32, 1}};
            vkCmdCopyImageToBuffer(command, depth.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   depths.handle, 1, &copy);
            submit();
            unsigned index = 16 * 32 + 16;
            float got = f ? (((uint32_t *)depths.map)[index] & 0xffffffu) / 16777215.0f
                          : ((uint16_t *)depths.map)[index] / 65535.0f;
            float expected = test == 0   ? .3f
                             : test == 2 ? .35f
                             : test == 3 ? .58125f
                             : test == 4 ? .25f
                                         : .55f;
            int ok = isfinite(got) && fabsf(got - expected) < .00005f;
            if (test < 4) {
                int shade = (int)lroundf(expected * 255);
                ok &= pixel(pixels, 16, 16, shade, shade, shade, 1);
            }
            if (!ok)
                printf("fixed bias format=%u test=%u depth=%g expected=%g\n", format, test, got,
                       expected);
            EXPECT(ok, "fixed-point depth bias preserves static/dynamic slope, FragCoord and depth "
                       "replacement across depth exponents");
            vkDestroyPipeline(device, p, NULL);
        }
        vkDestroyFramebuffer(device, fb, NULL);
        vkDestroyRenderPass(device, pass, NULL);
        free_image(depth);
    }
    pipeline_raster = NULL;
    pipeline_dynamic_raster = 0;
    pipeline_vertex = NULL;
    pipeline_cache = VK_NULL_HANDLE;
    vkDestroyPipelineCache(device, cache, NULL);
    vkDestroyPipelineLayout(device, layout, NULL);
    free_buffer(depths);
    free_buffer(pixels);
    free_image(color);
}
