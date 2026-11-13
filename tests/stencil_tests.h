/* SPDX-License-Identifier: LGPL-2.1-or-later */
static void stencil_tests(void)
{
    VkImageFormatProperties properties;
    VK(vkGetPhysicalDeviceImageFormatProperties(
        physical, VK_FORMAT_D24_UNORM_S8_UINT, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
            VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT,
        0, &properties));
    for (unsigned input = 0; input < 2; input++)
        for (unsigned samples = 1; samples <= 4; samples *= 2) {
            if (!(properties.sampleCounts & samples))
                continue;
            unsigned mip = samples == 1 ? 1 : 0;
            unsigned outw = input ? 16 : 16 * samples;
            VkDescriptorType image_type = input ? VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT
                                                : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            struct image src =
                image_config(16u << mip, 16u << mip, VK_FORMAT_D24_UNORM_S8_UINT,
                             VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                 VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT,
                             mip + 1, 4, 0, samples);
            vkDestroyImageView(device, src.view, NULL);
            VkImageView views[2];
            VkImageViewCreateInfo vi = {
                .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .image = src.handle,
                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                .format = VK_FORMAT_D24_UNORM_S8_UINT,
                .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, mip,
                                     1, 2, 1}};
            VK(vkCreateImageView(device, &vi, NULL, &src.view));
            vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
            VK(vkCreateImageView(device, &vi, NULL, &views[0]));
            vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
            VK(vkCreateImageView(device, &vi, NULL, &views[1]));
            struct image out =
                image(outw, 16, VK_FORMAT_R8G8B8A8_UNORM,
                      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
            VkAttachmentDescription attachments[2] = {
                {.format = VK_FORMAT_D24_UNORM_S8_UINT,
                 .samples = samples,
                 .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                 .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                 .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                 .stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE,
                 .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                 .finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                {.format = VK_FORMAT_R8G8B8A8_UNORM,
                 .samples = VK_SAMPLE_COUNT_1_BIT,
                 .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                 .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                 .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                 .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                 .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                 .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL}};
            VkAttachmentReference ds = {0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
            VkAttachmentReference color = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
            VkSubpassDescription sub = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        .pDepthStencilAttachment = &ds};
            VkSubpassDependency deps[2] = {
                {.srcSubpass = 0,
                 .dstSubpass = VK_SUBPASS_EXTERNAL,
                 .srcStageMask = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                 .dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                 VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
                 .srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                 .dstAccessMask =
                     VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT},
                {.srcSubpass = 0,
                 .dstSubpass = VK_SUBPASS_EXTERNAL,
                 .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                 .dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT,
                 .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                 .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT}};
            VkRenderPassCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                         .attachmentCount = 1,
                                         .pAttachments = &attachments[0],
                                         .subpassCount = 1,
                                         .pSubpasses = &sub,
                                         .dependencyCount = 1,
                                         .pDependencies = &deps[0]};
            VkRenderPass write_pass, read_pass;
            VK(vkCreateRenderPass(device, &pi, NULL, &write_pass));
            VkAttachmentDescription read_attachments[2] = {attachments[0], attachments[1]};
            read_attachments[0].loadOp = read_attachments[0].stencilLoadOp =
                VK_ATTACHMENT_LOAD_OP_LOAD;
            read_attachments[0].initialLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            VkAttachmentReference inputs[2] = {{0, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                               {0, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
            pi.pAttachments = input ? read_attachments : &attachments[1];
            pi.attachmentCount = input ? 2 : 1;
            pi.pDependencies = &deps[1];
            color.attachment = input ? 1 : 0;
            sub.inputAttachmentCount = input ? 2 : 0;
            sub.pInputAttachments = inputs;
            sub.pDepthStencilAttachment = NULL;
            sub.colorAttachmentCount = 1;
            sub.pColorAttachments = &color;
            VK(vkCreateRenderPass(device, &pi, NULL, &read_pass));
            VkFramebuffer fb[2];
            VkFramebufferCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                          .renderPass = write_pass,
                                          .attachmentCount = 1,
                                          .pAttachments = &src.view,
                                          .width = 16,
                                          .height = 16,
                                          .layers = 1};
            VK(vkCreateFramebuffer(device, &fi, NULL, &fb[0]));
            VkImageView read_views[2] = {src.view, out.view};
            fi.renderPass = read_pass;
            fi.pAttachments = input ? read_views : &out.view;
            fi.attachmentCount = input ? 2 : 1;
            fi.width = outw;
            VK(vkCreateFramebuffer(device, &fi, NULL, &fb[1]));
            VkDescriptorSetLayoutBinding bindings[3] = {
                {0, image_type, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
                {1, image_type, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
                {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1,
                 VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, NULL}};
            VkDescriptorSetLayoutCreateInfo si = {
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                .bindingCount = 3,
                .pBindings = bindings};
            VkDescriptorSetLayout sl;
            VK(vkCreateDescriptorSetLayout(device, &si, NULL, &sl));
            VkPushConstantRange push = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                        0, 32};
            VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                             .setLayoutCount = 1,
                                             .pSetLayouts = &sl,
                                             .pushConstantRangeCount = 1,
                                             .pPushConstantRanges = &push};
            VkPipelineLayout layout;
            VK(vkCreatePipelineLayout(device, &li, NULL, &layout));
            VkDescriptorPoolSize size[2] = {{image_type, 2},
                                            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1}};
            VkDescriptorPoolCreateInfo dpi = {.sType =
                                                  VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                              .maxSets = 1,
                                              .poolSizeCount = 2,
                                              .pPoolSizes = size};
            VkDescriptorPool dp;
            VK(vkCreateDescriptorPool(device, &dpi, NULL, &dp));
            VkDescriptorSetAllocateInfo ai = {.sType =
                                                  VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                              .descriptorPool = dp,
                                              .descriptorSetCount = 1,
                                              .pSetLayouts = &sl};
            VkDescriptorSet set;
            VK(vkAllocateDescriptorSets(device, &ai, &set));
            VkSamplerCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                       .magFilter = VK_FILTER_NEAREST,
                                       .minFilter = VK_FILTER_NEAREST,
                                       .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST};
            VkSampler sampler;
            VK(vkCreateSampler(device, &sci, NULL, &sampler));
            struct buffer base = buffer(1088, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
            memset(base.map, 0xa5, 1088);
            for (unsigned i = 0; i < 256; i++)
                ((uint32_t *)base.map)[8 + i] = i;
            VkDescriptorBufferInfo storage = {base.handle, 16, 1024};
            uint32_t dynamic_offset = 16;
            VkWriteDescriptorSet writes[3];
            VkDescriptorImageInfo images[2];
            for (unsigned i = 0; i < 2; i++) {
                images[i] = (VkDescriptorImageInfo){input ? VK_NULL_HANDLE : sampler, views[i],
                                                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                writes[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                                   .dstSet = set,
                                                   .dstBinding = i,
                                                   .descriptorType = image_type,
                                                   .descriptorCount = 1,
                                                   .pImageInfo = &images[i]};
            }
            writes[2] =
                (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                       .dstSet = set,
                                       .dstBinding = 2,
                                       .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC,
                                       .descriptorCount = 1,
                                       .pBufferInfo = &storage};
            vkUpdateDescriptorSets(device, 3, writes, 0, NULL);
            pipeline_samples = samples;
            pipeline_color_count = 0;
            VkPipeline writers[4];
            for (unsigned sid = 0; sid < samples; sid++) {
                VkSampleMask mask = 1u << sid;
                pipeline_sample_mask = &mask;
                writers[sid] = pipeline_shaders(stencil_write_vert, sizeof stencil_write_vert, NULL,
                                                write_pass, layout, stencil_write_frag,
                                                sizeof stencil_write_frag, 0, 2, 0);
            }
            pipeline_sample_mask = NULL;
            pipeline_samples = VK_SAMPLE_COUNT_1_BIT;
            pipeline_color_count = -1;
            VkPipeline reader = pipeline_shaders(
                ms_write_vert, sizeof ms_write_vert, NULL, read_pass, layout,
                (input ? (samples == 1 ? stencil_input_frag : stencil_input_ms_frag)
                       : (samples == 1 ? stencil_fetch_frag : stencil_fetch_ms_frag)),
                (input ? (samples == 1 ? sizeof stencil_input_frag : sizeof stencil_input_ms_frag)
                       : (samples == 1 ? sizeof stencil_fetch_frag : sizeof stencil_fetch_ms_frag)),
                0, 0, 0);
            VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                        .queryType = VK_QUERY_TYPE_OCCLUSION,
                                        .queryCount = 1};
            VkQueryPool query;
            VK(vkCreateQueryPool(device, &qi, NULL, &query));
            struct buffer pixels = buffer(outw * 16 * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
            begin();
            vkCmdResetQueryPool(command, query, 0, 1);
            VkClearValue clears[2] = {{.depthStencil = {.375f, 0}}, {.color = {{0, 0, 0, 0}}}};
            VkRenderPassBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                        .renderPass = write_pass,
                                        .framebuffer = fb[0],
                                        .renderArea = {{0, 0}, {16, 16}},
                                        .clearValueCount = 1,
                                        .pClearValues = clears};
            vkCmdBeginRenderPass(command, &bi, VK_SUBPASS_CONTENTS_INLINE);
            vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 1,
                                    &dynamic_offset);
            VkViewport vp = {0, 0, 16, 16, 0, 1};
            VkRect2D sc = {{0, 0}, {16, 16}};
            vkCmdSetViewport(command, 0, 1, &vp);
            vkCmdSetScissor(command, 0, 1, &sc);
            vkCmdSetStencilCompareMask(command, VK_STENCIL_FACE_FRONT_AND_BACK, 255);
            for (unsigned sid = 0; sid < samples; sid++) {
                vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, writers[sid]);
                for (unsigned bit = 0; bit < 8; bit++) {
                    vkCmdSetStencilWriteMask(command, VK_STENCIL_FACE_FRONT_AND_BACK, 1u << bit);
                    vkCmdSetStencilReference(command, VK_STENCIL_FACE_FRONT_AND_BACK, 1u << bit);
                    uint32_t pc[8] = {1u << bit, sid};
                    float z = .5f;
                    memcpy(pc + 4, &z, 4);
                    vkCmdPushConstants(command, layout,
                                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                       32, pc);
                    vkCmdDraw(command, 3, 1, 0, 0);
                }
            }
            vkCmdEndRenderPass(command);
            clears[0] = (VkClearValue){.color = {{0, 0, 0, 0}}};
            bi.clearValueCount = input ? 2 : 1;
            bi.renderPass = read_pass;
            bi.framebuffer = fb[1];
            bi.renderArea.extent.width = outw;
            vkCmdBeginRenderPass(command, &bi, VK_SUBPASS_CONTENTS_INLINE);
            vp.width = outw;
            sc.extent.width = outw;
            vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, reader);
            vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 1,
                                    &dynamic_offset);
            vkCmdSetViewport(command, 0, 1, &vp);
            vkCmdSetScissor(command, 0, 1, &sc);
            uint32_t read_pc[8] = {samples};
            float read_z = .5f;
            memcpy(read_pc + 4, &read_z, 4);
            vkCmdPushConstants(command, layout,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32,
                               read_pc);
            vkCmdBeginQuery(command, query, 0, 0);
            vkCmdDraw(command, 3, 1, 0, 0);
            vkCmdDraw(command, 3, 1, 0, 0);
            vkCmdEndQuery(command, query, 0);
            vkCmdEndRenderPass(command);
            VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                      .imageExtent = {outw, 16, 1}};
            vkCmdCopyImageToBuffer(command, out.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   pixels.handle, 1, &copy);
            submit();
            int good = 1;
            for (unsigned y = 0; y < 16; y++)
                for (unsigned x = 0; x < outw; x++) {
                    uint8_t *p = (uint8_t *)pixels.map + 4 * (y * outw + x);
                    unsigned value = (y * 16 + x % 16 + 37 * (x / 16)) & 255;
                    if (input) {
                        value = 0;
                        for (unsigned sid = 0; sid < samples; sid++)
                            value += (y * 16 + x + 37 * sid) & 255;
                        value &= 255;
                    }
                    if (p[0] != value || abs((int)p[1] - 96) > 1 || p[2] != 255 || p[3] != 255) {
                        if (good)
                            printf("stencil samples=%u (%u,%u) got %u,%u,%u,%u expected "
                                   "%u,96,255,255\n",
                                   samples, x, y, p[0], p[1], p[2], p[3], value);
                        good = 0;
                    }
                }
            EXPECT(good, "GPU stencil reads preserve all 256 values, sample IDs, mip/layer "
                         "views and depth");
            uint64_t count = 0;
            VK(vkGetQueryPoolResults(device, query, 0, 1, sizeof count, &count, 8,
                                     VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
            EXPECT(count == 2 * outw * 16, "private stencil extraction draws do not contribute to "
                                           "application occlusion queries");
            int guards = 1;
            for (unsigned i = 0; i < 272; i++) {
                uint32_t expected = i >= 8 && i < 264 ? i - 8 : 0xa5a5a5a5;
                if (((uint32_t *)base.map)[i] != expected)
                    guards = 0;
            }
            EXPECT(guards, "vertex/fragment storage reads honor dynamic descriptor offsets without "
                           "modifying guards");
            free_buffer(base);
            vkDestroyQueryPool(device, query, NULL);
            free_buffer(pixels);
            vkDestroyPipeline(device, reader, NULL);
            for (unsigned sid = 0; sid < samples; sid++)
                vkDestroyPipeline(device, writers[sid], NULL);
            vkDestroySampler(device, sampler, NULL);
            vkDestroyDescriptorPool(device, dp, NULL);
            vkDestroyPipelineLayout(device, layout, NULL);
            vkDestroyDescriptorSetLayout(device, sl, NULL);
            for (unsigned i = 0; i < 2; i++) {
                vkDestroyFramebuffer(device, fb[i], NULL);
                vkDestroyImageView(device, views[i], NULL);
            }
            vkDestroyRenderPass(device, read_pass, NULL);
            vkDestroyRenderPass(device, write_pass, NULL);
            free_image(out);
            free_image(src);
        }
}
