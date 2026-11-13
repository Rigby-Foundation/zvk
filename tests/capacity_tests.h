/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Exercise every advertised compute resource class simultaneously, with
 * allocation aliases and descriptor guards across the packed transport. */
static void capacity_barrier(VkImage image, unsigned levels, VkImageLayout old, VkImageLayout next)
{
    VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                              .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                              .dstAccessMask =
                                  VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                              .oldLayout = old,
                              .newLayout = next,
                              .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                              .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                              .image = image,
                              .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1}};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}
static void capacity_case(unsigned unaligned)
{
    if (!properties.limits.maxTexelBufferElements)
        return;
    VkDescriptorType types[] = {
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
    uint32_t counts[] = {12, 8, 16, 4};
    struct compute_case c = compute_case_counted(
        unaligned ? capacity_unaligned_comp : capacity_comp,
        unaligned ? sizeof capacity_unaligned_comp : sizeof capacity_comp, 4, types, counts);
    struct compute_case direct = {0};
    if (unaligned) {
        VkDescriptorType type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        uint32_t count = 4;
        direct = compute_case_counted(capacity_direct_unaligned_comp,
                                      sizeof capacity_direct_unaligned_comp, 1, &type, &count);
    }
    struct buffer ubos[12], stores[7], texels[16];
    VkDescriptorBufferInfo ui[12], ss[8];
    VkBufferView views[16];
    VkDescriptorImageInfo images[4];
    struct image image_data[4];
    for (unsigned i = 0; i < 12; i++) {
        ubos[i] = buffer(16, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
        *(uint32_t *)ubos[i].map = i + 1;
        ui[i] = (VkDescriptorBufferInfo){ubos[i].handle, 0, 16};
    }
    for (unsigned i = 0; i < 7; i++) {
        stores[i] = buffer(64, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        memset(stores[i].map, 0xa5, 64);
        ((uint32_t *)stores[i].map)[4] = 20 + i;
        ss[i] = (VkDescriptorBufferInfo){stores[i].handle, 16, 8};
    }
    ss[7] = ss[6];
    for (unsigned i = 0; i < 16; i++) {
        texels[i] = buffer(64, VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT);
        ((uint32_t *)texels[i].map)[4] = 40 + i;
        views[i] = texel_view(texels[i].handle, VK_FORMAT_R32_UINT, 16, 4);
    }
    begin();
    for (unsigned i = 0; i < 4; i++) {
        VkFormat format = unaligned ? VK_FORMAT_R8_UNORM : VK_FORMAT_R32_UINT;
        image_data[i] = image_levels(unaligned ? 3 : 1, unaligned ? 3 : 1, format,
                                     VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                         VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                     unaligned ? 2 : 1, 1, 0);
        if (unaligned) {
            vkDestroyImageView(device, image_data[i].view, NULL);
            VkImageViewCreateInfo vi = {
                .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .image = image_data[i].handle,
                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                .format = format,
                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, 0, 1}};
            VK(vkCreateImageView(device, &vi, NULL, &image_data[i].view));
        }
        capacity_barrier(image_data[i].handle, unaligned ? 2 : 1, VK_IMAGE_LAYOUT_UNDEFINED,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkClearColorValue value = {.uint32 = {100 + i, 0, 0, 0}};
        if (unaligned)
            value.float32[0] = (float)(100 + i) / 255;
        VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, unaligned ? 2 : 1, 0, 1};
        vkCmdClearColorImage(command, image_data[i].handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             &value, 1, &range);
        capacity_barrier(image_data[i].handle, unaligned ? 2 : 1,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
        images[i] = (VkDescriptorImageInfo){0, image_data[i].view, VK_IMAGE_LAYOUT_GENERAL};
    }
    VkWriteDescriptorSet writes[4];
    for (unsigned i = 0; i < 4; i++)
        writes[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                           .dstSet = c.set,
                                           .dstBinding = i,
                                           .descriptorCount = counts[i],
                                           .descriptorType = types[i],
                                           .pBufferInfo = i == 0   ? ui
                                                          : i == 1 ? ss
                                                                   : NULL,
                                           .pTexelBufferView = i == 2 ? views : NULL,
                                           .pImageInfo = i == 3 ? images : NULL};
    vkUpdateDescriptorSets(device, 4, writes, 0, NULL);
    if (unaligned) {
        VkWriteDescriptorSet write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                      .dstSet = direct.set,
                                      .dstBinding = 0,
                                      .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                      .descriptorCount = 4,
                                      .pImageInfo = images};
        vkUpdateDescriptorSets(device, 1, &write, 0, NULL);
        bind_compute_case(direct);
        vkCmdDispatch(command, 1, 1, 1);
        compute_visibility(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                           VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    }
    bind_compute_case(c);
    vkCmdDispatch(command, 1, 1, 1);
    compute_visibility(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_ACCESS_SHADER_WRITE_BIT,
                       VK_ACCESS_HOST_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT);
    struct buffer pixels = buffer(64, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    for (unsigned i = 0; i < 4; i++) {
        capacity_barrier(image_data[i].handle, unaligned ? 2 : 1, VK_IMAGE_LAYOUT_GENERAL,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy copy = {
            .bufferOffset = 4 * i,
            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, unaligned ? 1 : 0, 0, 1},
            .imageExtent = {1, 1, 1}};
        vkCmdCopyImageToBuffer(command, image_data[i].handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               pixels.handle, 1, &copy);
        if (unaligned) {
            copy.bufferOffset = 16 + i * 12;
            copy.imageSubresource.mipLevel = 0;
            copy.imageExtent = (VkExtent3D){3, 3, 1};
            vkCmdCopyImageToBuffer(command, image_data[i].handle,
                                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, pixels.handle, 1, &copy);
        }
    }
    submit();
    unsigned sum = 78 + 187 + 760 + 406 + (unaligned ? 4 : 0);
    int safe = 1;
    for (unsigned i = 0; i < 7; i++)
        for (unsigned word = 0; word < 16; word++) {
            uint32_t expected = word == 4   ? 20 + i
                                : word == 5 ? sum + (i == 6 ? 7 : i)
                                            : 0xa5a5a5a5;
            if (((uint32_t *)stores[i].map)[word] != expected)
                safe = 0;
        }
    EXPECT(safe, "40 simultaneous GPU descriptors retain all UBO/SSBO/texel inputs, guards and "
                 "aliased atomics");
    safe = 1;
    for (unsigned i = 0; i < 4; i++) {
        uint32_t actual = unaligned ? ((uint8_t *)pixels.map)[4 * i] : ((uint32_t *)pixels.map)[i];
        if (actual != (unaligned ? (sum + 200 + i) & 255u : sum + 200 + i))
            safe = 0;
        if (unaligned)
            for (unsigned pixel = 0; pixel < 9; pixel++)
                if (((uint8_t *)pixels.map)[16 + i * 12 + pixel] != 100 + i)
                    safe = 0;
    }
    EXPECT(safe,
           unaligned
               ? "direct and packed R8 mip views preserve byte-aligned data and neighboring mips"
               : "packed compute resources copy storage-image writes back into GPU textures");
    free_buffer(pixels);
    if (unaligned)
        free_compute_case(direct);
    free_compute_case(c);
    for (unsigned i = 0; i < 4; i++)
        free_image(image_data[i]);
    for (unsigned i = 0; i < 16; i++) {
        vkDestroyBufferView(device, views[i], NULL);
        free_buffer(texels[i]);
    }
    for (unsigned i = 0; i < 7; i++)
        free_buffer(stores[i]);
    for (unsigned i = 0; i < 12; i++)
        free_buffer(ubos[i]);
}

static void capacity_tests(void)
{
    capacity_case(0);
    capacity_case(1);
}
