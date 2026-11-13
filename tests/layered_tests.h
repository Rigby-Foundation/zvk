/* SPDX-License-Identifier: LGPL-2.1-or-later */
static void layered_framebuffer_test(unsigned layers, unsigned base, unsigned total,
                                     VkSampleCountFlagBits samples)
{
    if (samples != VK_SAMPLE_COUNT_1_BIT &&
        !(properties.limits.framebufferColorSampleCounts & samples))
        return;
    VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                              VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    struct image output = image_levels(4, 4, VK_FORMAT_R8G8B8A8_UNORM, usage, 1, total, 0);
    struct image color = output;
    if (samples != VK_SAMPLE_COUNT_1_BIT)
        color = image_config(4, 4, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, 1,
                             total, 0, samples);
    VkImageViewCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = color.handle,
        .viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, base, total - base}};
    VkImageView views[2];
    VK(vkCreateImageView(device, &vi, NULL, views));
    if (samples != VK_SAMPLE_COUNT_1_BIT) {
        vi.image = output.handle;
        VK(vkCreateImageView(device, &vi, NULL, views + 1));
    }
    VkAttachmentDescription descriptions[2] = {{.format = VK_FORMAT_R8G8B8A8_UNORM,
                                                .samples = samples,
                                                .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                                                .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                                .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                                .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                                .initialLayout = samples == VK_SAMPLE_COUNT_1_BIT
                                                                     ? VK_IMAGE_LAYOUT_GENERAL
                                                                     : VK_IMAGE_LAYOUT_UNDEFINED,
                                                .finalLayout = VK_IMAGE_LAYOUT_GENERAL},
                                               {.format = VK_FORMAT_R8G8B8A8_UNORM,
                                                .samples = VK_SAMPLE_COUNT_1_BIT,
                                                .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                                .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                                .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                                .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                                .initialLayout = VK_IMAGE_LAYOUT_GENERAL,
                                                .finalLayout = VK_IMAGE_LAYOUT_GENERAL}};
    unsigned count = samples == VK_SAMPLE_COUNT_1_BIT ? 1 : 2;
    VkAttachmentReference reference = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference resolve = {1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                .colorAttachmentCount = 1,
                                .pColorAttachments = &reference,
                                .pResolveAttachments = count == 2 ? &resolve : NULL};
    VkRenderPassCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                 .attachmentCount = count,
                                 .pAttachments = descriptions,
                                 .subpassCount = 1,
                                 .pSubpasses = &sub};
    VkRenderPass pass;
    VK(vkCreateRenderPass(device, &ri, NULL, &pass));
    VkFramebufferCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                  .renderPass = pass,
                                  .attachmentCount = count,
                                  .pAttachments = views,
                                  .width = 4,
                                  .height = 4,
                                  .layers = layers};
    VkFramebuffer fb;
    VK(vkCreateFramebuffer(device, &fi, NULL, &fb));
    struct buffer b = buffer(64 * total, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    VkImageMemoryBarrier ib = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = output.handle,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, total}};
    begin();
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
    VkClearColorValue green = {.float32 = {0, 1, 0, 1}};
    vkCmdClearColorImage(command, output.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &green, 1,
                         &ib.subresourceRange);
    ib.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    ib.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
    VkClearValue red = {.color.float32 = {1, 0, 0, 1}};
    VkRenderPassBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                .renderPass = pass,
                                .framebuffer = fb,
                                .renderArea = {{0, 0}, {4, 4}},
                                .clearValueCount = 1,
                                .pClearValues = &red};
    vkCmdBeginRenderPass(command, &bi, VK_SUBPASS_CONTENTS_INLINE);
    VkClearAttachment blue = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                              .colorAttachment = 0,
                              .clearValue.color.float32 = {0, 0, 1, 1}};
    VkClearRect rect = {.rect = {{1, 1}, {2, 2}}, .baseArrayLayer = 1, .layerCount = layers - 2};
    vkCmdClearAttachments(command, 1, &blue, 1, &rect);
    vkCmdEndRenderPass(command);
    ib.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    ib.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
    VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, total},
                              .imageExtent = {4, 4, 1}};
    vkCmdCopyImageToBuffer(command, output.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b.handle,
                           1, &copy);
    compute_visibility(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                       VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
    submit();
    int ok = 1;
    uint32_t *pixels = b.map;
    for (unsigned l = 0; l < total; l++)
        for (unsigned y = 0; y < 4; y++)
            for (unsigned x = 0; x < 4; x++) {
                uint32_t expected = 0xff00ff00;
                if (l >= base && l < base + layers) {
                    expected = 0xff0000ff;
                    if (l > base && l < base + layers - 1 && x >= 1 && x < 3 && y >= 1 && y < 3)
                        expected = 0xffff0000;
                }
                if (pixels[l * 16 + y * 4 + x] != expected)
                    ok = 0;
            }
    char label[160];
    snprintf(label, sizeof label,
             "layered framebuffer: %u layers, base %u, %u samples, load/rect clears and resolve",
             layers, base, (unsigned)samples);
    EXPECT(ok, label);
    free_buffer(b);
    vkDestroyFramebuffer(device, fb, NULL);
    vkDestroyRenderPass(device, pass, NULL);
    for (unsigned i = 0; i < count; i++)
        vkDestroyImageView(device, views[i], NULL);
    if (count == 2)
        free_image(color);
    free_image(output);
}
static void layered_framebuffer_tests(void)
{
    layered_framebuffer_test(256, 0, 256, VK_SAMPLE_COUNT_1_BIT);
    layered_framebuffer_test(4, 2, 8, VK_SAMPLE_COUNT_1_BIT);
    layered_framebuffer_test(4, 1, 6, VK_SAMPLE_COUNT_4_BIT);
}
