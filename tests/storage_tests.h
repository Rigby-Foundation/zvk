/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Native/loader tests for GPU storage images and texel buffers. */
struct compute_case {
    VkDescriptorSetLayout set_layout;
    VkPipelineLayout layout;
    VkDescriptorPool pool;
    VkDescriptorSet set;
    VkPipeline pipeline;
};
static struct compute_case compute_case_counted(const uint32_t *code, size_t size, unsigned count,
                                                const VkDescriptorType *types,
                                                const uint32_t *counts)
{
    struct compute_case c = {0};
    VkDescriptorSetLayoutBinding bindings[16] = {{0}};
    VkDescriptorPoolSize sizes[11] = {{0}};
    unsigned nsizes = 0;
    for (unsigned i = 0; i < count; i++) {
        bindings[i] = (VkDescriptorSetLayoutBinding){i, types[i], counts ? counts[i] : 1,
                                                     VK_SHADER_STAGE_COMPUTE_BIT, NULL};
        unsigned j = 0;
        while (j < nsizes && sizes[j].type != types[i])
            j++;
        if (j == nsizes)
            sizes[nsizes++] = (VkDescriptorPoolSize){types[i], 0};
        sizes[j].descriptorCount += bindings[i].descriptorCount;
    }
    VkDescriptorSetLayoutCreateInfo si = {.sType =
                                              VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                          .bindingCount = count,
                                          .pBindings = bindings};
    VK(vkCreateDescriptorSetLayout(device, &si, NULL, &c.set_layout));
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                     .setLayoutCount = 1,
                                     .pSetLayouts = &c.set_layout};
    VK(vkCreatePipelineLayout(device, &li, NULL, &c.layout));
    VkDescriptorPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                     .maxSets = 1,
                                     .poolSizeCount = nsizes,
                                     .pPoolSizes = sizes};
    VK(vkCreateDescriptorPool(device, &pi, NULL, &c.pool));
    VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                      .descriptorPool = c.pool,
                                      .descriptorSetCount = 1,
                                      .pSetLayouts = &c.set_layout};
    VK(vkAllocateDescriptorSets(device, &ai, &c.set));
    VkShaderModule shader = module(code, size);
    VkComputePipelineCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                  .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                  .module = shader,
                  .pName = "main"},
        .layout = c.layout};
    VK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &ci, NULL, &c.pipeline));
    vkDestroyShaderModule(device, shader, NULL);
    return c;
}
static struct compute_case compute_case(const uint32_t *code, size_t size, unsigned count,
                                        const VkDescriptorType *types)
{
    return compute_case_counted(code, size, count, types, NULL);
}
static void free_compute_case(struct compute_case c)
{
    vkDestroyPipeline(device, c.pipeline, NULL);
    vkDestroyDescriptorPool(device, c.pool, NULL);
    vkDestroyPipelineLayout(device, c.layout, NULL);
    vkDestroyDescriptorSetLayout(device, c.set_layout, NULL);
}
static void compute_visibility(VkPipelineStageFlags src, VkPipelineStageFlags dst,
                               VkAccessFlags reads, VkAccessFlags writes)
{
    VkMemoryBarrier b = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = reads, .dstAccessMask = writes};
    vkCmdPipelineBarrier(command, src, dst, 0, 1, &b, 0, NULL, 0, NULL);
}
static void bind_compute_case(struct compute_case c)
{
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, c.pipeline);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, c.layout, 0, 1, &c.set, 0,
                            NULL);
}
static void storage_image_tests(void)
{
    if (!properties.limits.maxPerStageDescriptorStorageImages) {
        puts("SKIP storage images: transport unavailable");
        return;
    }
    VkFormatProperties props;
    vkGetPhysicalDeviceFormatProperties(physical, VK_FORMAT_R32_UINT, &props);
    EXPECT((props.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_ATOMIC_BIT) != 0,
           "R32_UINT storage image atomic support reported");
    const VkImageUsageFlags usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    struct image output = image(8, 4, VK_FORMAT_R32G32B32A32_SFLOAT, usage);
    struct image input = image(8, 4, VK_FORMAT_R8G8B8A8_UNORM, usage);
    struct image counter = image(1, 1, VK_FORMAT_R32_UINT, usage);
    struct buffer staging =
        buffer(1024, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    struct buffer result = buffer(128, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    uint8_t *bytes = staging.map;
    memset(bytes, 0, 1024);
    for (unsigned i = 0; i < 32; i++) {
        bytes[4 * i] = i * 7;
        bytes[4 * i + 1] = 255 - i * 3;
        bytes[4 * i + 2] = i;
        bytes[4 * i + 3] = 255;
    }
    VkDescriptorType types[] = {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
    struct compute_case c = compute_case(storage_comp, sizeof storage_comp, 4, types);
    VkDescriptorImageInfo images[3] = {{0, output.view, VK_IMAGE_LAYOUT_GENERAL},
                                       {0, input.view, VK_IMAGE_LAYOUT_GENERAL},
                                       {0, counter.view, VK_IMAGE_LAYOUT_GENERAL}};
    VkDescriptorBufferInfo buffer_info = {result.handle, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet writes[4] = {{0}};
    for (unsigned i = 0; i < 4; i++)
        writes[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                           .dstSet = c.set,
                                           .dstBinding = i,
                                           .descriptorCount = 1,
                                           .descriptorType = types[i],
                                           .pImageInfo = i < 3 ? images + i : NULL,
                                           .pBufferInfo = i == 3 ? &buffer_info : NULL};
    vkUpdateDescriptorSets(device, 4, writes, 0, NULL);
    begin();
    barrier(input.handle, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    barrier(counter.handle, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                              .imageExtent = {8, 4, 1}};
    vkCmdCopyBufferToImage(command, staging.handle, input.handle,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    copy.bufferOffset = 128;
    copy.imageExtent = (VkExtent3D){1, 1, 1};
    vkCmdCopyBufferToImage(command, staging.handle, counter.handle,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    barrier(input.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    barrier(counter.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    barrier(output.handle, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    bind_compute_case(c);
    vkCmdDispatch(command, 2, 2, 1);
    barrier(output.handle, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    barrier(counter.handle, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    copy.bufferOffset = 256;
    copy.imageExtent = (VkExtent3D){8, 4, 1};
    vkCmdCopyImageToBuffer(command, output.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           staging.handle, 1, &copy);
    copy.bufferOffset = 800;
    copy.imageExtent = (VkExtent3D){1, 1, 1};
    vkCmdCopyImageToBuffer(command, counter.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           staging.handle, 1, &copy);
    compute_visibility(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_HOST_BIT,
                       VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                       VK_ACCESS_HOST_READ_BIT);
    submit();
    int ok = 1;
    float *pixels = (float *)(bytes + 256);
    for (unsigned i = 0; i < 32; i++)
        if (fabsf(pixels[4 * i] - (i * 7) / 255.0f) > 0.00001f ||
            fabsf(pixels[4 * i + 1] - (255 - i * 3) / 255.0f) > 0.00001f ||
            pixels[4 * i + 2] != (float)i || pixels[4 * i + 3] != 1)
            ok = 0;
    EXPECT(ok, "GPU storage-image RGBA8 reads and RGBA32F writes with imageSize");
    EXPECT(*(uint32_t *)(bytes + 800) == 32,
           "R32_UINT image atomics and image-to-buffer visibility");
    uint32_t seen = 0;
    uint32_t *old = result.map;
    for (unsigned i = 0; i < 32; i++) {
        if (old[i] >= 32)
            ok = 0;
        else
            seen |= 1u << old[i];
    }
    EXPECT(ok && seen == UINT32_MAX,
           "imageAtomicAdd returns distinct old values across workgroups");
    struct image packed = image(8, 4, VK_FORMAT_R8G8B8A8_UNORM, usage);
    struct compute_case conversion =
        compute_case(storage_unorm_comp, sizeof storage_unorm_comp, 2, types);
    images[0] = (VkDescriptorImageInfo){0, output.view, VK_IMAGE_LAYOUT_GENERAL};
    images[1] = (VkDescriptorImageInfo){0, packed.view, VK_IMAGE_LAYOUT_GENERAL};
    for (unsigned i = 0; i < 2; i++) {
        writes[i].dstSet = conversion.set;
        writes[i].pImageInfo = images + i;
    }
    vkUpdateDescriptorSets(device, 2, writes, 0, NULL);
    begin();
    barrier(output.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    barrier(packed.handle, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    bind_compute_case(conversion);
    vkCmdDispatch(command, 2, 2, 1);
    barrier(packed.handle, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    copy.bufferOffset = 256;
    copy.imageExtent = (VkExtent3D){8, 4, 1};
    vkCmdCopyImageToBuffer(command, packed.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           staging.handle, 1, &copy);
    compute_visibility(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                       VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
    submit();
    ok = 1;
    for (unsigned i = 0; i < 32; i++) {
        uint8_t *p = bytes + 256 + 4 * i;
        if (p[0] != i * 7 + 3 || p[1] != 255 - i * 3 || p[2] != ((i % 8) > 2 ? 255 : 0) ||
            p[3] != 128)
            ok = 0;
    }
    EXPECT(ok, "RGBA32F image loads and RGBA8 stores clamp and quantize on the GPU");
    free_compute_case(conversion);
    free_image(packed);
    free_compute_case(c);
    free_buffer(result);
    free_buffer(staging);
    free_image(counter);
    free_image(input);
    free_image(output);
}
static void storage_array_tests(void)
{
    if (!properties.limits.maxPerStageDescriptorStorageImages)
        return;
    struct image im = image_levels(8, 4, VK_FORMAT_R32_UINT,
                                   VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                       VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                   2, 3, 0);
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                .image = im.handle,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY,
                                .format = VK_FORMAT_R32_UINT,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, 1, 2}};
    VkImageView view;
    VK(vkCreateImageView(device, &vi, NULL, &view));
    VkDescriptorType types[] = {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
    const uint32_t counts[] = {2};
    struct compute_case c =
        compute_case_counted(storage_array_comp, sizeof storage_array_comp, 1, types, counts);
    VkDescriptorImageInfo di[2] = {{0, view, VK_IMAGE_LAYOUT_GENERAL},
                                   {0, view, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                  .dstSet = c.set,
                                  .dstBinding = 0,
                                  .descriptorCount = 2,
                                  .descriptorType = types[0],
                                  .pImageInfo = di};
    vkUpdateDescriptorSets(device, 1, &write, 0, NULL);
    struct buffer b =
        buffer(480, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    uint32_t *values = b.map;
    for (unsigned i = 0; i < 120; i++)
        values[i] = 17 * i + 3;
    VkBufferImageCopy regions[6];
    unsigned count = 0, offset = 0;
    for (unsigned mip = 0; mip < 2; mip++)
        for (unsigned layer = 0; layer < 3; layer++) {
            regions[count++] =
                (VkBufferImageCopy){.bufferOffset = offset,
                                    .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, layer, 1},
                                    .imageExtent = {8u >> mip, 4u >> mip, 1}};
            offset += (8u >> mip) * (4u >> mip) * 4;
        }
    VkImageMemoryBarrier ib = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                               .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                               .dstAccessMask =
                                   VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                               .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                               .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .image = im.handle,
                               .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 2, 0, 3}};
    begin();
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
    vkCmdCopyBufferToImage(command, b.handle, im.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 6,
                           regions);
    ib.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    ib.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
    bind_compute_case(c);
    vkCmdDispatch(command, 1, 1, 2);
    ib.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    ib.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
    vkCmdCopyImageToBuffer(command, im.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b.handle, 6,
                           regions);
    compute_visibility(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                       VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
    submit();
    int selected = 1, preserved = 1;
    for (unsigned i = 0; i < 120; i++) {
        unsigned extra = i >= 112 ? 1100 : i >= 104 ? 1000 : 0;
        if (values[i] != 17 * i + 3 + extra) {
            if (extra)
                selected = 0;
            else
                preserved = 0;
        }
    }
    EXPECT(selected, "aliased storage array views select a nonzero mip and base layer");
    EXPECT(preserved, "storage writes preserve other mips and array layers");
    free_buffer(b);
    free_compute_case(c);
    vkDestroyImageView(device, view, NULL);
    free_image(im);
}
static void storage_clear_tests(void)
{
    if (!properties.limits.maxPerStageDescriptorStorageImages)
        return;
    VkFormat formats[] = {VK_FORMAT_R32_SFLOAT,        VK_FORMAT_R32_SINT,
                          VK_FORMAT_R32_UINT,          VK_FORMAT_R32G32B32A32_SFLOAT,
                          VK_FORMAT_R32G32B32A32_SINT, VK_FORMAT_R32G32B32A32_UINT};
    struct image images[6];
    VkClearColorValue colors[6];
    struct buffer b = buffer(1536, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    memset(b.map, 0xa5, b.size);
    for (unsigned i = 0; i < 6; i++) {
        images[i] = image_levels(2, 2, formats[i],
                                 VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                     VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                 1, 2, 0);
        if (i % 3 == 0)
            colors[i] = (VkClearColorValue){.float32 = {0.25f, -2.0f, 3.0f, 1.0f}};
        if (i % 3 == 1)
            colors[i] = (VkClearColorValue){.int32 = {INT32_MIN, -2, 0, INT32_MAX}};
        if (i % 3 == 2)
            colors[i] = (VkClearColorValue){.uint32 = {0x76543210, UINT32_MAX, 1, 0}};
    }
    begin();
    for (unsigned i = 0; i < 6; i++) {
        VkImageMemoryBarrier ib = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                   .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                                   .dstAccessMask =
                                       VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                                   .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                   .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                   .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                   .image = images[i].handle,
                                   .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 2}};
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
        vkCmdClearColorImage(command, images[i].handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             colors + i, 1, &ib.subresourceRange);
        ib.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        ib.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
        VkBufferImageCopy copy[2];
        for (unsigned layer = 0; layer < 2; layer++)
            copy[layer] =
                (VkBufferImageCopy){.bufferOffset = 256 * i + 128 * layer,
                                    .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1},
                                    .imageExtent = {2, 2, 1}};
        vkCmdCopyImageToBuffer(command, images[i].handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               b.handle, 2, copy);
    }
    compute_visibility(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                       VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
    submit();
    for (unsigned i = 0; i < 6; i++) {
        unsigned bpp = i < 3 ? 4 : 16;
        int ok = 1;
        for (unsigned l = 0; l < 2; l++)
            for (unsigned p = 0; p < 4; p++)
                if (memcmp((uint8_t *)b.map + 256 * i + 128 * l + bpp * p, colors[i].uint32, bpp))
                    ok = 0;
        EXPECT(ok,
               "typed numeric image clears preserve float/signed/unsigned texels in every layer");
        free_image(images[i]);
    }
    free_buffer(b);
}
static VkBufferView texel_view(VkBuffer buffer, VkFormat format, VkDeviceSize offset,
                               VkDeviceSize range)
{
    VkBufferViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO,
                                 .buffer = buffer,
                                 .format = format,
                                 .offset = offset,
                                 .range = range};
    VkBufferView view;
    VK(vkCreateBufferView(device, &vi, NULL, &view));
    return view;
}
static void texel_buffer_tests(void)
{
    if (!properties.limits.maxTexelBufferElements) {
        puts("SKIP texel buffers");
        return;
    }
    enum { N = 65536, BYTES = N * 16 };
    struct buffer b = buffer(2 * BYTES + 64, VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT |
                                                 VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT);
    memset(b.map, 0xa5, b.size);
    uint32_t *src = (uint32_t *)((uint8_t *)b.map + 32), *dst = src + N * 4;
    *(int32_t *)b.map = -5;
    for (unsigned i = 0; i < N * 4; i++)
        src[i] = i * 3;
    VkBufferView views[3] = {texel_view(b.handle, VK_FORMAT_R32G32B32A32_UINT, 32, BYTES),
                             texel_view(b.handle, VK_FORMAT_R32G32B32A32_UINT, 32 + BYTES, BYTES),
                             texel_view(b.handle, VK_FORMAT_R32_SINT, 0, 4)};
    VkDescriptorType types[] = {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
                                VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER,
                                VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER};
    struct compute_case c = compute_case(texel_comp, sizeof texel_comp, 3, types);
    VkWriteDescriptorSet writes[3];
    for (unsigned i = 0; i < 3; i++)
        writes[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                           .dstSet = c.set,
                                           .dstBinding = i,
                                           .descriptorType = types[i],
                                           .descriptorCount = 1,
                                           .pTexelBufferView = views + i};
    vkUpdateDescriptorSets(device, 3, writes, 0, NULL);
    begin();
    bind_compute_case(c);
    vkCmdDispatch(command, N / 8 + 1, 1, 1);
    compute_visibility(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                       VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
    submit();
    int ok = 1;
    for (unsigned i = 0; i < N * 4; i++)
        if (src[i] != i * 3 || dst[i] != i * 3 + (i % 4) + 1)
            ok = 0;
    EXPECT(ok, "GPU fetch/store at all 65536 advertised texels with view offsets");
    EXPECT(*(int32_t *)b.map == N - 5,
           "signed storage-texel atomics share allocation with image writes");
    EXPECT(((uint32_t *)b.map)[1] == 0xa5a5a5a5 &&
               ((uint32_t *)b.map)[b.size / 4 - 1] == 0xa5a5a5a5,
           "texel stores preserve bytes outside buffer-view ranges");
    writes[1].pTexelBufferView = views;
    vkUpdateDescriptorSets(device, 1, writes + 1, 0, NULL);
    begin();
    compute_visibility(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_ACCESS_SHADER_WRITE_BIT,
                       VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    bind_compute_case(c);
    vkCmdDispatch(command, N / 8, 1, 1);
    compute_visibility(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                       VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
    submit();
    ok = 1;
    for (unsigned i = 0; i < N * 4; i++)
        if (src[i] != i * 3 + (i % 4) + 1 || dst[i] != src[i])
            ok = 0;
    EXPECT(ok && *(int32_t *)b.map == 2 * N - 5,
           "uniform and writable texel views alias one GPU backing across dispatches");
    free_compute_case(c);
    for (unsigned i = 0; i < 3; i++)
        vkDestroyBufferView(device, views[i], NULL);
    free_buffer(b);

    struct buffer packed = buffer(64, VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT);
    struct buffer unpacked = buffer(64, VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT);
    const uint8_t pixels[16] = {17, 51, 204, 255, 255, 0, 1, 127, 0, 128, 64, 32, 250, 150, 50, 0};
    memcpy((uint8_t *)packed.map + 16, pixels, sizeof pixels);
    VkBufferView bgra = texel_view(packed.handle, VK_FORMAT_B8G8R8A8_UNORM, 16, 16);
    VkBufferView floats = texel_view(unpacked.handle, VK_FORMAT_R32G32B32A32_SFLOAT, 0, 64);
    c = compute_case(texel_bgra_comp, sizeof texel_bgra_comp, 2, types);
    writes[0].dstSet = writes[1].dstSet = c.set;
    writes[0].pTexelBufferView = &bgra;
    writes[1].pTexelBufferView = &floats;
    vkUpdateDescriptorSets(device, 2, writes, 0, NULL);
    begin();
    bind_compute_case(c);
    vkCmdDispatch(command, 1, 1, 1);
    compute_visibility(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                       VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
    submit();
    ok = 1;
    const unsigned swizzle[4] = {2, 1, 0, 3};
    for (unsigned i = 0; i < 16; i++)
        if (fabsf(((float *)unpacked.map)[i] - pixels[4 * (i / 4) + swizzle[i % 4]] / 255.0f) >
            0.000001f)
            ok = 0;
    EXPECT(ok, "BGRA8 uniform texel fetch swizzles and normalizes each channel on the GPU");
    vkDestroyBufferView(device, bgra, NULL);
    vkDestroyBufferView(device, floats, NULL);
    free_compute_case(c);
    free_buffer(packed);
    free_buffer(unpacked);
}
static void compute_binding_limit_tests(void)
{
    if (!properties.limits.maxTexelBufferElements)
        return;
    VkDescriptorType types[14];
    uint32_t counts[14];
    for (unsigned i = 0; i < 14; i++) {
        counts[i] = 1;
        types[i] = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    }
    counts[12] = 15;
    types[12] = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
    types[13] = VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
    struct compute_case c =
        compute_case_counted(many_bindings_comp, sizeof many_bindings_comp, 14, types, counts);
    struct buffer b =
        buffer(64, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT |
                       VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT);
    memset(b.map, 0, 64);
    *(uint32_t *)b.map = 1;
    VkBufferView inputs[15];
    VkBufferView input = texel_view(b.handle, VK_FORMAT_R32_UINT, 0, 4);
    VkBufferView output = texel_view(b.handle, VK_FORMAT_R32_UINT, 16, 4);
    for (unsigned i = 0; i < 15; i++)
        inputs[i] = input;
    VkDescriptorBufferInfo bi = {b.handle, 0, 16};
    VkWriteDescriptorSet writes[14];
    for (unsigned i = 0; i < 14; i++)
        writes[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                           .dstSet = c.set,
                                           .dstBinding = i,
                                           .descriptorType = types[i],
                                           .descriptorCount = counts[i],
                                           .pBufferInfo = i < 12 ? &bi : NULL,
                                           .pTexelBufferView = i == 12   ? inputs
                                                               : i == 13 ? &output
                                                                         : NULL};
    vkUpdateDescriptorSets(device, 14, writes, 0, NULL);
    begin();
    bind_compute_case(c);
    vkCmdDispatch(command, 1, 1, 1);
    compute_visibility(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                       VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
    submit();
    EXPECT(((uint32_t *)b.map)[4] == 27,
           "28 compute descriptors plus metadata execute with static texel arrays and 12 UBOs");
    vkDestroyBufferView(device, input, NULL);
    vkDestroyBufferView(device, output, NULL);
    free_compute_case(c);
    free_buffer(b);
}
static void compute_function_tests(void)
{
    VkQueueFamilyProperties family;
    uint32_t count = 1;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, &family);
    if (!(family.queueFlags & VK_QUEUE_COMPUTE_BIT))
        return;
    VkDescriptorType types[2] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                 VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER};
    uint32_t counts[2] = {2, 2};
    struct compute_case c =
        compute_case_counted(functions_comp_spvasm, sizeof functions_comp_spvasm, 2, types, counts);
    struct buffer b =
        buffer(1024, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    memset(b.map, 0xa5, 1024);
    *(uint32_t *)b.map = 5;
    ((uint32_t *)b.map)[64] = 20;
    VkDescriptorBufferInfo bi[4] = {
        {b.handle, 512, 32}, {b.handle, 768, 32}, {b.handle, 0, 16}, {b.handle, 256, 16}};
    VkWriteDescriptorSet writes[2];
    for (unsigned i = 0; i < 2; i++)
        writes[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                           .dstSet = c.set,
                                           .dstBinding = i,
                                           .descriptorType = types[i],
                                           .descriptorCount = 2,
                                           .pBufferInfo = bi + 2 * i};
    vkUpdateDescriptorSets(device, 2, writes, 0, NULL);
    begin();
    bind_compute_case(c);
    vkCmdDispatch(command, 2, 1, 1);
    compute_visibility(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                       VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
    submit();
    int ok = 1;
    for (unsigned i = 0; i < 8; i++)
        if (((uint32_t *)b.map)[128 + i] != 7 || ((uint32_t *)b.map)[192 + i] != 26)
            ok = 0;
    EXPECT(ok, "nested/repeated value function calls preserve private side effects and buffer "
               "descriptor arrays");
    EXPECT(((uint32_t *)b.map)[136] == 0xa5a5a5a5 && ((uint32_t *)b.map)[200] == 0xa5a5a5a5,
           "divergent memory-only fences complete without an execution rendezvous or stray writes");
    free_compute_case(c);
    free_buffer(b);
}
static void compute_aggregate_math_tests(void)
{
    VkQueueFamilyProperties family;uint32_t count=1;
    vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,&family);
    if (!(family.queueFlags&VK_QUEUE_COMPUTE_BIT))return;
    VkDescriptorType type=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    struct compute_case c=compute_case(aggregate_math_comp,sizeof aggregate_math_comp,1,&type);
    struct buffer b=buffer(256,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    memset(b.map,0xa5,256);
    VkDescriptorBufferInfo info={b.handle,16,14*4};
    VkWriteDescriptorSet write={.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=c.set,
        .descriptorType=type,.descriptorCount=1,.pBufferInfo=&info};
    vkUpdateDescriptorSets(device,1,&write,0,NULL);
    begin();bind_compute_case(c);vkCmdDispatch(command,1,1,1);
    compute_visibility(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,
                       VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_HOST_READ_BIT);
    submit();
    const uint32_t expected[]={28,38,48,30,2,0xfffffffau,2,1,0xffffffffu,1,
                              0x40600000,0xbe800000,0xc0000000,0x3e800000};
    EXPECT(!memcmp((uint8_t *)b.map+16,expected,sizeof expected),
           "compute aggregate and pointer calls, non-square matrices, extended integers and specialization expressions");
    EXPECT(*(uint32_t *)b.map==0xa5a5a5a5 && ((uint32_t *)b.map)[18]==0xa5a5a5a5,
           "aggregate compute writes preserve descriptor guards");
    free_compute_case(c);free_buffer(b);
}
