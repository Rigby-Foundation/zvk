/* SPDX-License-Identifier: LGPL-2.1-or-later */
static struct image dimension_image_format(VkImageType type, VkExtent3D extent, unsigned layers,
                                           unsigned levels, VkFormat format)
{
    struct image image = {0};
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                            .imageType = type,
                            .extent = extent,
                            .mipLevels = levels,
                            .arrayLayers = layers,
                            .samples = VK_SAMPLE_COUNT_1_BIT,
                            .format = format,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                     VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                     (properties.limits.maxPerStageDescriptorStorageImages
                                          ? VK_IMAGE_USAGE_STORAGE_BIT
                                          : 0)};
    VK(vkCreateImage(device, &ci, NULL, &image.handle));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device, image.handle, &req);
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                               .allocationSize = req.size};
    VK(vkAllocateMemory(device, &ai, NULL, &image.memory));
    VK(vkBindImageMemory(device, image.handle, image.memory, 0));
    VkImageViewCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = image.handle,
        .viewType = type == VK_IMAGE_TYPE_3D ? VK_IMAGE_VIEW_TYPE_3D
                    : layers > 1             ? VK_IMAGE_VIEW_TYPE_1D_ARRAY
                                             : VK_IMAGE_VIEW_TYPE_1D,
        .format = ci.format,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, layers}};
    VK(vkCreateImageView(device, &vi, NULL, &image.view));
    return image;
}
static struct image dimension_image(VkImageType type, VkExtent3D extent, unsigned layers,
                                    unsigned levels)
{
    return dimension_image_format(type, extent, layers, levels, VK_FORMAT_R32_UINT);
}
static void dimension_barrier(struct image image, VkImageLayout old, VkImageLayout next,
                              unsigned layers, unsigned levels)
{
    VkImageMemoryBarrier b = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .oldLayout = old,
        .newLayout = next,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        .image = image.handle,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, layers}};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}
static int dimension_pixel(struct buffer result, float value)
{
    float *p = (float *)result.map + 4 * (16 * 32 + 16);
    if (fabsf(p[0] - value) > .01f || fabsf(p[1]) > .001f || fabsf(p[2]) > .001f ||
        fabsf(p[3] - 1) > .001f) {
        printf("DIMENSION PIXEL %.7g %.7g %.7g %.7g wanted %.7g\n", p[0], p[1], p[2], p[3], value);
        return 0;
    }
    return 1;
}
static void dimension_tests(VkRenderPass pass, VkFramebuffer fb, VkPipelineLayout layout,
                            VkDescriptorSet set, VkSampler sampler, struct image output,
                            struct buffer result)
{
    const uint32_t *fragments[3] = {texture_1d_frag, texture_1d_array_frag, texture_3d_frag};
    const size_t sizes[3] = {sizeof texture_1d_frag, sizeof texture_1d_array_frag,
                             sizeof texture_3d_frag};
    VkPipeline pipelines[3];
    for (unsigned i = 0; i < 3; i++)
        pipelines[i] = pipeline_shaders(builtin_vert, sizeof builtin_vert, NULL, pass, layout,
                                        fragments[i], sizes[i], 0, 0, 0);
    for (unsigned volume = 0; volume < 2; volume++) {
        VkImageType type = volume ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_1D;
        VkExtent3D extent = volume ? (VkExtent3D){8, 4, 3} : (VkExtent3D){8, 1, 1};
        unsigned layers = volume ? 1 : 3, height = extent.height;
        unsigned pitch = 12 * 4, slice = pitch * (volume ? 6 : 4);
        struct image source = dimension_image(type, extent, layers, 3);
        struct buffer data =
            buffer(2048, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        memset(data.map, 0xa5, 2048);
        for (unsigned z = 0; z < 3; z++)
            for (unsigned y = 0; y < height; y++)
                for (unsigned x = 0; x < 8; x++)
                    *(uint32_t *)((uint8_t *)data.map + 16 + z * slice + y * pitch + x * 4) =
                        100 * z + 10 * y + x;
        VkBufferImageCopy transfer = {.bufferOffset = 16,
                                      .bufferRowLength = 12,
                                      .bufferImageHeight = volume ? 6 : 4,
                                      .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, layers},
                                      .imageExtent = extent};
        begin();
        dimension_barrier(source, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          layers, 3);
        vkCmdCopyBufferToImage(command, data.handle, source.handle,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &transfer);
        dimension_barrier(source, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, layers, 3);
        transfer.bufferOffset = 1024;
        vkCmdCopyImageToBuffer(command, source.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               data.handle, 1, &transfer);
        dimension_barrier(source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, layers, 3);
        sample_format_image(source.view, pass, fb, pipelines[volume ? 2 : 1], layout, set, sampler,
                            output, result);
        int ok = 1;
        for (unsigned z = 0; z < 3; z++)
            for (unsigned y = 0; y < height; y++) {
                uint8_t *row = (uint8_t *)data.map + 1024 + z * slice + y * pitch;
                for (unsigned x = 0; x < 8; x++)
                    if (((uint32_t *)row)[x] != 100 * z + 10 * y + x)
                        ok = 0;
                for (unsigned x = 8 * 4; x < pitch; x++)
                    if (row[x] != 0xa5)
                        ok = 0;
            }
        EXPECT(ok, "1D array / 3D volume transfers preserve row, slice and layer pitches");
        EXPECT(dimension_pixel(result, volume ? 212 : 202),
               "GPU 1D array / 3D sampling selects the expected layer or Z coordinate");

        VkExtent3D copy_extent = volume ? (VkExtent3D){4, 2, 2} : (VkExtent3D){4, 1, 1};
        struct image copied = dimension_image(type, copy_extent, 1, 1);
        VkImageCopy copy = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, volume ? 0 : 1, 1},
                            .srcOffset = {2, volume ? 1 : 0, volume ? 1 : 0},
                            .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                            .extent = copy_extent};
        begin();
        dimension_barrier(source, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, layers, 3);
        dimension_barrier(copied, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          1, 1);
        vkCmdCopyImage(command, source.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, copied.handle,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        dimension_barrier(copied, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 1, 1);
        sample_format_image(copied.view, pass, fb, pipelines[volume ? 2 : 0], layout, set, sampler,
                            output, result);
        EXPECT(dimension_pixel(result, volume ? 213 : 103),
               "1D / 3D image subrectangle copies preserve nonzero layer/Z origins");
        if (volume) {
            struct image blitted = dimension_image(type, (VkExtent3D){4, 2, 3}, 1, 1);
            VkImageBlit blit = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                .srcOffsets = {{0, 0, 3}, {8, 4, 0}},
                                .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                .dstOffsets = {{0, 0, 0}, {4, 2, 3}}};
            begin();
            dimension_barrier(blitted, VK_IMAGE_LAYOUT_UNDEFINED,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, 1);
            vkCmdBlitImage(command, source.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           blitted.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                           VK_FILTER_NEAREST);
            dimension_barrier(blitted, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 1, 1);
            sample_format_image(blitted.view, pass, fb, pipelines[2], layout, set, sampler, output,
                                result);
            EXPECT(dimension_pixel(result, 13),
                   "GPU 3D nearest blit scales XY and reverses the Z axis");
            free_image(blitted);
        }
        free_buffer(data);
        free_image(copied);
        free_image(source);
    }

    struct image mip_image = dimension_image(VK_IMAGE_TYPE_3D, (VkExtent3D){4, 4, 4}, 1, 3);
    struct buffer mip_data = buffer(32, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    const uint32_t mip_values[8] = {0, 1, 10, 11, 100, 101, 110, 111};
    memcpy(mip_data.map, mip_values, 32);
    VkBufferImageCopy mip_copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 1, 0, 1},
                                  .imageExtent = {2, 2, 2}};
    VkImageViewCreateInfo mip_vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                    .image = mip_image.handle,
                                    .viewType = VK_IMAGE_VIEW_TYPE_3D,
                                    .format = VK_FORMAT_R32_UINT,
                                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, 0, 1}};
    VkImageView mip_view;
    VK(vkCreateImageView(device, &mip_vi, NULL, &mip_view));
    begin();
    dimension_barrier(mip_image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                      3);
    vkCmdCopyBufferToImage(command, mip_data.handle, mip_image.handle,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &mip_copy);
    dimension_barrier(mip_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 1, 3);
    sample_format_image(mip_view, pass, fb, pipelines[2], layout, set, sampler, output, result);
    EXPECT(dimension_pixel(result, 100),
           "restricted 3D mip views preserve all depth slices in their GPU snapshot");
    vkDestroyImageView(device, mip_view, NULL);
    free_buffer(mip_data);
    free_image(mip_image);
    struct image linear_src =
        dimension_image_format(VK_IMAGE_TYPE_3D, (VkExtent3D){2, 2, 2}, 1, 2, VK_FORMAT_R32_SFLOAT);
    struct image linear_dst =
        dimension_image_format(VK_IMAGE_TYPE_3D, (VkExtent3D){1, 1, 3}, 1, 1, VK_FORMAT_R32_SFLOAT);
    struct buffer floats =
        buffer(128, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    float *values = floats.map;
    for (unsigned z = 0; z < 2; z++)
        for (unsigned y = 0; y < 2; y++)
            for (unsigned x = 0; x < 2; x++)
                values[z * 4 + y * 2 + x] = 10 * z + 2 * y + x;
    VkBufferImageCopy float_copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                    .imageExtent = {2, 2, 2}};
    VkImageBlit linear = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                          .srcOffsets = {{0, 0, 0}, {2, 2, 2}},
                          .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                          .dstOffsets = {{0, 0, 0}, {1, 1, 3}}};
    begin();
    dimension_barrier(linear_src, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      1, 2);
    vkCmdCopyBufferToImage(command, floats.handle, linear_src.handle,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &float_copy);
    dimension_barrier(linear_src, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 1, 2);
    dimension_barrier(linear_dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      1, 1);
    VkBufferImageCopy debug_copy = float_copy;
    debug_copy.bufferOffset = 32;
    vkCmdCopyImageToBuffer(command, linear_src.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           floats.handle, 1, &debug_copy);
    vkCmdBlitImage(command, linear_src.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   linear_dst.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &linear,
                   VK_FILTER_LINEAR);
    dimension_barrier(linear_dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 1, 1);
    float_copy.bufferOffset = 64;
    float_copy.imageExtent = (VkExtent3D){1, 1, 3};
    vkCmdCopyImageToBuffer(command, linear_dst.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           floats.handle, 1, &float_copy);
    submit();
    int source_ok = 1;
    for (unsigned i = 0; i < 8; i++)
        if (values[8 + i] != values[i])
            source_ok = 0;
    EXPECT(source_ok, "3D float uploads and readback retain all source slices");
    EXPECT(fabsf(values[16] - 1.5f) < .001f && fabsf(values[17] - 6.5f) < .001f &&
               fabsf(values[18] - 11.5f) < .001f,
           "GPU 3D linear blits interpolate all three axes and clamp source edges");
    begin();
    dimension_barrier(linear_src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, 1,
                      2);
    linear.dstSubresource.mipLevel = 1;
    linear.dstOffsets[1] = (VkOffset3D){1, 1, 1};
    vkCmdBlitImage(command, linear_src.handle, VK_IMAGE_LAYOUT_GENERAL, linear_src.handle,
                   VK_IMAGE_LAYOUT_GENERAL, 1, &linear, VK_FILTER_LINEAR);
    dimension_barrier(linear_src, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 1,
                      2);
    float_copy.bufferOffset = 96;
    float_copy.imageSubresource.mipLevel = 1;
    float_copy.imageExtent = (VkExtent3D){1, 1, 1};
    vkCmdCopyImageToBuffer(command, linear_src.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           floats.handle, 1, &float_copy);
    submit();
    EXPECT(fabsf(values[24] - 6.5f) < .001f,
           "3D self-image mip blits preserve the source with a GPU snapshot");
    free_buffer(floats);
    free_image(linear_src);
    free_image(linear_dst);
    if (properties.limits.maxPerStageDescriptorStorageImages) {
        const VkDescriptorType type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        for (unsigned volume = 0; volume < 2; volume++) {
            VkExtent3D extent = volume ? (VkExtent3D){256, 256, 256} : (VkExtent3D){4096, 1, 1};
            unsigned layers = volume ? 1 : 256;
            struct image image =
                dimension_image(volume ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_1D, extent, layers, 1);
            struct compute_case compute = compute_case(
                volume ? dimensions_3d_comp : dimensions_1d_comp,
                volume ? sizeof dimensions_3d_comp : sizeof dimensions_1d_comp, 1, &type);
            VkDescriptorImageInfo ii = {0, image.view, VK_IMAGE_LAYOUT_GENERAL};
            VkWriteDescriptorSet write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                          .dstSet = compute.set,
                                          .dstBinding = 0,
                                          .descriptorType = type,
                                          .descriptorCount = 1,
                                          .pImageInfo = &ii};
            vkUpdateDescriptorSets(device, 1, &write, 0, NULL);
            begin();
            dimension_barrier(image, VK_IMAGE_LAYOUT_UNDEFINED,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, layers, 1);
            VkClearColorValue zero = {.uint32 = {0, 0, 0, 0}};
            VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
            vkCmdClearColorImage(command, image.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero,
                                 1, &range);
            dimension_barrier(image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                              layers, 1);
            bind_compute_case(compute);
            vkCmdDispatch(command, extent.width / 8, volume ? extent.height / 4 : layers / 4,
                          volume ? extent.depth / 4 : 1);
            dimension_barrier(image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                              layers, 1);
            VkDeviceSize bytes =
                (VkDeviceSize)extent.width * extent.height * extent.depth * layers * 4;
            struct buffer results = buffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
            VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, layers},
                                      .imageExtent = extent};
            vkCmdCopyImageToBuffer(command, image.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   results.handle, 1, &copy);
            submit();
            int ok = 1;
            for (unsigned z = 0; z < (volume ? extent.depth : layers); z++)
                for (unsigned y = 0; y < extent.height; y++)
                    for (unsigned x = 0; x < extent.width; x++) {
                        uint32_t expected = volume ? x + 11 * y + 101 * z + 17 : x + 100 * z + 17;
                        if (((uint32_t *)
                                 results.map)[((size_t)z * extent.height + y) * extent.width + x] !=
                            expected)
                            ok = 0;
                    }
            EXPECT(ok,
                   volume ? "full 256-cubed storage image obeys imageSize and preserves every voxel"
                          : "4096-wide 256-layer 1D storage image obeys imageSize and preserves "
                            "every texel");
            free_buffer(results);
            free_compute_case(compute);
            free_image(image);
        }
    }
    if (properties.limits.maxPerStageDescriptorStorageImages) {
        struct image cube = image_levels(8, 8, VK_FORMAT_R32_UINT,
                                         VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                             VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                         1, 6, 1);
        const VkDescriptorType type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        struct compute_case compute =
            compute_case(dimensions_cube_comp, sizeof dimensions_cube_comp, 1, &type);
        VkDescriptorImageInfo ii = {0, cube.view, VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                      .dstSet = compute.set,
                                      .dstBinding = 0,
                                      .descriptorType = type,
                                      .descriptorCount = 1,
                                      .pImageInfo = &ii};
        vkUpdateDescriptorSets(device, 1, &write, 0, NULL);
        struct buffer results = buffer(8 * 8 * 6 * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        begin();
        dimension_barrier(cube, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 6, 1);
        bind_compute_case(compute);
        vkCmdDispatch(command, 1, 2, 6);
        dimension_barrier(cube, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 6,
                          1);
        VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 6},
                                  .imageExtent = {8, 8, 1}};
        vkCmdCopyImageToBuffer(command, cube.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               results.handle, 1, &copy);
        submit();
        int ok = 1;
        for (unsigned face = 0; face < 6; face++)
            for (unsigned y = 0; y < 8; y++)
                for (unsigned x = 0; x < 8; x++)
                    if (((uint32_t *)results.map)[face * 64 + y * 8 + x] !=
                        x + 11 * y + 101 * face + 17)
                        ok = 0;
        EXPECT(ok, "cube storage-image queries and GPU writes preserve all six faces");
        free_buffer(results);
        free_compute_case(compute);
        free_image(cube);
    }
    for (unsigned i = 0; i < 3; i++)
        vkDestroyPipeline(device, pipelines[i], NULL);
}
