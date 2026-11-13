/* SPDX-License-Identifier: LGPL-2.1-or-later */
static void texture_operand_tests(void)
{
    struct image source = image(16, 16, VK_FORMAT_R8G8B8A8_UNORM,
                                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    struct image output =
        image(32, 32, VK_FORMAT_R8G8B8A8_UNORM,
              VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    struct buffer staging = buffer(16 * 16 * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    struct buffer result = buffer(16 + 40 * 32 * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    for (unsigned y = 0; y < 16; y++)
        for (unsigned x = 0; x < 16; x++) {
            uint8_t *pixel = (uint8_t *)staging.map + (y * 16 + x) * 4;
            pixel[0] = 16 * x;
            pixel[1] = 16 * y;
            pixel[2] = 32;
            pixel[3] = 255;
        }
    begin();
    barrier(source.handle, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy upload = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                .imageExtent = {16, 16, 1}};
    vkCmdCopyBufferToImage(command, staging.handle, source.handle,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &upload);
    barrier(source.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    submit();
    VkSampler samplers[2];
    VkSamplerCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                               .magFilter = VK_FILTER_NEAREST,
                               .minFilter = VK_FILTER_NEAREST,
                               .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                               .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
                               .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT};
    VK(vkCreateSampler(device, &sci, NULL, samplers));
    sci.addressModeU = sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VK(vkCreateSampler(device, &sci, NULL, samplers + 1));
    VkSampler immutable = samplers[1];
    VkDescriptorSetLayoutBinding bindings[] = {
        {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {2, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, &immutable}};
    VkDescriptorSetLayoutCreateInfo si = {.sType =
                                              VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                          .bindingCount = 3,
                                          .pBindings = bindings};
    VkDescriptorSetLayout set_layout;
    VK(vkCreateDescriptorSetLayout(device, &si, NULL, &set_layout));
    immutable = VK_NULL_HANDLE;
    VkPushConstantRange push = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32};
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                     .setLayoutCount = 1,
                                     .pSetLayouts = &set_layout,
                                     .pushConstantRangeCount = 1,
                                     .pPushConstantRanges = &push};
    VkPipelineLayout layout;
    VK(vkCreatePipelineLayout(device, &li, NULL, &layout));
    VkDescriptorPoolSize capacities[] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1},
                                         {VK_DESCRIPTOR_TYPE_SAMPLER, 2}};
    VkDescriptorPoolCreateInfo dpi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                      .maxSets = 1,
                                      .poolSizeCount = 2,
                                      .pPoolSizes = capacities};
    VkDescriptorPool dp;
    VK(vkCreateDescriptorPool(device, &dpi, NULL, &dp));
    VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                      .descriptorPool = dp,
                                      .descriptorSetCount = 1,
                                      .pSetLayouts = &set_layout};
    VkDescriptorSet set;
    VK(vkAllocateDescriptorSets(device, &ai, &set));
    VkDescriptorImageInfo infos[] = {{0, source.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                     {samplers[0], 0, VK_IMAGE_LAYOUT_UNDEFINED}};
    VkWriteDescriptorSet writes[2];
    for (unsigned i = 0; i < 2; i++)
        writes[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                           .dstSet = set,
                                           .dstBinding = i,
                                           .descriptorCount = 1,
                                           .descriptorType = bindings[i].descriptorType,
                                           .pImageInfo = infos + i};
    vkUpdateDescriptorSets(device, 2, writes, 0, NULL);
    VkAttachmentDescription attachment = {.format = VK_FORMAT_R8G8B8A8_UNORM,
                                          .samples = VK_SAMPLE_COUNT_1_BIT,
                                          .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                                          .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                          .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                          .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
    VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                .colorAttachmentCount = 1,
                                .pColorAttachments = &ref};
    VkSubpassDependency dep = {.srcSubpass = 0,
                               .dstSubpass = VK_SUBPASS_EXTERNAL,
                               .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                               .dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT,
                               .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                               .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT};
    VkRenderPassCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                 .attachmentCount = 1,
                                 .pAttachments = &attachment,
                                 .subpassCount = 1,
                                 .pSubpasses = &sub,
                                 .dependencyCount = 1,
                                 .pDependencies = &dep};
    VkRenderPass pass;
    VK(vkCreateRenderPass(device, &pi, NULL, &pass));
    VkFramebufferCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                  .renderPass = pass,
                                  .attachmentCount = 1,
                                  .pAttachments = &output.view,
                                  .width = 32,
                                  .height = 32,
                                  .layers = 1};
    VkFramebuffer fb;
    VK(vkCreateFramebuffer(device, &fi, NULL, &fb));
    VkPipeline render =
        pipeline_shaders(ms_write_vert, sizeof ms_write_vert, NULL, pass, layout,
                         texture_operands_frag, sizeof texture_operands_frag, 0, 0, 0);
    const unsigned positions[][2] = {{0, 0}, {15, 15}, {0, 15}, {15, 0}, {3, 1}, {15, 1}, {15, 1}};
    for (unsigned mode = 0; mode < 7; mode++) {
        if (mode == 6) {
            VkCopyDescriptorSet copy = {.sType = VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET,
                                        .srcSet = set,
                                        .srcBinding = 2,
                                        .dstSet = set,
                                        .dstBinding = 1,
                                        .descriptorCount = 1};
            vkUpdateDescriptorSets(device, 0, NULL, 1, &copy);
        }
        begin();
        begin_render(pass, fb, render, (struct buffer){0}, 1);
        int32_t value = mode == 6 ? 4 : mode;
        vkCmdPushConstants(command, layout, push.stageFlags, 0, 4, &value);
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0,
                                NULL);
        vkCmdDraw(command, 3, 1, 0, 0);
        vkCmdEndRenderPass(command);
        readback(output.handle, result);
        submit();
        EXPECT(pixel(result, 16, 16, 16 * positions[mode][0], 16 * positions[mode][1], 32, 0),
               "GPU texture offsets, LOD/bias/gradients and separate mutable/immutable sampler "
               "pairs select the expected texel");
    }
    vkDestroyPipeline(device, render, NULL);
    vkDestroyFramebuffer(device, fb, NULL);
    vkDestroyRenderPass(device, pass, NULL);
    vkDestroyDescriptorPool(device, dp, NULL);
    vkDestroyPipelineLayout(device, layout, NULL);
    vkDestroyDescriptorSetLayout(device, set_layout, NULL);
    for (unsigned i = 0; i < 2; i++)
        vkDestroySampler(device, samplers[i], NULL);
    free_buffer(result);
    free_buffer(staging);
    free_image(output);
    free_image(source);
}
