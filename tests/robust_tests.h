/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Check descriptor boundaries inside larger allocations, not just allocation
 * ends. Guard words expose writes accidentally escaping the descriptor. */
static void robust_compute_tests(void)
{
    if (!properties.limits.maxComputeWorkGroupInvocations)
        return;
    VkDescriptorType types[] = {
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
        VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER};
    struct compute_case c = compute_case(robust_comp, sizeof robust_comp, 5, types);
    struct buffer results = buffer(64, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    struct buffer source = buffer(64, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    struct buffer params = buffer(512, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    struct buffer uniform_texels = buffer(64, VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT);
    struct buffer storage_texels = buffer(64, VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT);
    for (unsigned i = 0; i < 16; i++) {
        ((uint32_t *)source.map)[i] = 10 + i;
        ((uint32_t *)params.map)[i] = 20 + i;
        ((uint32_t *)params.map)[64 + i] = 24 + i;
        ((uint32_t *)uniform_texels.map)[i] = 30 + i;
        ((uint32_t *)storage_texels.map)[i] = 40 + i;
    }
    VkBufferView views[2];
    VkBufferViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO,
                                 .format = VK_FORMAT_R32_UINT,
                                 .offset = 16,
                                 .range = 8};
    vi.buffer = uniform_texels.handle;
    VK(vkCreateBufferView(device, &vi, NULL, views));
    vi.buffer = storage_texels.handle;
    VK(vkCreateBufferView(device, &vi, NULL, views + 1));
    VkDescriptorBufferInfo infos[] = {
        {results.handle, 0, 64}, {source.handle, 16, 12}, {params.handle, 256, 4}};
    VkWriteDescriptorSet writes[5] = {{0}};
    for (unsigned i = 0; i < 5; i++)
        writes[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                           .dstSet = c.set,
                                           .dstBinding = i,
                                           .descriptorCount = 1,
                                           .descriptorType = types[i],
                                           .pBufferInfo = i < 3 ? infos + i : NULL,
                                           .pTexelBufferView = i >= 3 ? views + (i - 3) : NULL};
    vkUpdateDescriptorSets(device, 5, writes, 0, NULL);
    begin();
    bind_compute_case(c);
    vkCmdDispatch(command, 4, 1, 1);
    compute_visibility(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                       VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
    submit();
    const uint32_t expected[] = {14, 24, 34, 44, 15, 0, 35, 45, 16, 0, 0, 0, 0, 0, 0, 0};
    EXPECT(!memcmp(results.map, expected, sizeof expected),
           "robust GPU buffer/UBO/texel reads respect descriptor ranges and negative indices");
    int safe = 1;
    for (unsigned i = 0; i < 16; i++) {
        unsigned s = i >= 4 && i < 7 ? 71 + i - 4 : 10 + i;
        unsigned t = i >= 4 && i < 6 ? 81 + i - 4 : 40 + i;
        if (((uint32_t *)source.map)[i] != s || ((uint32_t *)storage_texels.map)[i] != t)
            safe = 0;
    }
    EXPECT(safe, "robust stores and atomics preserve guards before and after SSBO/texel views");
    vkDestroyBufferView(device, views[0], NULL);
    vkDestroyBufferView(device, views[1], NULL);
    free_compute_case(c);
    free_buffer(storage_texels);
    free_buffer(uniform_texels);
    free_buffer(params);
    free_buffer(source);
    free_buffer(results);
}
static void robust_matrix_tests(void)
{
    if (!properties.limits.maxComputeWorkGroupInvocations)
        return;
    VkDescriptorType types[] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
    struct compute_case c = compute_case(robust_matrix_comp, sizeof robust_matrix_comp, 3, types);
    struct buffer input = buffer(512, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    struct buffer result = buffer(24, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    struct buffer output = buffer(128, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    for (unsigned i = 0; i < 32; i++) {
        ((float *)input.map)[i] = (float)i - 3;
        ((float *)input.map)[64 + i] = (float)i + 1;
        ((uint32_t *)output.map)[i] = 0xa5a5a5a5;
    }
    VkDescriptorBufferInfo infos[] = {
        {input.handle, 256, 68}, {result.handle, 0, 24}, {output.handle, 16, 68}};
    VkWriteDescriptorSet writes[3] = {{0}};
    for (unsigned i = 0; i < 3; i++)
        writes[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                           .dstSet = c.set,
                                           .dstBinding = i,
                                           .descriptorCount = 1,
                                           .descriptorType = types[i],
                                           .pBufferInfo = infos + i};
    vkUpdateDescriptorSets(device, 3, writes, 0, NULL);
    begin();
    bind_compute_case(c);
    vkCmdDispatch(command, 1, 1, 1);
    compute_visibility(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                       VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
    submit();
    const float expected[] = {13, 17, 0, 14, 0, 0};
    EXPECT(
        !memcmp(result.map, expected, sizeof expected),
        "robust non-square row-major matrix loads honor array/matrix strides and partial ranges");
    int safe = 1;
    for (unsigned i = 0; i < 32; i++) {
        uint32_t wanted = 0xa5a5a5a5;
        if (i == 16 || i == 17 || i == 20) {
            float value = (float)i - 3;
            memcpy(&wanted, &value, 4);
        }
        if (((uint32_t *)output.map)[i] != wanted)
            safe = 0;
    }
    EXPECT(safe, "robust row-major matrix stores preserve padding and descriptor guards");
    free_compute_case(c);
    free_buffer(output);
    free_buffer(result);
    free_buffer(input);
}
static void robust_graphics_tests(void)
{
    struct image output =
        image(32, 32, VK_FORMAT_R8G8B8A8_UNORM,
              VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    struct buffer result = buffer(16 + 40 * 32 * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    struct buffer vertices = buffer(64, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    for (unsigned i = 0; i < 16; i++)
        ((float *)vertices.map)[i] = 1;
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
    VkDescriptorSetLayoutBinding descriptor = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1,
                                               VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo si = {.sType =
                                              VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                          .bindingCount = 1,
                                          .pBindings = &descriptor};
    VkDescriptorSetLayout set_layout;
    VK(vkCreateDescriptorSetLayout(device, &si, NULL, &set_layout));
    VkPushConstantRange push = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 8};
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                     .setLayoutCount = 1,
                                     .pSetLayouts = &set_layout,
                                     .pushConstantRangeCount = 1,
                                     .pPushConstantRanges = &push};
    VkPipelineLayout layout;
    VK(vkCreatePipelineLayout(device, &li, NULL, &layout));
    VkVertexInputBindingDescription vb = {0, 16, VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attr = {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0};
    VkPipelineVertexInputStateCreateInfo vertex = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1,
        .pVertexBindingDescriptions = &vb,
        .vertexAttributeDescriptionCount = 1,
        .pVertexAttributeDescriptions = &attr};
    struct buffer indices = buffer(16, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    uint32_t *ix = indices.map;
    for (unsigned test = 0; test < 8; test++) {
        int32_t constants[2] = {0, test == 0 ? 1 : 0};
        vb.inputRate = test == 6 ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX;
        vb.stride = test == 7 ? 0 : 16;
        pipeline_vertex = &vertex;
        VkPipeline render = pipeline_shaders(robust_vert, sizeof robust_vert, NULL, pass, layout,
                                             triangle_frag, sizeof triangle_frag, 0, 0, 0);
        pipeline_vertex = NULL;
        if (test == 1 || test == 3)
            constants[0] = 5;
        if (test == 2)
            constants[0] = 1000000;
        if (test == 4) {
            constants[0] = -2;
            constants[1] = 2;
        }
        if (test == 7)
            constants[1] = 1;
        begin();
        begin_render(pass, fb, render, vertices, 1);
        VkDeviceSize offset = test == 5 ? 60 : 0;
        vkCmdBindVertexBuffers(command, 0, 1, &vertices.handle, &offset);
        vkCmdPushConstants(command, layout, push.stageFlags, 0, 8, constants);
        if (test == 3 || test == 4) {
            ix[0] = test == 3 ? 5 : 0;
            ix[1] = ix[0] + 1;
            ix[2] = ix[0] + 2;
            vkCmdBindIndexBuffer(command, indices.handle, 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(command, 3, 1, 0, test == 4 ? -2 : 0, 0);
        } else {
            vkCmdDraw(command, 3, 1, constants[0], test == 6 ? 9 : 0);
        }
        vkCmdEndRenderPass(command);
        readback(output.handle, result);
        submit();
        const char *labels[] = {
            "robust vertex fetch retains in-range data",
            "robust vertex fetch bounds a nonzero firstVertex",
            "robust vertex fetch handles an absent millionth vertex without a huge allocation",
            "robust indexed vertex fetch bounds indices beyond the buffer",
            "robust negative baseVertex preserves valid neighboring data and VertexIndex",
            "robust vertex fetch zeros a partially present attribute",
            "robust instance fetch applies firstInstance before checking bounds",
            "robust zero-stride vertex fetch preserves constant in-range data"};
        EXPECT(pixel(result, 16, 16, 0, 255, 0, 0), labels[test]);

        vkDestroyPipeline(device, render, NULL);
    }
    VkDescriptorPoolSize capacity = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1};
    VkDescriptorPoolCreateInfo dpi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                      .maxSets = 1,
                                      .poolSizeCount = 1,
                                      .pPoolSizes = &capacity};
    VkDescriptorPool dp;
    VK(vkCreateDescriptorPool(device, &dpi, NULL, &dp));
    VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                      .descriptorPool = dp,
                                      .descriptorSetCount = 1,
                                      .pSetLayouts = &set_layout};
    VkDescriptorSet set;
    VK(vkAllocateDescriptorSets(device, &ai, &set));
    struct buffer uniform = buffer(1024, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    float *u = uniform.map;
    for (unsigned i = 0; i < 256; i++)
        u[i] = 1;
    VkDescriptorBufferInfo info = {uniform.handle, 256, 4};
    VkWriteDescriptorSet write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                  .dstSet = set,
                                  .dstBinding = 0,
                                  .descriptorCount = 1,
                                  .descriptorType = descriptor.descriptorType,
                                  .pBufferInfo = &info};
    vkUpdateDescriptorSets(device, 1, &write, 0, NULL);
    VkPipeline render = pipeline_shaders(builtin_vert, sizeof builtin_vert, NULL, pass, layout,
                                         robust_ubo_frag, sizeof robust_ubo_frag, 0, 0, 0);
    const int32_t indices_to_test[] = {-1, 0, 2, 3};
    for (unsigned i = 0; i < 4; i++) {
        begin();
        begin_render(pass, fb, render, vertices, 1);
        uint32_t extra = 256;
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 1,
                                &extra);
        vkCmdPushConstants(command, layout, push.stageFlags, 0, 4, indices_to_test + i);
        vkCmdDraw(command, 3, 1, 0, 0);
        vkCmdEndRenderPass(command);
        readback(output.handle, result);
        submit();
        uint8_t *p = (uint8_t *)result.map + 16 + (16 * 40 + 16) * 4;
        EXPECT(p[0] == (i == 0 ? 255 : 0) && p[1] == 0 && p[2] == 0 && p[3] == 0,
               "robust dynamic UBO ranges zero missing components and out-of-array loop loads");
    }
    vkDestroyPipeline(device, render, NULL);
    free_buffer(uniform);
    vkDestroyDescriptorPool(device, dp, NULL);
    vkDestroyPipelineLayout(device, layout, NULL);
    vkDestroyDescriptorSetLayout(device, set_layout, NULL);
    free_buffer(indices);
    vkDestroyFramebuffer(device, fb, NULL);
    vkDestroyRenderPass(device, pass, NULL);
    free_buffer(vertices);
    free_buffer(result);
    free_image(output);
}
static void robust_tests(void)
{
    VkPhysicalDeviceFeatures features;
    vkGetPhysicalDeviceFeatures(physical, &features);
    EXPECT(features.robustBufferAccess,
           "robustBufferAccess is available and enabled for these tests");
    robust_compute_tests();
    robust_matrix_tests();
    robust_graphics_tests();
}
