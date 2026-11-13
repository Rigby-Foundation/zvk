/* SPDX-License-Identifier: LGPL-2.1-or-later */
static void samplers_tests(void)
{
    if (!properties.limits.maxComputeWorkGroupInvocations)
        return;
    VkDescriptorSetLayout layouts[3];
    VkDescriptorSet sets[3];
    for (unsigned i = 0; i < 3; i++) {
        VkDescriptorSetLayoutBinding b[2] = {
            {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16,
             i == 0   ? VK_SHADER_STAGE_VERTEX_BIT
             : i == 1 ? VK_SHADER_STAGE_FRAGMENT_BIT
                      : VK_SHADER_STAGE_COMPUTE_BIT,
             NULL},
            {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
        VkDescriptorSetLayoutCreateInfo si = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = i == 2 ? 2 : 1,
            .pBindings = b};
        VK(vkCreateDescriptorSetLayout(device, &si, NULL, &layouts[i]));
    }
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                     .setLayoutCount = 3,
                                     .pSetLayouts = layouts};
    VkPipelineLayout layout;
    VK(vkCreatePipelineLayout(device, &li, NULL, &layout));
    VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 48},
                                     {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}};
    VkDescriptorPoolCreateInfo dpi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                      .maxSets = 3,
                                      .poolSizeCount = 2,
                                      .pPoolSizes = sizes};
    VkDescriptorPool pool;
    VK(vkCreateDescriptorPool(device, &dpi, NULL, &pool));
    VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                      .descriptorPool = pool,
                                      .descriptorSetCount = 3,
                                      .pSetLayouts = layouts};
    VK(vkAllocateDescriptorSets(device, &ai, sets));
    VkSamplerCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                               .magFilter = VK_FILTER_NEAREST,
                               .minFilter = VK_FILTER_NEAREST,
                               .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST};
    VkSampler sampler;
    VK(vkCreateSampler(device, &sci, NULL, &sampler));
    struct image textures[16];
    VkFramebuffer sources[16];
    VkDescriptorImageInfo images[16];
    VkAttachmentDescription attachment = {.format = VK_FORMAT_R8G8B8A8_UNORM,
                                          .samples = VK_SAMPLE_COUNT_4_BIT,
                                          .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                                          .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                          .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                          .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                          .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                          .finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                .colorAttachmentCount = 1,
                                .pColorAttachments = &ref};
    VkSubpassDependency dep = {.srcSubpass = 0,
                               .dstSubpass = VK_SUBPASS_EXTERNAL,
                               .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                               .dstStageMask = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                                               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                               .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                               .dstAccessMask = VK_ACCESS_SHADER_READ_BIT};
    VkRenderPassCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                 .attachmentCount = 1,
                                 .pAttachments = &attachment,
                                 .subpassCount = 1,
                                 .pSubpasses = &sub,
                                 .dependencyCount = 1,
                                 .pDependencies = &dep};
    VkRenderPass clear_pass, render_pass;
    VK(vkCreateRenderPass(device, &ri, NULL, &clear_pass));
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    dep.dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
    dep.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    VK(vkCreateRenderPass(device, &ri, NULL, &render_pass));
    struct image out = image(32, 32, VK_FORMAT_R8G8B8A8_UNORM,
                             VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    VkFramebufferCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                  .renderPass = render_pass,
                                  .attachmentCount = 1,
                                  .pAttachments = &out.view,
                                  .width = 32,
                                  .height = 32,
                                  .layers = 1};
    VkFramebuffer target;
    VK(vkCreateFramebuffer(device, &fi, NULL, &target));
    fi.renderPass = clear_pass;
    fi.width = fi.height = 2;
    for (unsigned i = 0; i < 16; i++) {
        textures[i] = image_config(2, 2, VK_FORMAT_R8G8B8A8_UNORM,
                                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                   1, 1, 0, VK_SAMPLE_COUNT_4_BIT);
        fi.pAttachments = &textures[i].view;
        VK(vkCreateFramebuffer(device, &fi, NULL, &sources[i]));
        images[i] = (VkDescriptorImageInfo){sampler, textures[i].view,
                                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    }
    struct buffer results = buffer(96, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    memset(results.map, 0xa5, 96);
    VkDescriptorBufferInfo info = {results.handle, 16, 64};
    VkWriteDescriptorSet writes[4];
    for (unsigned i = 0; i < 3; i++)
        writes[i] =
            (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                   .dstSet = sets[i],
                                   .dstBinding = 0,
                                   .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                   .descriptorCount = 16,
                                   .pImageInfo = images};
    writes[3] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                       .dstSet = sets[2],
                                       .dstBinding = 1,
                                       .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                       .descriptorCount = 1,
                                       .pBufferInfo = &info};
    vkUpdateDescriptorSets(device, 4, writes, 0, NULL);
    VkPipelineVertexInputStateCreateInfo vertex = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    pipeline_vertex = &vertex;
    VkPipeline graphics =
        pipeline_shaders(samplers48_vert, sizeof samplers48_vert, NULL, render_pass, layout,
                         samplers48_frag, sizeof samplers48_frag, 0, 0, 0);
    pipeline_vertex = NULL;
    VkShaderModule shader = module(samplers48_comp, sizeof samplers48_comp);
    VkComputePipelineCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .layout = layout,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                  .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                  .module = shader,
                  .pName = "main"}};
    VkPipeline compute;
    VK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &ci, NULL, &compute));
    vkDestroyShaderModule(device, shader, NULL);
    struct buffer pixels = buffer(32 * 32 * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    begin();
    VkClearValue clear;
    VkRenderPassBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                .renderPass = clear_pass,
                                .renderArea = {{0, 0}, {2, 2}},
                                .clearValueCount = 1,
                                .pClearValues = &clear};
    for (unsigned i = 0; i < 16; i++) {
        clear.color = (VkClearColorValue){
            .float32 = {i * 17.0f / 255.0f, (255.0f - i * 11.0f) / 255.0f, i * 7.0f / 255.0f, 1}};
        bi.framebuffer = sources[i];
        vkCmdBeginRenderPass(command, &bi, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdEndRenderPass(command);
    }
    clear = (VkClearValue){.color = {{0, 0, 0, 0}}};
    bi.renderPass = render_pass;
    bi.framebuffer = target;
    bi.renderArea.extent = (VkExtent2D){32, 32};
    vkCmdBeginRenderPass(command, &bi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, graphics);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 3, sets, 0, NULL);
    VkViewport vp = {0, 0, 32, 32, 0, 1};
    VkRect2D sc = {{0, 0}, {32, 32}};
    vkCmdSetViewport(command, 0, 1, &vp);
    vkCmdSetScissor(command, 0, 1, &sc);
    vkCmdDraw(command, 3, 1, 0, 0);
    vkCmdEndRenderPass(command);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, compute);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 3, sets, 0, NULL);
    vkCmdDispatch(command, 4, 1, 1);
    compute_visibility(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                       VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
    VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                              .imageExtent = {32, 32, 1}};
    vkCmdCopyImageToBuffer(command, out.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, pixels.handle,
                           1, &copy);
    submit();
    int good = 1;
    for (unsigned i = 0; i < 32 * 32 * 4; i++)
        if (((uint8_t *)pixels.map)[i] != 255)
            good = 0;
    EXPECT(
        good,
        "all sixteen samplers execute independently in each graphics stage of a 48-sampler layout");
    good = 1;
    const uint32_t expected[4] = {23120, 19720, 9520, 34680};
    for (unsigned i = 0; i < 4; i++)
        for (unsigned k = 0; k < 4; k++)
            if (((uint32_t *)results.map)[4 + 4 * i + k] != expected[k])
                good = 0;
    EXPECT(good, "all sixteen compute multisample image descriptors fetch GPU texels and "
                 "sample/size queries");
    good = 1;
    for (unsigned i = 0; i < 24; i++)
        if ((i < 4 || i >= 20) && ((uint32_t *)results.map)[i] != 0xa5a5a5a5)
            good = 0;
    EXPECT(good, "compute sampled-image reads and storage writes preserve descriptor guards");
    free_buffer(pixels);
    free_buffer(results);
    vkDestroyPipeline(device, compute, NULL);
    vkDestroyPipeline(device, graphics, NULL);
    for (unsigned i = 0; i < 16; i++) {
        vkDestroyFramebuffer(device, sources[i], NULL);
        free_image(textures[i]);
    }
    vkDestroyFramebuffer(device, target, NULL);
    free_image(out);
    vkDestroyRenderPass(device, render_pass, NULL);
    vkDestroyRenderPass(device, clear_pass, NULL);
    vkDestroySampler(device, sampler, NULL);
    vkDestroyDescriptorPool(device, pool, NULL);
    vkDestroyPipelineLayout(device, layout, NULL);
    for (unsigned i = 0; i < 3; i++)
        vkDestroyDescriptorSetLayout(device, layouts[i], NULL);
}
static void queue_priority_tests(void)
{
    for (unsigned i = 0; i < 4; i++) {
        float priority = i == 0 ? 0.0f : i == 1 ? .49f : i == 2 ? .5f : 1.0f;
        VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                      .queueCount = 1,
                                      .pQueuePriorities = &priority};
        VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                                 .queueCreateInfoCount = 1,
                                 .pQueueCreateInfos = &qi};
        VkDevice d;
        VK(vkCreateDevice(physical, &di, NULL, &d));
        VkQueue q;
        vkGetDeviceQueue(d, 0, 0, &q);
        VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VkFence f;
        VK(vkCreateFence(d, &fi, NULL, &f));
        VK(vkQueueSubmit(q, 0, NULL, f));
        VK(vkWaitForFences(d, 1, &f, VK_TRUE, UINT64_MAX));
        EXPECT(vkGetFenceStatus(d, f) == VK_SUCCESS,
               "both discrete queue-priority classes create and execute synchronized submissions");
        vkDestroyFence(d, f, NULL);
        vkDestroyDevice(d, NULL);
    }
}
