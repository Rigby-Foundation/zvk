/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Independent GPU checks of format conversion, sampling and transfers. */
static const struct {
    VkFormat format;
    unsigned kind, components;
} tested_formats[] = {
    {VK_FORMAT_R8_UNORM, 0, 1},
    {VK_FORMAT_R8_SNORM, 0, 1},
    {VK_FORMAT_R8_SINT, 2, 1},
    {VK_FORMAT_R8_UINT, 1, 1},
    {VK_FORMAT_R8_SRGB, 0, 1},
    {VK_FORMAT_R8G8_UNORM, 0, 2},
    {VK_FORMAT_R8G8_SNORM, 0, 2},
    {VK_FORMAT_R8G8_SINT, 2, 2},
    {VK_FORMAT_R8G8_UINT, 1, 2},
    {VK_FORMAT_R8G8_SRGB, 0, 2},
    {VK_FORMAT_R8G8B8_UNORM, 0, 3},
    {VK_FORMAT_R8G8B8_SNORM, 0, 3},
    {VK_FORMAT_R8G8B8_SINT, 2, 3},
    {VK_FORMAT_R8G8B8_UINT, 1, 3},
    {VK_FORMAT_R8G8B8_SRGB, 0, 3},
    {VK_FORMAT_R8G8B8A8_UNORM, 0, 4},
    {VK_FORMAT_R8G8B8A8_SNORM, 0, 4},
    {VK_FORMAT_R8G8B8A8_SINT, 2, 4},
    {VK_FORMAT_R8G8B8A8_UINT, 1, 4},
    {VK_FORMAT_R8G8B8A8_SRGB, 0, 4},
    {VK_FORMAT_R16_UNORM, 0, 1},
    {VK_FORMAT_R16_SNORM, 0, 1},
    {VK_FORMAT_R16_SINT, 2, 1},
    {VK_FORMAT_R16_UINT, 1, 1},
    {VK_FORMAT_R16_SFLOAT, 0, 1},
    {VK_FORMAT_R16G16_UNORM, 0, 2},
    {VK_FORMAT_R16G16_SNORM, 0, 2},
    {VK_FORMAT_R16G16_SINT, 2, 2},
    {VK_FORMAT_R16G16_UINT, 1, 2},
    {VK_FORMAT_R16G16_SFLOAT, 0, 2},
    {VK_FORMAT_R16G16B16_UNORM, 0, 3},
    {VK_FORMAT_R16G16B16_SNORM, 0, 3},
    {VK_FORMAT_R16G16B16_SINT, 2, 3},
    {VK_FORMAT_R16G16B16_UINT, 1, 3},
    {VK_FORMAT_R16G16B16_SFLOAT, 0, 3},
    {VK_FORMAT_R16G16B16A16_UNORM, 0, 4},
    {VK_FORMAT_R16G16B16A16_SNORM, 0, 4},
    {VK_FORMAT_R16G16B16A16_SINT, 2, 4},
    {VK_FORMAT_R16G16B16A16_UINT, 1, 4},
    {VK_FORMAT_R16G16B16A16_SFLOAT, 0, 4},
    {VK_FORMAT_R32_SFLOAT, 0, 1},
    {VK_FORMAT_R32_SINT, 2, 1},
    {VK_FORMAT_R32_UINT, 1, 1},
    {VK_FORMAT_R32G32_SFLOAT, 0, 2},
    {VK_FORMAT_R32G32_SINT, 2, 2},
    {VK_FORMAT_R32G32_UINT, 1, 2},
    {VK_FORMAT_R32G32B32_SFLOAT, 0, 3},
    {VK_FORMAT_R32G32B32_SINT, 2, 3},
    {VK_FORMAT_R32G32B32_UINT, 1, 3},
    {VK_FORMAT_R32G32B32A32_SFLOAT, 0, 4},
    {VK_FORMAT_R32G32B32A32_SINT, 2, 4},
    {VK_FORMAT_R32G32B32A32_UINT, 1, 4},
    {VK_FORMAT_A8B8G8R8_UNORM_PACK32, 0, 4},
    {VK_FORMAT_A8B8G8R8_SNORM_PACK32, 0, 4},
    {VK_FORMAT_A8B8G8R8_SINT_PACK32, 2, 4},
    {VK_FORMAT_A8B8G8R8_UINT_PACK32, 1, 4},
    {VK_FORMAT_A8B8G8R8_SRGB_PACK32, 0, 4},
    {VK_FORMAT_B8G8R8A8_UNORM, 0, 4},
    {VK_FORMAT_B8G8R8A8_SRGB, 0, 4},
    {VK_FORMAT_R5G6B5_UNORM_PACK16, 0, 3},
    {VK_FORMAT_A1R5G5B5_UNORM_PACK16, 0, 4},
    {VK_FORMAT_B4G4R4A4_UNORM_PACK16, 0, 4},
    {VK_FORMAT_A2B10G10R10_UNORM_PACK32, 0, 4},
    {VK_FORMAT_A2B10G10R10_UINT_PACK32, 1, 4},
    {VK_FORMAT_B10G11R11_UFLOAT_PACK32, 0, 3},
    {VK_FORMAT_E5B9G9R9_UFLOAT_PACK32, 0, 3},
};
static void sample_format_image(VkImageView view, VkRenderPass pass, VkFramebuffer fb,
                                VkPipeline pipeline, VkPipelineLayout layout, VkDescriptorSet set,
                                VkSampler sampler, struct image output, struct buffer result)
{
    VkClearValue zero = {0};
    VkRenderPassBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                .renderPass = pass,
                                .framebuffer = fb,
                                .renderArea = {{0, 0}, {32, 32}},
                                .clearValueCount = 1,
                                .pClearValues = &zero};
    VkDescriptorImageInfo ii = {sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                  .dstSet = set,
                                  .dstBinding = 0,
                                  .descriptorCount = 1,
                                  .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                  .pImageInfo = &ii};
    if (view)
        vkUpdateDescriptorSets(device, 1, &write, 0, NULL);
    vkCmdBeginRenderPass(command, &bi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    if (view)
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0,
                                NULL);
    VkViewport vp = {0, 0, 32, 32, 0, 1};
    VkRect2D sc = {{0, 0}, {32, 32}};
    vkCmdSetViewport(command, 0, 1, &vp);
    vkCmdSetScissor(command, 0, 1, &sc);
    vkCmdDraw(command, 3, 1, 0, 0);
    vkCmdEndRenderPass(command);
    VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                              .imageExtent = {32, 32, 1}};
    vkCmdCopyImageToBuffer(command, output.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           result.handle, 1, &copy);
    submit();
}
#include "dimension_tests.h"
#include "sampler_bias_tests.h"
#include "texture_discard_tests.h"
static int only_dimension_tests;
static void format_tests(void)
{
    struct image output =
        image(32, 32, VK_FORMAT_R32G32B32A32_SFLOAT,
              VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    struct buffer result = buffer(32 * 32 * 16, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    VkAttachmentDescription attachment = {.format = VK_FORMAT_R32G32B32A32_SFLOAT,
                                          .samples = VK_SAMPLE_COUNT_1_BIT,
                                          .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                                          .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                          .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                          .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
    VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                .colorAttachmentCount = 1,
                                .pColorAttachments = &ref};
    VkSubpassDependency dependency = {.srcSubpass = 0,
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
                                 .pDependencies = &dependency};
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
    VkDescriptorSetLayoutBinding binding = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                            VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo si = {.sType =
                                              VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                          .bindingCount = 1,
                                          .pBindings = &binding};
    VkDescriptorSetLayout set_layout;
    VK(vkCreateDescriptorSetLayout(device, &si, NULL, &set_layout));
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                     .setLayoutCount = 1,
                                     .pSetLayouts = &set_layout};
    VkPipelineLayout layout;
    VK(vkCreatePipelineLayout(device, &li, NULL, &layout));
    VkPipeline pipelines[3] = {
        pipeline_shaders(builtin_vert, sizeof builtin_vert, NULL, pass, layout, texture_frag,
                         sizeof texture_frag, 0, 0, 0),
        pipeline_shaders(builtin_vert, sizeof builtin_vert, NULL, pass, layout, texture_uint_frag,
                         sizeof texture_uint_frag, 0, 0, 0),
        pipeline_shaders(builtin_vert, sizeof builtin_vert, NULL, pass, layout, texture_sint_frag,
                         sizeof texture_sint_frag, 0, 0, 0)};
    VkDescriptorPoolSize size = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    VkDescriptorPoolCreateInfo dpi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                      .maxSets = 1,
                                      .poolSizeCount = 1,
                                      .pPoolSizes = &size};
    VkDescriptorPool dp;
    VK(vkCreateDescriptorPool(device, &dpi, NULL, &dp));
    VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                      .descriptorPool = dp,
                                      .descriptorSetCount = 1,
                                      .pSetLayouts = &set_layout};
    VkDescriptorSet set;
    VK(vkAllocateDescriptorSets(device, &ai, &set));
    VkSamplerCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                               .magFilter = VK_FILTER_NEAREST,
                               .minFilter = VK_FILTER_NEAREST,
                               .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                               .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                               .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                               .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    VkSampler sampler;
    VK(vkCreateSampler(device, &sci, NULL, &sampler));
    unsigned exercised = 0;
    struct buffer texels = {0};
    VkBufferView texel_output = VK_NULL_HANDLE;
    struct compute_case texel_cases[3] = {{0}};
    if (properties.limits.maxTexelBufferElements && !only_dimension_tests) {
        texels = buffer(64, VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT);
        texel_output = texel_view(texels.handle, VK_FORMAT_R32G32B32A32_SFLOAT, 0, 64);
        const VkDescriptorType types[2] = {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
                                           VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER};
        texel_cases[0] = compute_case(texel_bgra_comp, sizeof texel_bgra_comp, 2, types);
        texel_cases[1] = compute_case(texel_uint_comp, sizeof texel_uint_comp, 2, types);
        texel_cases[2] = compute_case(texel_sint_comp, sizeof texel_sint_comp, 2, types);
    }
    if (only_dimension_tests) {
        dimension_tests(pass, fb, layout, set, sampler, output, result);
        goto cleanup;
    }
    sampler_bias_tests(pass,fb,set_layout,set,output,result);
    texture_discard_tests(pass,fb,layout,set,output,result);
    for (unsigned f = 0; f < sizeof tested_formats / sizeof tested_formats[0]; f++) {
        VkFormat format = tested_formats[f].format;
        VkFormatProperties props;
        vkGetPhysicalDeviceFormatProperties(physical, format, &props);
        if (!(props.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))
            continue;
        unsigned kind = tested_formats[f].kind, components = tested_formats[f].components;
        struct image source = image(2, 2, format,
                                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                        VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        struct buffer raw =
            buffer(256, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                            VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT);
        VkClearColorValue value = {.float32 = {.25f, .5f, .75f, 1}};
        if (kind == 1)
            value = (VkClearColorValue){.uint32 = {17, 31, 7, 1}};
        if (kind == 2)
            value = (VkClearColorValue){.int32 = {-7, 4, -1, 1}};
        VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                  .imageExtent = {2, 2, 1}};
        begin();
        barrier(source.handle, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdClearColorImage(command, source.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value,
                             1, &range);
        barrier(source.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        vkCmdCopyImageToBuffer(command, source.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               raw.handle, 1, &copy);
        compute_visibility(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        barrier(source.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdCopyBufferToImage(command, raw.handle, source.handle,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        barrier(source.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        sample_format_image(source.view, pass, fb, pipelines[kind], layout, set, sampler, output,
                            result);
        float *pixel = (float *)result.map + 4 * (16 * 32 + 16);
        int ok = 1;
        for (unsigned c = 0; c < 4; c++) {
            float expected = c >= components ? (c == 3 ? 1 : 0)
                             : kind == 1     ? (float)value.uint32[c]
                             : kind == 2     ? (float)value.int32[c]
                                             : value.float32[c];
            if (!isfinite(pixel[c]) || fabsf(pixel[c] - expected) > (kind ? 0.001f : 0.04f))
                ok = 0;
        }
        if (!ok)
            printf("FORMAT %d sampled %.7g %.7g %.7g %.7g\n", format, pixel[0], pixel[1], pixel[2],
                   pixel[3]);
        EXPECT(ok, "format clear/transfer roundtrip and GPU sampling");
        if (props.bufferFeatures & VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT) {
            VkBufferView input = texel_view(raw.handle, format, 0, VK_WHOLE_SIZE);
            VkBufferView views[2] = {input, texel_output};
            VkWriteDescriptorSet writes[2];
            for (unsigned i = 0; i < 2; i++)
                writes[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                                   .dstSet = texel_cases[kind].set,
                                                   .dstBinding = i,
                                                   .descriptorCount = 1,
                                                   .descriptorType =
                                                       i ? VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER
                                                         : VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
                                                   .pTexelBufferView = views + i};
            vkUpdateDescriptorSets(device, 2, writes, 0, NULL);
            begin();
            compute_visibility(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                               VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
            bind_compute_case(texel_cases[kind]);
            vkCmdDispatch(command, 1, 1, 1);
            compute_visibility(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                               VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
            submit();
            ok = 1;
            for (unsigned i = 0; i < 16; i++) {
                unsigned c = i % 4;
                float expected = c >= components ? (c == 3 ? 1 : 0)
                                 : kind == 1     ? (float)value.uint32[c]
                                 : kind == 2     ? (float)value.int32[c]
                                                 : value.float32[c];
                if (!isfinite(((float *)texels.map)[i]) ||
                    fabsf(((float *)texels.map)[i] - expected) > (kind ? .001f : .04f))
                    ok = 0;
            }
            if (!ok)
                printf("TEXEL FORMAT %d: %.7g %.7g %.7g %.7g\n", format, ((float *)texels.map)[0],
                       ((float *)texels.map)[1], ((float *)texels.map)[2],
                       ((float *)texels.map)[3]);
            EXPECT(ok,
                   "GPU uniform texel reads preserve packed, half, normalized and integer formats");
            vkDestroyBufferView(device, input, NULL);
        }
        exercised++;
        free_buffer(raw);
        free_image(source);
    }
    static const VkFormat bc_formats[] = {
        VK_FORMAT_BC1_RGB_UNORM_BLOCK, VK_FORMAT_BC1_RGB_SRGB_BLOCK, VK_FORMAT_BC1_RGBA_UNORM_BLOCK,
        VK_FORMAT_BC1_RGBA_SRGB_BLOCK, VK_FORMAT_BC2_UNORM_BLOCK,    VK_FORMAT_BC2_SRGB_BLOCK,
        VK_FORMAT_BC3_UNORM_BLOCK,     VK_FORMAT_BC3_SRGB_BLOCK,     VK_FORMAT_BC4_UNORM_BLOCK,
        VK_FORMAT_BC4_SNORM_BLOCK,     VK_FORMAT_BC5_UNORM_BLOCK,    VK_FORMAT_BC5_SNORM_BLOCK,
        VK_FORMAT_BC6H_UFLOAT_BLOCK,   VK_FORMAT_BC6H_SFLOAT_BLOCK,  VK_FORMAT_BC7_UNORM_BLOCK,
        VK_FORMAT_BC7_SRGB_BLOCK,
    };
    for (unsigned f = 0; f < 16; f++) {
        VkFormat format = bc_formats[f];
        unsigned block_bytes = f < 4 || f == 8 || f == 9 ? 8 : 16;
        struct image source =
            image_levels(9, 7, format,
                         VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                             VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                         3, 3, 0);
        struct image copied =
            image(5, 7, format, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        struct buffer blocks =
            buffer(2048, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        memset(blocks.map, 0xa5, 2048);
        uint8_t block[16] = {0};
        float expected[4] = {0, 0, 0, 1};
        if (f < 8) {
            unsigned color = f < 4 ? 0 : 8;
            block[color] = 0;
            block[color + 1] = 0xf8;
            block[color + 2] = 0xe0;
            block[color + 3] = 7;
            expected[0] = 1;
            if (f == 4 || f == 5)
                memset(block, 255, 8);
            if (f == 6 || f == 7)
                block[0] = 255;
        } else if (f < 12) {
            int sign = f == 9 || f == 11;
            block[0] = sign ? 129 : 255;
            expected[0] = sign ? -1 : 1;
            if (f >= 10) {
                block[8] = 64;
                expected[1] = 64.0f / (sign ? 127 : 255);
            }
        } else if (f >= 14) {
            block[0] = 0x40;
            const unsigned fields[4] = {7, 14, 49, 56};
            for (unsigned c = 0; c < 4; c++)
                for (unsigned bit = 0; bit < 7; bit++)
                    block[(fields[c] + bit) / 8] |= 1u << ((fields[c] + bit) % 8);
            expected[0] = expected[3] = 254.0f / 255;
            if (f == 15)
                expected[0] = powf((expected[0] + .055f) / 1.055f, 2.4f);
        }
        unsigned pitch = 4 * block_bytes, layer_pitch = 12 * block_bytes;
        for (unsigned layer = 0; layer < 3; layer++)
            for (unsigned y = 0; y < 2; y++)
                for (unsigned x = 0; x < 3; x++)
                    memcpy((uint8_t *)blocks.map + 16 + layer * layer_pitch + y * pitch +
                               x * block_bytes,
                           block, block_bytes);
        VkBufferImageCopy transfer = {.bufferOffset = 16,
                                      .bufferRowLength = 16,
                                      .bufferImageHeight = 12,
                                      .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 3},
                                      .imageExtent = {9, 7, 1}};
        VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 3, 0, 3};
        VkImageMemoryBarrier ib = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                   .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                   .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                   .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                   .image = source.handle,
                                   .subresourceRange = range,
                                   .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT};
        begin();
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
        vkCmdCopyBufferToImage(command, blocks.handle, source.handle,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &transfer);
        transfer.imageSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, 1};
        transfer.imageExtent = (VkExtent3D){4, 3, 1};
        transfer.bufferRowLength = transfer.bufferImageHeight = 0;
        vkCmdCopyBufferToImage(command, blocks.handle, source.handle,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &transfer);
        ib.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        ib.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        ib.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        ib.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
        transfer.bufferOffset = 512;
        transfer.bufferRowLength = 16;
        transfer.bufferImageHeight = 12;
        transfer.imageSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 3};
        transfer.imageExtent = (VkExtent3D){9, 7, 1};
        vkCmdCopyImageToBuffer(command, source.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               blocks.handle, 1, &transfer);
        barrier(copied.handle, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageCopy copy = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 2, 1},
                            .srcOffset = {4, 0, 0},
                            .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                            .extent = {5, 7, 1}};
        vkCmdCopyImage(command, source.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, copied.handle,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        barrier(copied.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        sample_format_image(copied.view, pass, fb, pipelines[0], layout, set, sampler, output,
                            result);
        int ok = 1;
        for (unsigned layer = 0; layer < 3; layer++)
            for (unsigned y = 0; y < 2; y++) {
                uint8_t *row = (uint8_t *)blocks.map + 512 + layer * layer_pitch + y * pitch;
                for (unsigned x = 0; x < 3; x++)
                    if (memcmp(row + x * block_bytes, block, block_bytes))
                        ok = 0;
                for (unsigned x = 3 * block_bytes; x < pitch; x++)
                    if (row[x] != 0xa5)
                        ok = 0;
            }
        EXPECT(ok, "BC canonical blocks, padded rows and array-layer pitches survive transfers");
        float *pixel = (float *)result.map + 4 * (16 * 32 + 16);
        ok = 1;
        for (unsigned c = 0; c < 4; c++)
            if (!isfinite(pixel[c]) || fabsf(pixel[c] - expected[c]) > .008f)
                ok = 0;
        if (!ok)
            printf("BC FORMAT %d: %.7g %.7g %.7g %.7g\n", format, pixel[0], pixel[1], pixel[2],
                   pixel[3]);
        EXPECT(ok, "BC decoded texture sampling after a block-aligned image subrectangle copy");
        VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                    .image = source.handle,
                                    .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                    .format = format,
                                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, 1, 1}};
        VkImageView view;
        VK(vkCreateImageView(device, &vi, NULL, &view));
        begin();
        ib.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        ib.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        ib.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        ib.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
        sample_format_image(view, pass, fb, pipelines[0], layout, set, sampler, output, result);
        pixel = (float *)result.map + 4 * (16 * 32 + 16);
        ok = 1;
        for (unsigned c = 0; c < 4; c++)
            if (!isfinite(pixel[c]) || fabsf(pixel[c] - expected[c]) > .008f)
                ok = 0;
        EXPECT(ok, "BC view sampling selects a nonzero mip and array layer");
        vkDestroyImageView(device, view, NULL);
        free_buffer(blocks);
        free_image(copied);
        free_image(source);
    }
    /* Attribute bindings deliberately differ from shader locations, and use
     * offsets/strides plus signed and unsigned missing-component defaults. */
    struct buffer vertices = buffer(200, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    const VkFormat position_formats[3] = {VK_FORMAT_R8G8_SINT, VK_FORMAT_R16G16_SINT,
                                          VK_FORMAT_R32G32_SINT};
    const VkFormat color_formats[4] = {VK_FORMAT_R8G8B8_UINT, VK_FORMAT_R16G16B16_UINT,
                                       VK_FORMAT_R32G32B32_UINT, VK_FORMAT_A2B10G10R10_UINT_PACK32};
    const int32_t positions[6] = {-8, -8, 8, -8, 0, 8};
    VkVertexInputBindingDescription vb = {5, 64, VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attrs[2] = {{0, 5, 0, 4}, {1, 5, 0, 16}};
    VkPipelineVertexInputStateCreateInfo vertex = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1,
        .pVertexBindingDescriptions = &vb,
        .vertexAttributeDescriptionCount = 2,
        .pVertexAttributeDescriptions = attrs};
    for (unsigned p = 0; p < 3; p++)
        for (unsigned c = 0; c < 4; c++) {
            memset(vertices.map, 0xa5, 200);
            unsigned bytes = 1u << p, color_bytes = c < 3 ? 1u << c : 4;
            for (unsigned i = 0; i < 3; i++) {
                for (unsigned j = 0; j < 2; j++) {
                    uint32_t raw = (uint32_t)positions[2 * i + j];
                    memcpy((uint8_t *)vertices.map + 64 * i + 4 + j * bytes, &raw, bytes);
                }
                if (c < 3)
                    for (unsigned j = 0; j < 3; j++) {
                        uint32_t raw = j == 0 ? 255 : 0;
                        memcpy((uint8_t *)vertices.map + 64 * i + 16 + j * color_bytes, &raw,
                               color_bytes);
                    }
                else {
                    uint32_t packed = (1u << 30) | 255;
                    memcpy((uint8_t *)vertices.map + 64 * i + 16, &packed, 4);
                }
            }
            attrs[0].format = position_formats[p];
            attrs[1].format = color_formats[c];
            pipeline_vertex = &vertex;
            VkPipeline render =
                pipeline_shaders(vertex_formats_vert, sizeof vertex_formats_vert, NULL, pass,
                                 layout, triangle_frag, sizeof triangle_frag, 0, 0, 0);
            pipeline_vertex = NULL;
            begin();
            VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(command, 5, 1, &vertices.handle, &offset);
            sample_format_image(VK_NULL_HANDLE, pass, fb, render, layout, set, sampler, output,
                                result);
            float *pixel = (float *)result.map + 4 * (16 * 32 + 16);
            EXPECT(fabsf(pixel[0] - 1) < .0001f && fabsf(pixel[1]) < .0001f &&
                       fabsf(pixel[2]) < .0001f && fabsf(pixel[3] - 1) < .0001f,
                   "GPU vertex fetch preserves signed/unsigned 8/16/32/packed values and typed "
                   "default components");
            vkDestroyPipeline(device, render, NULL);
        }
    free_buffer(vertices);

    struct image depth = image(2, 2, VK_FORMAT_D16_UNORM,
                               VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                   VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    struct buffer raw_depth = buffer(32, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    begin();
    barrier_aspect(depth.handle, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_IMAGE_ASPECT_DEPTH_BIT);
    VkClearDepthStencilValue clear_depth = {.5f, 0};
    VkImageSubresourceRange depth_range = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    vkCmdClearDepthStencilImage(command, depth.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                &clear_depth, 1, &depth_range);
    barrier_aspect(depth.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
    VkBufferImageCopy depth_copy = {.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1},
                                    .imageExtent = {2, 2, 1}};
    vkCmdCopyImageToBuffer(command, depth.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           raw_depth.handle, 1, &depth_copy);
    barrier_aspect(depth.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
    sample_format_image(depth.view, pass, fb, pipelines[0], layout, set, sampler, output, result);
    float *depth_pixel = (float *)result.map + 4 * (16 * 32 + 16);
    int depth_ok = 1;
    for (unsigned i = 0; i < 4; i++)
        if (((uint16_t *)raw_depth.map)[i] != 32768)
            depth_ok = 0;
    EXPECT(depth_ok && fabsf(depth_pixel[0] - .5f) < .00002f,
           "D16 clear, depth-aspect transfer and GPU sampling preserve normalized depth");
    free_image(depth);
    free_buffer(raw_depth);

    if (texel_output) {
        struct image half = image(1, 1, VK_FORMAT_R16G16B16A16_SFLOAT,
                                  VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                      VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        struct buffer raw =
            buffer(16, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT);
        begin();
        barrier(half.handle, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkClearColorValue values = {.float32 = {0x1p-24f, 65504.0f, -0.0f, NAN}};
        VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(command, half.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &values, 1,
                             &range);
        barrier(half.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                  .imageExtent = {1, 1, 1}};
        vkCmdCopyImageToBuffer(command, half.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               raw.handle, 1, &copy);
        submit();
        uint16_t *words = raw.map;
        EXPECT(words[0] == 1 && words[1] == 0x7bff && words[2] == 0x8000 &&
                   (words[3] & 0x7c00) == 0x7c00 && (words[3] & 0x3ff),
               "half-float clears preserve subnormals, finite maxima, signed zero and NaN");
        VkBufferView input = texel_view(raw.handle, VK_FORMAT_R16G16B16A16_SFLOAT, 0, 8);
        VkBufferView views[2] = {input, texel_output};
        VkWriteDescriptorSet writes[2];
        for (unsigned i = 0; i < 2; i++)
            writes[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                               .dstSet = texel_cases[0].set,
                                               .dstBinding = i,
                                               .descriptorCount = 1,
                                               .descriptorType =
                                                   i ? VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER
                                                     : VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
                                               .pTexelBufferView = views + i};
        vkUpdateDescriptorSets(device, 2, writes, 0, NULL);
        begin();
        compute_visibility(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
        bind_compute_case(texel_cases[0]);
        vkCmdDispatch(command, 1, 1, 1);
        compute_visibility(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                           VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
        submit();
        float *v = texels.map;
        EXPECT(v[0] == 0x1p-24f && v[1] == 65504.0f && v[2] == 0 && signbit(v[2]) && isnan(v[3]),
               "Metal half-float texel reads preserve subnormals, signed zero and NaN without half "
               "arithmetic");
        vkDestroyBufferView(device, input, NULL);
        free_image(half);
        free_buffer(raw);
    }
    dimension_tests(pass, fb, layout, set, sampler, output, result);
    EXPECT(exercised >= 50,
           "at least 50 normalized, integer, float and packed image formats exercised");
cleanup:
    if (texel_output) {
        for (unsigned i = 0; i < 3; i++)
            free_compute_case(texel_cases[i]);
        vkDestroyBufferView(device, texel_output, NULL);
        free_buffer(texels);
    }
    for (unsigned i = 0; i < 3; i++)
        vkDestroyPipeline(device, pipelines[i], NULL);
    vkDestroySampler(device, sampler, NULL);
    vkDestroyDescriptorPool(device, dp, NULL);
    vkDestroyPipelineLayout(device, layout, NULL);
    vkDestroyDescriptorSetLayout(device, set_layout, NULL);
    vkDestroyFramebuffer(device, fb, NULL);
    vkDestroyRenderPass(device, pass, NULL);
    free_buffer(result);
    free_image(output);
}
