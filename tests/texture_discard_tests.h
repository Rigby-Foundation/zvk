/* SPDX-License-Identifier: LGPL-2.1-or-later */
static void texture_discard_tests(VkRenderPass pass, VkFramebuffer fb, VkPipelineLayout layout,
                                  VkDescriptorSet set, struct image output, struct buffer result)
{
    struct image source =
        image_levels(256, 256, VK_FORMAT_R8G8B8A8_UNORM,
                     VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 9, 1, 0);
    struct buffer upload = buffer(350000, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    VkBufferImageCopy copies[9];
    unsigned offset = 0;
    for (unsigned mip = 0; mip < 9; mip++) {
        unsigned side = 256u >> mip;
        for (unsigned y = 0; y < side; y++)
            for (unsigned x = 0; x < side; x++) {
                uint8_t *pixel = (uint8_t *)upload.map + offset + 4 * (y * side + x);
                pixel[0] = ((x < side / 2) != (y < side / 2)) ? 255 : 0;
                pixel[1] = pixel[2] = 0;
                pixel[3] = 255;
            }
        copies[mip] =
            (VkBufferImageCopy){.bufferOffset = offset,
                                .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, 1},
                                .imageExtent = {side, side, 1}};
        offset += side * side * 4;
    }
    begin();
    dimension_barrier(source, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                      9);
    vkCmdCopyBufferToImage(command, upload.handle, source.handle,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 9, copies);
    dimension_barrier(source, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 1, 9);
    submit();
    VkSamplerCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                               .magFilter = VK_FILTER_LINEAR,
                               .minFilter = VK_FILTER_LINEAR,
                               .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
                               .maxLod = .25f,
                               .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                               .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                               .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    VkSampler sampler;
    VK(vkCreateSampler(device, &sci, NULL, &sampler));
    VkPipeline render =
        pipeline_shaders(builtin_vert, sizeof builtin_vert, NULL, pass, layout,
                         texture_discard_frag, sizeof texture_discard_frag, 0, 0, 0);
    begin();
    sample_format_image(source.view, pass, fb, render, layout, set, sampler, output, result);
    const float *values = result.map;
    int good = 1;
    for (unsigned y = 12; y < 20; y++)
        for (unsigned x = 12; x < 20; x++)
            for (unsigned c = 0; c < 4; c++) {
                float expected = ((x < 16) != (y < 16)) && (c == 1 || c == 3) ? 1 : 0;
                if (values[(y * 32 + x) * 4 + c] != expected)
                    good = 0;
            }
    EXPECT(good, "texture-controlled discard preserves cleared pixels and shaded survivors");
    vkDestroyPipeline(device, render, NULL);
    vkDestroySampler(device, sampler, NULL);
    free_buffer(upload);
    free_image(source);
}
