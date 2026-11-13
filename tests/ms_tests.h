/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Different values in each sample distinguish a true multisample lookup from
 * an accidental resolve. Array views deliberately exclude the first layer. */
static void ms_tests(void)
{
    for (unsigned depth_kind = 0; depth_kind < 3; depth_kind++)
        for (unsigned numeric = 0; numeric < 4; numeric++)
            for (unsigned input = 0; input < 2; input++)
                for (unsigned array = 0; array < 2; array++) {
                    VkFormat depth_format = depth_kind == 0   ? VK_FORMAT_D16_UNORM
                                            : depth_kind == 1 ? VK_FORMAT_D32_SFLOAT
                                                              : VK_FORMAT_D24_UNORM_S8_UINT;
                    VkFormat format = numeric == 1   ? VK_FORMAT_R32G32B32A32_UINT
                                      : numeric == 2 ? VK_FORMAT_R32G32B32A32_SINT
                                      : numeric == 3 ? VK_FORMAT_B8G8R8A8_UNORM
                                                     : VK_FORMAT_R8G8B8A8_UNORM;
                    VkImageFormatProperties props;
                    VK(vkGetPhysicalDeviceImageFormatProperties(
                        physical, format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 0,
                        &props));
                    for (unsigned samples = 2; samples <= 4; samples *= 2) {
                        if (!(props.sampleCounts & samples))
                            continue;
                        struct image source = image_config(32, 32, format,
                                                           VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                                               VK_IMAGE_USAGE_SAMPLED_BIT |
                                                               VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT,
                                                           1, array ? 256 : 1, 0, samples);
                        struct image depth = image_config(
                            32, 32, depth_format,
                            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT,
                            1, array ? 256 : 1, 0, samples);
                        struct image output = image(32, 32, VK_FORMAT_R8G8B8A8_UNORM,
                                                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
                        struct buffer result =
                            buffer(16 + 40 * 32 * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
                        VkAttachmentDescription attachments[3] = {
                            {.format = format,
                             .samples = samples,
                             .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                             .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                             .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                             .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                             .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                             .finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                            {.format = depth_format,
                             .samples = samples,
                             .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                             .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                             .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                             .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                             .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                             .finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
                        VkAttachmentReference
                            color_ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
                            depth_ref = {1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
                        VkSubpassDescription sub = {.pipelineBindPoint =
                                                        VK_PIPELINE_BIND_POINT_GRAPHICS,
                                                    .colorAttachmentCount = 1,
                                                    .pColorAttachments = &color_ref,
                                                    .pDepthStencilAttachment = &depth_ref};
                        VkSubpassDependency dep = {
                            .srcSubpass = 0,
                            .dstSubpass = VK_SUBPASS_EXTERNAL,
                            .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                            VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                            .dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                            .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                             VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT};
                        VkRenderPassCreateInfo pi = {.sType =
                                                         VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                                     .attachmentCount = 2,
                                                     .pAttachments = attachments,
                                                     .subpassCount = 1,
                                                     .pSubpasses = &sub,
                                                     .dependencyCount = 1,
                                                     .pDependencies = &dep};
                        VkRenderPass write_pass, read_pass;
                        VkAttachmentReference output_ref = {
                            2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
                        VkAttachmentReference inputs[2] = {
                            {0, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                            {1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
                        VkSubpassDescription subs[2] = {
                            sub,
                            {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                             .colorAttachmentCount = 1,
                             .pColorAttachments = &output_ref,
                             .inputAttachmentCount = 2,
                             .pInputAttachments = inputs}};
                        VkSubpassDependency deps[2] = {
                            dep,
                            {.srcSubpass = 1,
                             .dstSubpass = VK_SUBPASS_EXTERNAL,
                             .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             .dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT,
                             .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                             .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT}};
                        if (input) {
                            attachments[2] = attachments[0];
                            attachments[2].samples = VK_SAMPLE_COUNT_1_BIT;
                            attachments[2].format = VK_FORMAT_R8G8B8A8_UNORM;
                            attachments[2].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
                            attachments[2].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                            deps[0].dstSubpass = 1;
                            deps[0].dstStageMask |= VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                                    VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
                            deps[0].dstAccessMask = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT |
                                                   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                                   VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
                            pi.attachmentCount = 3;
                            pi.subpassCount = 2;
                            pi.pSubpasses = subs;
                            pi.dependencyCount = 2;
                            pi.pDependencies = deps;
                        }
                        VK(vkCreateRenderPass(device, &pi, NULL, &write_pass));
                        VkAttachmentDescription output_attachment = attachments[0];
                        output_attachment.format = VK_FORMAT_R8G8B8A8_UNORM;
                        output_attachment.samples = VK_SAMPLE_COUNT_1_BIT;
                        output_attachment.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                        sub.pDepthStencilAttachment = NULL;
                        dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
                        dep.dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
                        dep.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                        dep.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                        pi.attachmentCount = 1;
                        pi.pAttachments = &output_attachment;
                        pi.subpassCount = 1;
                        pi.pSubpasses = &sub;
                        pi.dependencyCount = 1;
                        pi.pDependencies = &dep;
                        VK(vkCreateRenderPass(device, &pi, NULL, &read_pass));
                        VkDescriptorSetLayoutBinding bindings[2] = {
                            {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                             VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
                            {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                             VK_SHADER_STAGE_FRAGMENT_BIT, NULL}};
                        if (input)
                            for (unsigned i = 0; i < 2; i++)
                                bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
                        VkDescriptorSetLayoutCreateInfo si = {
                            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                            .bindingCount = 2,
                            .pBindings = bindings};
                        VkDescriptorSetLayout set_layout;
                        VK(vkCreateDescriptorSetLayout(device, &si, NULL, &set_layout));
                        VkPushConstantRange push = {
                            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32};
                        VkPipelineLayoutCreateInfo li = {
                            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                            .setLayoutCount = 1,
                            .pSetLayouts = &set_layout,
                            .pushConstantRangeCount = 1,
                            .pPushConstantRanges = &push};
                        VkPipelineLayout layout;
                        VK(vkCreatePipelineLayout(device, &li, NULL, &layout));
                        VkDescriptorPoolSize capacity = {bindings[0].descriptorType, 2};
                        VkDescriptorPoolCreateInfo dpi = {
                            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                            .maxSets = 1,
                            .poolSizeCount = 1,
                            .pPoolSizes = &capacity};
                        VkDescriptorPool dp;
                        VK(vkCreateDescriptorPool(device, &dpi, NULL, &dp));
                        VkDescriptorSetAllocateInfo ai = {
                            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                            .descriptorPool = dp,
                            .descriptorSetCount = 1,
                            .pSetLayouts = &set_layout};
                        VkDescriptorSet set;
                        VK(vkAllocateDescriptorSets(device, &ai, &set));
                        VkSamplerCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                                   .magFilter = VK_FILTER_NEAREST,
                                                   .minFilter = VK_FILTER_NEAREST,
                                                   .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST};
                        VkSampler sampler;
                        VK(vkCreateSampler(device, &sci, NULL, &sampler));
                        VkPipeline writers[4];
                        pipeline_samples = samples;
                        for (unsigned s = 0; s < samples; s++) {
                            uint32_t mask = 1u << s;
                            pipeline_sample_mask = &mask;
                            writers[s] = pipeline_shaders(ms_write_vert, sizeof ms_write_vert, NULL,
                                                          write_pass, layout,
                                                          numeric == 1   ? ms_uint_write_frag
                                                          : numeric == 2 ? ms_sint_write_frag
                                                                         : push_frag,
                                                          numeric == 1   ? sizeof ms_uint_write_frag
                                                          : numeric == 2 ? sizeof ms_sint_write_frag
                                                                         : sizeof push_frag,
                                                          0, 1, 0);
                        }
                        pipeline_samples = VK_SAMPLE_COUNT_1_BIT;
                        pipeline_sample_mask = NULL;
                        pipeline_subpass = input ? 1 : 0;
                        const uint32_t *reader_code =
                            numeric == 1   ? (input   ? ms_uint_input_frag
                                              : array ? ms_uint_array_fetch_frag
                                                      : ms_uint_fetch_frag)
                            : numeric == 2 ? (input   ? ms_sint_input_frag
                                              : array ? ms_sint_array_fetch_frag
                                                      : ms_sint_fetch_frag)
                            : input        ? ms_input_frag
                            : array        ? ms_array_fetch_frag
                                           : ms_fetch_frag;
                        size_t reader_size =
                            numeric == 1   ? (input   ? sizeof ms_uint_input_frag
                                              : array ? sizeof ms_uint_array_fetch_frag
                                                      : sizeof ms_uint_fetch_frag)
                            : numeric == 2 ? (input   ? sizeof ms_sint_input_frag
                                              : array ? sizeof ms_sint_array_fetch_frag
                                                      : sizeof ms_sint_fetch_frag)
                            : input        ? sizeof ms_input_frag
                            : array        ? sizeof ms_array_fetch_frag
                                           : sizeof ms_fetch_frag;
                        VkPipeline reader =
                            pipeline_shaders(ms_write_vert, sizeof ms_write_vert, NULL,
                                             input ? write_pass : read_pass, layout, reader_code,
                                             reader_size, 0, 0, 0);
                        pipeline_subpass = 0;
                        VkImageView color_views[2], depth_views[2], sample_views[2];
                        VkFramebuffer source_fb[2], output_fb;
                        VkFramebufferCreateInfo fi = {.sType =
                                                          VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                                      .renderPass = read_pass,
                                                      .attachmentCount = 1,
                                                      .pAttachments = &output.view,
                                                      .width = 32,
                                                      .height = 32,
                                                      .layers = 1};
                        VK(vkCreateFramebuffer(device, &fi, NULL, &output_fb));
                        unsigned layers = array ? 2 : 1;
                        for (unsigned l = 0; l < layers; l++) {
                            VkImageViewCreateInfo vi = {
                                .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                .image = source.handle,
                                .format = format,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1,
                                                     array ? l + 254 : 0, 1}};
                            VK(vkCreateImageView(device, &vi, NULL, color_views + l));
                            vi.image = depth.handle;
                            vi.format = depth_format;
                            vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
                            VK(vkCreateImageView(device, &vi, NULL, depth_views + l));
                            VkImageView pair[] = {color_views[l], depth_views[l], output.view};
                            fi.renderPass = write_pass;
                            fi.attachmentCount = input ? 3 : 2;
                            fi.pAttachments = pair;
                            VK(vkCreateFramebuffer(device, &fi, NULL, source_fb + l));
                            if (input)
                                continue;
                            begin();
                            begin_render(write_pass, source_fb[l], writers[0], (struct buffer){0},
                                         2);
                            for (unsigned s = 0; s < samples; s++) {
                                float values[] = {.2f + .15f * s + .05f * l, 0,
                                                  .8f - .1f * s - .05f * l, 1,
                                                  .2f + .1f * s + .1f * l};
                                if (numeric == 1 || numeric == 2) {
                                    int32_t integers[4] = {
                                        lroundf(255 * values[0]) - (numeric == 2 ? 128 : 0), 0,
                                        lroundf(255 * values[2]) - (numeric == 2 ? 128 : 0),
                                        numeric == 2 ? -1 : 255};
                                    memcpy(values, integers, 16);
                                }
                                vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                                  writers[s]);
                                vkCmdPushConstants(command, layout, push.stageFlags, 0,
                                                   sizeof values, values);
                                vkCmdDraw(command, 3, 1, 0, 0);
                            }
                            vkCmdEndRenderPass(command);
                            submit();
                        }
                        for (unsigned i = 0; i < 2; i++) {
                            VkImageViewCreateInfo vi = {
                                .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                .viewType =
                                    array ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D,
                                .image = i ? depth.handle : source.handle,
                                .format = i ? depth_format : format,
                                .subresourceRange = {i ? VK_IMAGE_ASPECT_DEPTH_BIT
                                                       : VK_IMAGE_ASPECT_COLOR_BIT,
                                                     0, 1, array ? 254 : 0, layers}};
                            VK(vkCreateImageView(device, &vi, NULL, sample_views + i));
                        }
                        VkDescriptorImageInfo infos[2] = {
                            {sampler, sample_views[0], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                            {sampler, sample_views[1], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
                        VkWriteDescriptorSet writes[2];
                        for (unsigned i = 0; i < 2; i++)
                            writes[i] = (VkWriteDescriptorSet){
                                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                .dstSet = set,
                                .dstBinding = i,
                                .descriptorCount = 1,
                                .descriptorType = bindings[i].descriptorType,
                                .pImageInfo = infos + i};
                        vkUpdateDescriptorSets(device, 2, writes, 0, NULL);
                        for (unsigned l = 0; l < layers; l++)
                            for (unsigned s = 0; s < samples; s++) {
                                begin();
                                if (input) {
                                    infos[0].imageView = color_views[l];
                                    infos[1].imageView = depth_views[l];
                                    vkUpdateDescriptorSets(device, 2, writes, 0, NULL);
                                    begin_render(write_pass, source_fb[l], writers[0],
                                                 (struct buffer){0}, 2);
                                    for (unsigned sample = 0; sample < samples; sample++) {
                                        float values[] = {.2f + .15f * sample + .05f * l, 0,
                                                          .8f - .1f * sample - .05f * l, 1,
                                                          .2f + .1f * sample + .1f * l};
                                        if (numeric == 1 || numeric == 2) {
                                            int32_t integers[4] = {
                                                lroundf(255 * values[0]) - (numeric == 2 ? 128 : 0),
                                                0,
                                                lroundf(255 * values[2]) - (numeric == 2 ? 128 : 0),
                                                numeric == 2 ? -1 : 255};
                                            memcpy(values, integers, 16);
                                        }
                                        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                                          writers[sample]);
                                        vkCmdPushConstants(command, layout, push.stageFlags, 0,
                                                           sizeof values, values);
                                        vkCmdDraw(command, 3, 1, 0, 0);
                                    }
                                    vkCmdNextSubpass(command, VK_SUBPASS_CONTENTS_INLINE);
                                    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                                      reader);
                                } else
                                    begin_render(read_pass, output_fb, reader, (struct buffer){0},
                                                 1);
                                int32_t values[] = {s, l, samples, 0, 0};
                                vkCmdPushConstants(command, layout, push.stageFlags, 0,
                                                   sizeof values, values);
                                vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                                        layout, 0, 1, &set, 0, NULL);
                                vkCmdDraw(command, 3, 1, 0, 0);
                                vkCmdEndRenderPass(command);
                                readback(output.handle, result);
                                submit();
                                EXPECT(
                                    pixel(result, 16, 16,
                                          lroundf(255 * (.2f + .15f * s + .05f * l)),
                                          lroundf(255 * (.2f + .1f * s + .1f * l)),
                                          lroundf(255 * (.8f - .1f * s - .05f * l)), 2),
                                    input
                                        ? "GPU multisample color/depth input attachments preserve "
                                          "individual samples and nonzero layers"
                                        : "GPU multisample color/depth fetch preserves individual "
                                          "samples, queries and restricted array layers");
                            }
                        if (!input) {
                            VkImageViewCreateInfo vi = {
                                .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                .image = source.handle,
                                .viewType =
                                    array ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D,
                                .format = format,
                                .components = {VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_G,
                                               VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_ONE},
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, array ? 254 : 0,
                                                     layers}};
                            VkImageView swapped;
                            VK(vkCreateImageView(device, &vi, NULL, &swapped));
                            infos[0].imageView = swapped;
                            vkUpdateDescriptorSets(device, 2, writes, 0, NULL);
                            begin();
                            begin_render(read_pass, output_fb, reader, (struct buffer){0}, 1);
                            int32_t values[] = {0, 0, samples, 1, 0};
                            vkCmdPushConstants(command, layout, push.stageFlags, 0, sizeof values,
                                               values);
                            vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                                    layout, 0, 1, &set, 0, NULL);
                            vkCmdDraw(command, 3, 1, 0, 0);
                            vkCmdEndRenderPass(command);
                            readback(output.handle, result);
                            submit();
                            EXPECT(pixel(result, 16, 16, 204, 51, 51, 2),
                                   "GPU multisample view swizzles retain typed constant-one "
                                   "components");
                            vkDestroyImageView(device, swapped, NULL);
                        }
                        for (unsigned i = 0; i < 2; i++)
                            vkDestroyImageView(device, sample_views[i], NULL);
                        for (unsigned l = 0; l < layers; l++) {
                            vkDestroyFramebuffer(device, source_fb[l], NULL);
                            vkDestroyImageView(device, color_views[l], NULL);
                            vkDestroyImageView(device, depth_views[l], NULL);
                        }
                        for (unsigned s = 0; s < samples; s++)
                            vkDestroyPipeline(device, writers[s], NULL);
                        vkDestroyPipeline(device, reader, NULL);
                        vkDestroySampler(device, sampler, NULL);
                        vkDestroyDescriptorPool(device, dp, NULL);
                        vkDestroyPipelineLayout(device, layout, NULL);
                        vkDestroyDescriptorSetLayout(device, set_layout, NULL);
                        vkDestroyFramebuffer(device, output_fb, NULL);
                        vkDestroyRenderPass(device, read_pass, NULL);
                        vkDestroyRenderPass(device, write_pass, NULL);
                        free_buffer(result);
                        free_image(output);
                        free_image(depth);
                        free_image(source);
                    }
                }
}
static void ms_empty_tests(void)
{
    VkSubpassDescription sub = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS};
    VkRenderPassCreateInfo pi = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .subpassCount = 1, .pSubpasses = &sub};
    VkRenderPass pass;
    VK(vkCreateRenderPass(device, &pi, NULL, &pass));
    VkFramebufferCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                  .renderPass = pass,
                                  .width = 32,
                                  .height = 32,
                                  .layers = 1};
    VkFramebuffer fb;
    VK(vkCreateFramebuffer(device, &fi, NULL, &fb));
    VkPushConstantRange push = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32};
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                     .pushConstantRangeCount = 1,
                                     .pPushConstantRanges = &push};
    VkPipelineLayout layout;
    VK(vkCreatePipelineLayout(device, &li, NULL, &layout));
    VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                .queryType = VK_QUERY_TYPE_OCCLUSION,
                                .queryCount = 1};
    VkQueryPool queries;
    VK(vkCreateQueryPool(device, &qi, NULL, &queries));
    for (unsigned samples = 1; samples <= 4; samples *= 2) {
        if (!(properties.limits.framebufferColorSampleCounts & samples))
            continue;
        for (unsigned mode = 0; mode < 5u; mode++) {
            if (samples == 1 && mode == 3)
                continue;
            uint32_t mask = mode ? ~0u : 0;
            pipeline_samples = samples;
            pipeline_color_count = 0;
            pipeline_sample_mask = &mask;
            pipeline_alpha_coverage = mode >= 2;
            VkPipeline render = pipeline_shaders(ms_write_vert, sizeof ms_write_vert, NULL, pass,
                                                 layout, push_frag, sizeof push_frag, 0, 0, 0);
            pipeline_samples = VK_SAMPLE_COUNT_1_BIT;
            pipeline_color_count = -1;
            pipeline_sample_mask = NULL;
            pipeline_alpha_coverage = 0;
            float alpha = mode == 2 ? 0 : mode == 3 ? .5f : 1;
            float values[] = {1, 0, 0, alpha, .5f};
            begin();
            vkCmdResetQueryPool(command, queries, 0, 1);
            begin_render(pass, fb, render, (struct buffer){0}, 0);
            vkCmdPushConstants(command, layout, push.stageFlags, 0, sizeof values, values);
            vkCmdBeginQuery(command, queries, 0, 0);
            vkCmdDraw(command, 3, 1, 0, 0);
            vkCmdEndQuery(command, queries, 0);
            vkCmdEndRenderPass(command);
            submit();
            uint64_t count = 0;
            VK(vkGetQueryPoolResults(device, queries, 0, 1, sizeof count, &count, sizeof count,
                                     VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
            uint64_t full = 1024 * samples;
            EXPECT(mode == 0 || mode == 2 ? count == 0
                   : mode == 3            ? count > 0 && count < full
                                          : count == full,
                   mode >= 2
                       ? "GPU alpha-to-coverage produces zero, partial and full sample coverage"
                       : "attachment-free rasterization honors pipeline sample counts and sample "
                         "masks");
            vkDestroyPipeline(device, render, NULL);
        }
    }
    vkDestroyQueryPool(device, queries, NULL);
    vkDestroyPipelineLayout(device, layout, NULL);
    vkDestroyFramebuffer(device, fb, NULL);
    vkDestroyRenderPass(device, pass, NULL);
}

/* A depth-only draw still consumes COLOR0 alpha for coverage. Verify the
 * depth it leaves behind, rather than only its own occlusion result. */
static void ms_depth_coverage_tests(void)
{
    VkPushConstantRange push={VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,20};
    VkPipelineLayoutCreateInfo li={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pushConstantRangeCount=1,.pPushConstantRanges=&push};
    VkPipelineLayout layout;VK(vkCreatePipelineLayout(device,&li,NULL,&layout));
    VkQueryPoolCreateInfo qi={.sType=VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
        .queryType=VK_QUERY_TYPE_OCCLUSION,.queryCount=1};
    VkQueryPool queries;VK(vkCreateQueryPool(device,&qi,NULL,&queries));
    VkImageFormatProperties props;
    VK(vkGetPhysicalDeviceImageFormatProperties(physical,VK_FORMAT_D16_UNORM,VK_IMAGE_TYPE_2D,
        VK_IMAGE_TILING_OPTIMAL,VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,0,&props));
    for (unsigned samples=1;samples<=4;samples*=2) {
        if (!(props.sampleCounts&samples))continue;
        struct image depth=image_config(32,32,VK_FORMAT_D16_UNORM,
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,1,1,0,samples);
        VkAttachmentDescription attachment={.format=VK_FORMAT_D16_UNORM,.samples=samples,
            .loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR,.storeOp=VK_ATTACHMENT_STORE_OP_STORE,
            .initialLayout=VK_IMAGE_LAYOUT_UNDEFINED,
            .finalLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkAttachmentReference ref={0,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub={.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS,
            .pDepthStencilAttachment=&ref};
        VkRenderPassCreateInfo pi={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
            .attachmentCount=1,.pAttachments=&attachment,.subpassCount=1,.pSubpasses=&sub};
        VkRenderPass pass;VK(vkCreateRenderPass(device,&pi,NULL,&pass));
        VkFramebufferCreateInfo fi={.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass=pass,.attachmentCount=1,.pAttachments=&depth.view,.width=32,.height=32,.layers=1};
        VkFramebuffer fb;VK(vkCreateFramebuffer(device,&fi,NULL,&fb));
        pipeline_samples=samples;pipeline_color_count=0;pipeline_alpha_coverage=1;
        VkPipeline writer=pipeline_shaders(ms_write_vert,sizeof ms_write_vert,NULL,pass,layout,
            push_frag,sizeof push_frag,0,1,0);
        pipeline_alpha_coverage=0;
        VkPipeline reader=pipeline_shaders(ms_write_vert,sizeof ms_write_vert,NULL,pass,layout,
            push_frag,sizeof push_frag,0,1,0);
        pipeline_samples=VK_SAMPLE_COUNT_1_BIT;pipeline_color_count=-1;
        for (unsigned opaque=0;opaque<2;opaque++) {
            begin();vkCmdResetQueryPool(command,queries,0,1);
            VkClearValue clear={.depthStencil={1.0f,0}};
            VkRenderPassBeginInfo bi={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                .renderPass=pass,.framebuffer=fb,.renderArea={{0,0},{32,32}},
                .clearValueCount=1,.pClearValues=&clear};
            vkCmdBeginRenderPass(command,&bi,VK_SUBPASS_CONTENTS_INLINE);
            VkViewport vp={0,0,32,32,0,1};VkRect2D sc={{0,0},{32,32}};
            vkCmdSetViewport(command,0,1,&vp);vkCmdSetScissor(command,0,1,&sc);
            float values[]={1,0,0,(float)opaque,.25f};
            vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,writer);
            vkCmdPushConstants(command,layout,push.stageFlags,0,sizeof values,values);
            vkCmdDraw(command,3,1,0,0);
            values[3]=1;values[4]=.5f;
            vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,reader);
            vkCmdPushConstants(command,layout,push.stageFlags,0,sizeof values,values);
            vkCmdBeginQuery(command,queries,0,0);vkCmdDraw(command,3,1,0,0);
            vkCmdEndQuery(command,queries,0);vkCmdEndRenderPass(command);submit();
            uint64_t count=UINT64_MAX;
            VK(vkGetQueryPoolResults(device,queries,0,1,sizeof count,&count,sizeof count,
                VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WAIT_BIT));
            EXPECT(opaque?count==0:count==1024*samples,
                "depth-only alpha-to-coverage preserves or updates every depth sample");
        }
        vkDestroyPipeline(device,reader,NULL);vkDestroyPipeline(device,writer,NULL);
        vkDestroyFramebuffer(device,fb,NULL);vkDestroyRenderPass(device,pass,NULL);free_image(depth);
    }
    vkDestroyQueryPool(device,queries,NULL);vkDestroyPipelineLayout(device,layout,NULL);
}
