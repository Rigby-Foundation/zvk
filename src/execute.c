/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (C) 2026 Rigby Foundation */
#include "zvk_internal.h"
#include <math.h>
#include <spirv/unified1/spirv.h>

struct execution {
    VkQueue queue;
    VkQueryPool active_query;
    uint32_t active_query_index;
    VkPipeline pipeline, compute_pipeline;
    VkFramebuffer framebuffer;
    VkRenderPass pass;
    uint32_t subpass;
    const struct zvk_begin_pass *begin;
    VkRect2D area, scissor;
    VkViewport viewport;
    int have_viewport, have_scissor;
    VkBuffer vertices[ZVK_ATTRS], index;
    VkDeviceSize vertex_offsets[ZVK_ATTRS], index_offset;
    VkIndexType index_type;
    VkDescriptorSet sets[ZVK_SETS], compute_sets[ZVK_SETS];
    const uint32_t *compute_dynamic[ZVK_SETS];
    const uint32_t *dynamic[ZVK_SETS];
    uint32_t push[3][ZVK_PUSH / 4];
    float blend[4];
    int have_blend, have_line_width, have_depth_bias;
    float line_width, depth_bias[3];
    uint32_t dynamic_raster;
    virgl_res *uniforms[2][ZVK_UBOS];
    uint32_t uniform_lengths[2][ZVK_UBOS];
    virgl_res *atlas[2];
    uint32_t atlas_view[2],atlas_sampler[2],atlas_height[2],atlas_layers[2];
    VkBuffer uploaded[64];
    uint32_t nuploaded;
    uint64_t data_generation;
    struct {
        VkBuffer buffer;
        VkDeviceSize start;
        VkFormat format;
        uint32_t stride;
        uint64_t generation, count, prefix, copies;
    } vertex_cache[ZVK_ATTRS];
    virgl_res *expanded_vertices[ZVK_ATTRS];
    virgl_res *empty_gpu;
    uint32_t empty_surface, empty_samples;
    uint32_t stencil[3][2], have_stencil[3], dynamic_dsa;
};
/* Host-coherent memory is read afresh on each execution and after commands
 * that can modify it or establish host visibility. Repeated draws in between
 * share the same immutable input bytes, rather than fencing an entire buffer
 * upload for every vertex attribute on every draw. */
static VkResult upload_buffer(struct execution *e, VkBuffer buffer)
{
    for (unsigned i = 0; i < e->nuploaded; i++)
        if (e->uploaded[i] == buffer)
            return VK_SUCCESS;
    VkResult result = zvk_buffer_upload(buffer);
    if (result == VK_SUCCESS) {
        if (e->nuploaded == ZVK_ARRAY_SIZE(e->uploaded))
            e->nuploaded = 0;
        e->uploaded[e->nuploaded++] = buffer;
    }
    return result;
}
/* Keep the CPU transfer map in Z24:S8 order regardless of the host's
 * packed GL depth/stencil order. Vulkan copies expose aspects separately. */
static void depth_order(VkImage image, uint32_t mip, uint32_t layer, int to_host)
{
    if (image->format != VIRGL_FORMAT_S8_UINT_Z24_UNORM)
        return;
    uint32_t *pixels =
        (uint32_t *)((uint8_t *)zvk_image_map(image) + zvk_image_offset(image, mip, layer));
    for (size_t i = 0; i < (size_t)zvk_mip(image->info.extent.width, mip) *
                               zvk_mip(image->info.extent.height, mip);
         i++) {
        uint32_t p = pixels[i];
        pixels[i] = to_host ? (p << 8) | (p >> 24) : (p >> 8) | (p << 24);
    }
}
static VkResult image_read(VkImage image, uint32_t mip, uint32_t layer)
{
    if (image->info.tiling == VK_IMAGE_TILING_LINEAR)
        return image->memory ? VK_SUCCESS : VK_ERROR_INITIALIZATION_FAILED;
    if (image->info.samples != VK_SAMPLE_COUNT_1_BIT)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    if (image->transfer_valid[mip * image->info.arrayLayers + layer])
        return VK_SUCCESS;
    VkResult r = zvk_image_gpu(image);
    if (r != VK_SUCCESS)
        return r;
    uint32_t width = zvk_mip(image->info.extent.width, mip),
             height = zvk_mip(image->info.extent.height, mip),
             depth = zvk_mip(image->info.extent.depth, mip);
    if (zvk_block(image->info.format) == 4)
        return VK_SUCCESS;
    int expanded = image->transfer_map != NULL;
    uint32_t bpp = zvk_gpu_bpp(image);
    VkDeviceSize offset = zvk_gpu_image_offset(image, mip, layer);
    /* The pinned host can drop later slices of thin volumes in bulk
     * transfers. Explicit slices preserve data for every mip shape. */
    for (unsigned z = 0; z < depth; z++)
        if (virgl_res_from_host(image->device->gpu, image->gpu, mip, 0, 0, layer + z, width, height,
                                1, width * bpp, width * height * bpp,
                                offset + (VkDeviceSize)z * width * height * bpp))
            return VK_ERROR_DEVICE_LOST;
    if (expanded) {
        uint8_t *pixels = (uint8_t *)image->gpu->map + offset;
        uint8_t *out = image->transfer_map + zvk_image_offset(image, mip, layer);
        for (size_t i = 0; i < (size_t)width * height * depth; i++) {
            uint16_t packed = ((pixels[4 * i] + 8) / 17) << 4 |
                              ((pixels[4 * i + 1] + 8) / 17) << 8 |
                              ((pixels[4 * i + 2] + 8) / 17) << 12 | ((pixels[4 * i + 3] + 8) / 17);
            memcpy(out + 2 * i, &packed, 2);
        }
    }
    depth_order(image, mip, layer, 0);
    image->transfer_valid[mip * image->info.arrayLayers + layer] = 1;
    return VK_SUCCESS;
}
/* A single-sample stencil aspect is a byte-format transfer. Keep it separate
 * from depth sampling, including mip/layer/cube view restrictions. */
static uint32_t swizzle(VkComponentSwizzle s, uint32_t component);
static VkResult stencil_view(VkImageView view, int input, uint32_t *out)
{
    VkImage image = view->image;
    virgl *gpu = view->device->gpu;
    unsigned levels = view->range.levelCount, layers = input ? 1 : view->range.layerCount;
    unsigned w = zvk_mip(image->info.extent.width, view->range.baseMipLevel);
    unsigned h = zvk_mip(image->info.extent.height, view->range.baseMipLevel);
    unsigned target = input ? PIPE_TEXTURE_2D : view->target;
    virgl_res **res = input ? &view->input_gpu : &view->stencil_gpu;
    uint32_t *handle = input ? &view->input_view : &view->sampler_view;
    if (!*res) {
        uint64_t size = 0;
        for (unsigned m = 0; m < levels; m++)
            size += (uint64_t)zvk_mip(w, m) * zvk_mip(h, m) * layers;
        struct gpu_res_create rc = {.target = target,
                                    .format = VIRGL_FORMAT_R8_UINT,
                                    .bind = VIRGL_BIND_SAMPLER_VIEW,
                                    .width = w,
                                    .height = h,
                                    .depth = 1,
                                    .array_size = layers,
                                    .last_level = levels - 1,
                                    .size = size};
        *res = virgl_res_create_ex(gpu, &rc);
        if (!*res)
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    uint64_t offset = 0;
    for (unsigned m = 0; m < levels; m++) {
        unsigned mw = zvk_mip(w, m), mh = zvk_mip(h, m);
        for (unsigned l = 0; l < layers; l++) {
            VkResult r =
                image_read(image, view->range.baseMipLevel + m, view->range.baseArrayLayer + l);
            if (r != VK_SUCCESS)
                return r;
            const uint8_t *pixels = (uint8_t *)zvk_image_map(image) +
                                    zvk_image_offset(image, view->range.baseMipLevel + m,
                                                     view->range.baseArrayLayer + l);
            uint8_t *dest = (uint8_t *)(*res)->map + offset;
            for (size_t p = 0; p < (size_t)mw * mh; p++)
                dest[p] = pixels[p * 4 + 3];
            if (virgl_res_to_host(gpu, *res, m, 0, 0, l, mw, mh, 1, mw, mw * mh, offset))
                return VK_ERROR_DEVICE_LOST;
            offset += (size_t)mw * mh;
        }
    }
    if (!*handle) {
        *handle = virgl_handle(gpu);
        uint32_t mapping = swizzle(view->swizzle.r, 0) | (swizzle(view->swizzle.g, 1) << 3) |
                           (swizzle(view->swizzle.b, 2) << 6) | (swizzle(view->swizzle.a, 3) << 9);
        virgl_create_sampler_view_ex(gpu, *handle, (*res)->id, target, VIRGL_FORMAT_R8_UINT, 0,
                                     layers - 1, 0, levels - 1, mapping);
    }
    *out = *handle;
    return VK_SUCCESS;
}
static VkResult image_write(VkImage image, uint32_t mip, uint32_t layer)
{
    if (image->info.tiling == VK_IMAGE_TILING_LINEAR)
        return image->memory ? VK_SUCCESS : VK_ERROR_INITIALIZATION_FAILED;
    uint32_t width = zvk_mip(image->info.extent.width, mip),
             height = zvk_mip(image->info.extent.height, mip),
             depth = zvk_mip(image->info.extent.depth, mip);
    int expanded = image->transfer_map != NULL;
    uint32_t bpp = zvk_gpu_bpp(image);
    VkDeviceSize offset = zvk_gpu_image_offset(image, mip, layer);
    if (zvk_block(image->info.format) == 4) {
        for (unsigned z = 0; z < depth; z++)
            zvk_decode_bc(image->info.format,
                          image->transfer_map + zvk_image_offset(image, mip, layer) +
                              (VkDeviceSize)z * zvk_block_count(image->info.format, width) *
                                  zvk_block_count(image->info.format, height) * image->bpp,
                          (uint8_t *)image->gpu->map + offset +
                              (VkDeviceSize)z * width * height * bpp,
                          width, height);
    } else if (expanded) {
        uint8_t *pixels = (uint8_t *)image->gpu->map + offset;
        const uint8_t *source = image->transfer_map + zvk_image_offset(image, mip, layer);
        for (size_t i = 0; i < (size_t)width * height * depth; i++) {
            uint16_t packed;
            memcpy(&packed, source + 2 * i, 2);
            pixels[4 * i] = ((packed >> 4) & 15) * 17;
            pixels[4 * i + 1] = ((packed >> 8) & 15) * 17;
            pixels[4 * i + 2] = ((packed >> 12) & 15) * 17;
            pixels[4 * i + 3] = (packed & 15) * 17;
        }
    }
    depth_order(image, mip, layer, 1);
    int failed = 0;
    for (unsigned z = 0; z < depth && !failed; z++)
        failed = virgl_res_to_host(image->device->gpu, image->gpu, mip, 0, 0, layer + z, width,
                                   height, 1, width * bpp, width * height * bpp,
                                   offset + (VkDeviceSize)z * width * height * bpp);
    depth_order(image, mip, layer, 0);
    if (!failed)
        image->transfer_valid[mip * image->info.arrayLayers + layer] = 1;
    return failed ? VK_ERROR_DEVICE_LOST : VK_SUCCESS;
}
#include "mutable_views.h"
/* Nearest depth/stencil blits are byte-preserving transfers. The GL fallback
 * can change packed depth values or omit stencil when scaling; stage the two
 * aspects explicitly, just as image/buffer copies do. */
static VkResult depth_blit(const struct zvk_blit *p)
{
    const VkImageBlit *r = &p->region;
    unsigned sm = r->srcSubresource.mipLevel, dm = r->dstSubresource.mipLevel;
    unsigned sl = r->srcSubresource.baseArrayLayer, dl = r->dstSubresource.baseArrayLayer;
    VkResult result = image_read(p->src, sm, sl);
    if (result != VK_SUCCESS)
        return result;
    result = image_read(p->dst, dm, dl);
    if (result != VK_SUCCESS)
        return result;
    int dx = r->dstOffsets[1].x - r->dstOffsets[0].x;
    int dy = r->dstOffsets[1].y - r->dstOffsets[0].y;
    if (!dx || !dy)
        return VK_SUCCESS;
    unsigned sw = zvk_mip(p->src->info.extent.width, sm);
    unsigned sh = zvk_mip(p->src->info.extent.height, sm);
    unsigned dw = zvk_mip(p->dst->info.extent.width, dm), bpp = p->src->bpp;
    const uint8_t *source = (uint8_t *)zvk_image_map(p->src) + zvk_image_offset(p->src, sm, sl);
    uint8_t *target = (uint8_t *)zvk_image_map(p->dst) + zvk_image_offset(p->dst, dm, dl);
    for (int j = 0; j < abs(dy); j++) {
        int y = dy > 0 ? r->dstOffsets[0].y + j : r->dstOffsets[0].y - 1 - j;
        int sy = (int)floor(r->srcOffsets[0].y + (j + .5) *
                           (r->srcOffsets[1].y - r->srcOffsets[0].y) / abs(dy));
        if (sy < 0) sy = 0;
        if ((unsigned)sy >= sh) sy = sh - 1;
        for (int i = 0; i < abs(dx); i++) {
            int x = dx > 0 ? r->dstOffsets[0].x + i : r->dstOffsets[0].x - 1 - i;
            int sx = (int)floor(r->srcOffsets[0].x + (i + .5) *
                               (r->srcOffsets[1].x - r->srcOffsets[0].x) / abs(dx));
            if (sx < 0) sx = 0;
            if ((unsigned)sx >= sw) sx = sw - 1;
            const uint8_t *in = source + ((size_t)sy * sw + sx) * bpp;
            uint8_t *out = target + ((size_t)y * dw + x) * bpp;
            if (p->src->info.format == VK_FORMAT_D24_UNORM_S8_UINT) {
                uint32_t a, b, mask = r->srcSubresource.aspectMask == VK_IMAGE_ASPECT_DEPTH_BIT
                                          ? 0x00ffffffu
                                      : r->srcSubresource.aspectMask == VK_IMAGE_ASPECT_STENCIL_BIT
                                          ? 0xff000000u : UINT32_MAX;
                memcpy(&a, in, 4);
                memcpy(&b, out, 4);
                b = (b & ~mask) | (a & mask);
                memcpy(out, &b, 4);
            } else
                memcpy(out, in, bpp);
        }
    }
    return image_write(p->dst, dm, dl);
}
static unsigned unorm(float value, unsigned max)
{
    if (!(value > 0))
        return 0;
    if (value >= 1)
        return max;
    return (unsigned)(value * (float)max + 0.5f);
}
/* Multisample backing cannot be CPU-mapped. Clear a temporary surface for
 * partial rectangles, then copy the matching samples with a GPU blit. This
 * ignores application masks/scissors and does not add occlusion-query samples. */
static VkResult clear_multisample(VkImage image, uint32_t mip, uint32_t layer, VkRect2D area,
                                  const VkClearValue *value, int depth, VkImageAspectFlags aspects)
{
    virgl *gpu = image->device->gpu;
    uint32_t width = zvk_mip(image->info.extent.width, mip),
             height = zvk_mip(image->info.extent.height, mip);
    int partial = area.offset.x || area.offset.y || area.extent.width != width ||
                  area.extent.height != height;
    virgl_res *temporary = NULL;
    if (partial) {
        struct gpu_res_create rc = {.target = PIPE_TEXTURE_2D,
                                    .format = image->format,
                                    .bind =
                                        depth ? VIRGL_BIND_DEPTH_STENCIL : VIRGL_BIND_RENDER_TARGET,
                                    .width = width,
                                    .height = height,
                                    .depth = 1,
                                    .array_size = 1,
                                    .nr_samples = image->info.samples};
        temporary = virgl_res_create_ex(gpu, &rc);
        if (!temporary)
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    uint32_t handle = virgl_handle(gpu);
    virgl_create_surface(gpu, handle, temporary ? temporary->id : image->gpu->id, image->format,
                         temporary ? 0 : mip, temporary ? 0 : layer | (layer << 16));
    virgl_set_framebuffer(gpu, depth ? handle : 0, &handle, depth ? 0 : 1);
    uint32_t mask = depth ? ((aspects & VK_IMAGE_ASPECT_DEPTH_BIT ? PIPE_CLEAR_DEPTH : 0) |
                             (aspects & VK_IMAGE_ASPECT_STENCIL_BIT ? PIPE_CLEAR_STENCIL : 0))
                          : PIPE_CLEAR_COLOR0;
    const float color[4] = {
        depth ? 0 : value->color.float32[0], depth ? 0 : value->color.float32[1],
        depth ? 0 : value->color.float32[2], depth ? 0 : value->color.float32[3]};
    virgl_clear(gpu, mask, color, depth ? value->depthStencil.depth : 0,
                depth ? value->depthStencil.stencil : 0);
    if (temporary) {
        struct virgl_blit_info blit = {
            .mask = depth ? ((aspects & VK_IMAGE_ASPECT_DEPTH_BIT ? PIPE_MASK_Z : 0) |
                             (aspects & VK_IMAGE_ASPECT_STENCIL_BIT ? PIPE_MASK_S : 0))
                          : PIPE_MASK_RGBA,
            .filter = PIPE_TEX_FILTER_NEAREST,
            .src = {temporary->id, 0, image->format, area.offset.x, area.offset.y, 0,
                    (int)area.extent.width, (int)area.extent.height, 1},
            .dst = {image->gpu->id, mip, image->format, area.offset.x, area.offset.y, (int)layer,
                    (int)area.extent.width, (int)area.extent.height, 1}};
        virgl_blit(gpu, &blit);
    }
    virgl_destroy_object(gpu, handle, VIRGL_OBJECT_SURFACE);
    int failed = temporary ? virgl_flush_fenced(gpu) : 0;
    if (temporary)
        virgl_res_destroy(gpu, temporary);
    return failed ? VK_ERROR_DEVICE_LOST : VK_SUCCESS;
}
static VkResult clear_image(VkImage image, uint32_t mip, uint32_t layer, VkRect2D area,
                            const VkClearValue *value, int depth, VkImageAspectFlags aspects)
{
    if (zvk_block(image->info.format) != 1)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    VkResult result = zvk_image_gpu(image);
    if (result != VK_SUCCESS)
        return result;
    if (image->info.samples != VK_SAMPLE_COUNT_1_BIT)
        return clear_multisample(image, mip, layer, area, value, depth, aspects);
    int partial = area.offset.x || area.offset.y ||
                  area.extent.width != zvk_mip(image->info.extent.width, mip) ||
                  area.extent.height != zvk_mip(image->info.extent.height, mip);
    if (partial || (depth && image->info.format == VK_FORMAT_D24_UNORM_S8_UINT &&
                    aspects != (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))) {
        result = image_read(image, mip, layer);
        if (result != VK_SUCCESS)
            return result;
    }
    uint32_t pixel = 0;
    uint8_t texel[16];
    if (!depth)
        zvk_pack_clear(image->info.format, &value->color, texel);
    else {
        if (image->info.format == VK_FORMAT_D32_SFLOAT)
            memcpy(&pixel, &value->depthStencil.depth, 4);
        else if (image->info.format == VK_FORMAT_D16_UNORM)
            pixel = unorm(value->depthStencil.depth, 65535);
        else
            pixel =
                unorm(value->depthStencil.depth, 0xffffff) | (value->depthStencil.stencil << 24);
        memcpy(texel, &pixel, sizeof pixel);
    }
    uint8_t *out = (uint8_t *)zvk_image_map(image) + zvk_image_offset(image, mip, layer);
    for (uint32_t z = 0; z < zvk_mip(image->info.extent.depth, mip); z++)
        for (uint32_t y = 0; y < area.extent.height; y++)
            for (uint32_t x = 0; x < area.extent.width; x++) {
                size_t i = ((size_t)z * zvk_mip(image->info.extent.height, mip) +
                            (uint32_t)area.offset.y + y) *
                               zvk_mip(image->info.extent.width, mip) +
                           (uint32_t)area.offset.x + x;
                if (depth && image->info.format == VK_FORMAT_D24_UNORM_S8_UINT) {
                    uint32_t old;
                    memcpy(&old, out + i * image->bpp, 4);
                    uint32_t packed =
                        (aspects & VK_IMAGE_ASPECT_DEPTH_BIT ? pixel & 0xffffff : old & 0xffffff) |
                        (aspects & VK_IMAGE_ASPECT_STENCIL_BIT ? pixel & 0xff000000
                                                               : old & 0xff000000);
                    memcpy(out + i * image->bpp, &packed, 4);
                } else
                    memcpy(out + i * image->bpp, texel, image->bpp);
            }
    return image_write(image, mip, layer);
}
static VkResult surface(struct execution *e, VkImageView view)
{
    VkResult r = view->format!=view->image->info.format && !view->reinterpret
                     ? mutable_prepare(e,view) : zvk_image_gpu(view_image(view));
    if (r != VK_SUCCESS)
        return r;
    if (!view->surface) {
        view->surface = virgl_handle(view->device->gpu);
        virgl_create_surface(view->device->gpu, view->surface, view_image(view)->gpu->id,
                             view_image(view)->format, view->range.baseMipLevel,
                             view->range.baseArrayLayer | (view->range.baseArrayLayer << 16));
    }
    return VK_SUCCESS;
}
static VkImageSubresourceRange framebuffer_range(VkFramebuffer fb, VkImageView view)
{
    (void)fb;
    /* Implicit attachment layout transitions cover the whole view, even
     * when the framebuffer renders fewer layers than that view exposes. */
    return view->range;
}
static VkResult bind_framebuffer(struct execution *e)
{
    if (!e->framebuffer || !e->pass)
        return VK_ERROR_INITIALIZATION_FAILED;
    uint32_t colors[ZVK_COLORS] = {0}, depth = 0;
    int color_present=0;
    VkResult r;
    for (uint32_t i = 0; i < e->pass->subpasses[e->subpass].ncolors; i++) {
        if (e->pass->subpasses[e->subpass].colors[i] == VK_ATTACHMENT_UNUSED)
            continue;
        VkImageView view = e->framebuffer->views[e->pass->subpasses[e->subpass].colors[i]];
        r = surface(e,view);
        if (r != VK_SUCCESS)
            return r;
        colors[i] = view->surface;
        color_present=1;
    }
    if (e->pass->subpasses[e->subpass].depth != VK_ATTACHMENT_UNUSED) {
        VkImageView view = e->framebuffer->views[e->pass->subpasses[e->subpass].depth];
        r = surface(e,view);
        if (r != VK_SUCCESS)
            return r;
        depth = view->surface;
    }
    uint32_t count = e->pass->subpasses[e->subpass].ncolors;
    /* The transport rejects a nonempty array of null color surfaces even
     * with a depth target. Vulkan's unused slots need no physical targets. */
    if (!color_present && depth)count=0;
    /* GL can ignore the fragment alpha with no color target. Vulkan still
     * applies alpha-to-coverage to depth/stencil writes in that subpass. */
    if ((!color_present && !depth) || (!colors[0] && e->pipeline && e->pipeline->alpha_coverage)) {
        virgl *gpu = e->queue->device->gpu;
        uint32_t samples = e->pipeline ? e->pipeline->samples : VK_SAMPLE_COUNT_1_BIT;
        if (e->empty_gpu &&
            (e->empty_gpu->width != e->framebuffer->width ||
             e->empty_gpu->height != e->framebuffer->height || e->empty_samples != samples)) {
            virgl_destroy_object(gpu, e->empty_surface, VIRGL_OBJECT_SURFACE);
            virgl_res_destroy(gpu, e->empty_gpu);
            e->empty_gpu = NULL;
        }
        if (!e->empty_gpu) {
            struct gpu_res_create rc = {.target = PIPE_TEXTURE_2D,
                                        .format = VIRGL_FORMAT_R8G8B8A8_UNORM,
                                        .bind = VIRGL_BIND_RENDER_TARGET,
                                        .width = e->framebuffer->width,
                                        .height = e->framebuffer->height,
                                        .depth = 1,
                                        .array_size = 1,
                                        .nr_samples =
                                            samples == VK_SAMPLE_COUNT_1_BIT ? 0 : samples};
            e->empty_gpu = virgl_res_create_ex(gpu, &rc);
            if (!e->empty_gpu)
                return VK_ERROR_OUT_OF_DEVICE_MEMORY;
            e->empty_surface = virgl_handle(gpu);
            e->empty_samples = samples;
            virgl_create_surface(gpu, e->empty_surface, e->empty_gpu->id,
                                 VIRGL_FORMAT_R8G8B8A8_UNORM, 0, 0);
        }
        colors[0] = e->empty_surface;
        if (!count)
            count = 1;
    }
    virgl_set_framebuffer(e->queue->device->gpu, depth, colors, count);
    return VK_SUCCESS;
}
static VkResult start_subpass(struct execution *e)
{
    struct zvk_subpass *sub = e->pass->subpasses + e->subpass;
    for (uint32_t i = 0; i < e->pass->count; i++) {
        if (sub->layouts[i] == VK_IMAGE_LAYOUT_UNDEFINED)
            continue;
        VkImageView view = e->framebuffer->views[i];
        VkAttachmentDescription *a = e->pass->attachments + i;
        if (e->pass->first_use[i] == e->subpass) {
            int depth = !!(view->range.aspectMask &
                           (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT));
            VkImageAspectFlags aspects =
                depth
                    ? ((a->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) |
                       (a->stencilLoadOp == VK_ATTACHMENT_LOAD_OP_CLEAR
                            ? VK_IMAGE_ASPECT_STENCIL_BIT
                            : 0))
                    : (a->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR ? VK_IMAGE_ASPECT_COLOR_BIT : 0);
            /* A stencil load operation is ignored for depth-only formats. */
            aspects &= zvk_format_aspects(view->format);
            if (aspects) {
                VkResult prepared=mutable_prepare(e,view);
                if (prepared!=VK_SUCCESS)return prepared;
                if (i >= e->begin->nclear)
                    return VK_ERROR_INITIALIZATION_FAILED;
                for (uint32_t layer = 0; layer < e->framebuffer->layers; layer++) {
                    VkResult r = clear_image(view_image(view), view->range.baseMipLevel,
                                             view->range.baseArrayLayer + layer, e->area,
                                             e->begin->clear + i, depth, aspects);
                    if (r != VK_SUCCESS)
                        return r;
                }
                VkResult committed=mutable_commit(e,view);
                if (committed!=VK_SUCCESS)return committed;
            }
        }
        VkImageSubresourceRange range = framebuffer_range(e->framebuffer, view);
        zvk_layout_set(view->image, &range, sub->layouts[i]);
    }
    return bind_framebuffer(e);
}
static VkResult begin_pass(struct execution *e, const struct zvk_begin_pass *p)
{
    e->framebuffer = p->fb;
    e->pass = p->pass;
    e->subpass = 0;
    e->begin = p;
    e->area = p->area;
    if (p->pass->count != p->fb->count)
        return VK_ERROR_INITIALIZATION_FAILED;
    for (uint32_t i = 0; i < p->pass->count; i++) {
        VkAttachmentDescription *a = p->pass->attachments + i;
        VkImageView view = p->fb->views[i];
        for (uint32_t layer = 0; layer < view->range.layerCount; layer++)
            if (a->initialLayout != VK_IMAGE_LAYOUT_UNDEFINED &&
                !zvk_layout_is(view->image, view->range.baseMipLevel,
                               view->range.baseArrayLayer + layer, a->initialLayout)) {
                zvk_error("render-pass initial image layout mismatch");
                return VK_ERROR_INITIALIZATION_FAILED;
            }
    }
    return start_subpass(e);
}
static VkResult finish_subpass(struct execution *e)
{
    struct zvk_subpass *sub = e->pass->subpasses + e->subpass;
    virgl *gpu = e->queue->device->gpu;
    for (uint32_t i = 0; i < sub->ncolors; i++) {
        if (sub->resolves[i] == VK_ATTACHMENT_UNUSED)
            continue;
        VkImageView src = e->framebuffer->views[sub->colors[i]],
                    dst = e->framebuffer->views[sub->resolves[i]];
        VkResult r = mutable_prepare(e,src);
        if (r != VK_SUCCESS)
            return r;
        r = mutable_prepare(e,dst);
        if (r != VK_SUCCESS)
            return r;
        struct virgl_blit_info blit = {
            .mask = PIPE_MASK_RGBA,
            .filter = PIPE_TEX_FILTER_NEAREST,
            .src = {view_image(src)->gpu->id, src->range.baseMipLevel, view_image(src)->format,
                    e->area.offset.x, e->area.offset.y, (int)src->range.baseArrayLayer,
                    (int)e->area.extent.width, (int)e->area.extent.height, 1},
            .dst = {view_image(dst)->gpu->id, dst->range.baseMipLevel, view_image(dst)->format,
                    e->area.offset.x, e->area.offset.y, (int)dst->range.baseArrayLayer,
                    (int)e->area.extent.width, (int)e->area.extent.height, 1}};
        for (uint32_t layer = 0; layer < e->framebuffer->layers; layer++) {
            blit.src.z = src->range.baseArrayLayer + layer;
            blit.dst.z = dst->range.baseArrayLayer + layer;
            virgl_blit(gpu, &blit);
            view_image(dst)->transfer_valid[dst->range.baseMipLevel * dst->image->info.arrayLayers +
                                           dst->range.baseArrayLayer + layer] = 0;
        }
        r=mutable_commit(e,dst);
        if (r!=VK_SUCCESS)return r;
    }
    /* A full completion is conservative but covers all subpass dependencies. */
    return virgl_flush_fenced(gpu) ? VK_ERROR_DEVICE_LOST : VK_SUCCESS;
}
static uint32_t swizzle(VkComponentSwizzle swizzle, uint32_t channel)
{
    switch (swizzle) {
    case VK_COMPONENT_SWIZZLE_ZERO:
        return 4;
    case VK_COMPONENT_SWIZZLE_ONE:
        return 5;
    case VK_COMPONENT_SWIZZLE_R:
        return 0;
    case VK_COMPONENT_SWIZZLE_G:
        return 1;
    case VK_COMPONENT_SWIZZLE_B:
        return 2;
    case VK_COMPONENT_SWIZZLE_A:
        return 3;
    default:
        return channel;
    }
}
#include "compute_images.h"
#include "graphics_buffers.h"
static VkResult bind_shader_resources(struct execution *e, zvk_shader *shader, int index,
                                      uint32_t first_instance)
{
    VkDevice d = e->queue->device;
    virgl *gpu = d->gpu;

    uint32_t stage = index ? PIPE_SHADER_FRAGMENT : PIPE_SHADER_VERTEX;
    uint32_t constants[(ZVK_PUSH + 16 + ZVK_SAMPLERS * 16) / 4] = {0};
    memcpy(constants, e->push[index], ZVK_PUSH);
    constants[ZVK_PUSH / 4] = first_instance;
    uint32_t samples=e->pipeline->samples;
    constants[ZVK_PUSH / 4 + 1] = e->pipeline->sample_mask &
        (samples<32?(1u<<samples)-1:UINT32_MAX);
    if(shader->fixed_depth_bias) {
        VkPipeline p=e->pipeline;
        float factor=p->dynamic_depth_bias?e->depth_bias[0]:p->raster.depthBiasConstantFactor;
        float constant=ldexpf(factor,-(int)p->fixed_depth_bias_bits);
        float slope=p->dynamic_depth_bias?e->depth_bias[2]:p->raster.depthBiasSlopeFactor;
        memcpy(constants+ZVK_PUSH/4+2,&constant,4);
        memcpy(constants+ZVK_PUSH/4+3,&slope,4);
    }
    uint32_t views[ZVK_SAMPLERS] = {0}, samplers[ZVK_SAMPLERS] = {0}, nsamplers = 0;
    if (shader->resource_atlas) {
        VkResult result=graphics_buffers(e,shader,index,first_instance);
        if (result!=VK_SUCCESS)return result;
        views[15]=e->atlas_view[index];samplers[15]=e->atlas_sampler[index];nsamplers=16;
        if (shader->push_bytes || shader->instance_index || shader->sample_mask_in || shader->sample_mask_out || shader->fixed_depth_bias)
            virgl_set_constants(gpu,stage,(const float *)constants,
                shader->instance_index || shader->sample_mask_in || shader->sample_mask_out || shader->fixed_depth_bias?(ZVK_PUSH+16)/4:(shader->push_bytes+15)/16*4);
        virgl_set_sampler_views(gpu,stage,0,views,nsamplers);
        virgl_bind_sampler_states(gpu,stage,0,samplers,nsamplers);
        return VK_SUCCESS;
    }
    for (uint32_t i = 0; i < shader->nbindings; i++) {
        zvk_shader_binding *binding = shader->bindings + i;
        VkDescriptorSet set = e->sets[binding->set];
        if (!set) {
            zvk_error("descriptor set %u is not bound", binding->set);
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        zvk_descriptor *desc = set->descriptors+binding->index;
        if (binding->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ||
            binding->type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
            if (shader->resource_atlas)continue;
            VkBuffer b = desc->buffer;
            VkDeviceSize extra = (desc->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC ||
                                  desc->type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC)
                                     ? e->dynamic[binding->set][binding->dynamic_index]
                                     : 0;
            if (!b || !b->memory || extra > UINT64_MAX - desc->offset)
                return VK_ERROR_INITIALIZATION_FAILED;
            VkDeviceSize off = desc->offset + extra,
                         range = desc->range == VK_WHOLE_SIZE ? (off <= b->size ? b->size - off : 0)
                                                              : desc->range;
            if (!zvk_range(b->size, off, range) ||
                (!d->enabled_features.robustBufferAccess && range < binding->bytes)) {
                zvk_error("uniform descriptor range is smaller than the shader access");
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            uint32_t length = (binding->bytes + 15) & ~15u;
            if (!length)
                length = 16;
            virgl_res **res = &e->uniforms[index][binding->slot - 1];
            if (!*res) {
                *res = virgl_res_create(gpu, PIPE_BUFFER, VIRGL_FORMAT_R8_UNORM,
                                        VIRGL_BIND_CONSTANT_BUFFER, 16384, 1, 1, 0, 16384);
                if (!*res)
                    return VK_ERROR_OUT_OF_DEVICE_MEMORY;
            }
            /* Bounds are descriptor-relative, not allocation-relative. A
             * partially present 32-bit scalar also returns zero. */
            uint32_t copy = range < binding->bytes ? (uint32_t)range & ~3u : binding->bytes;
            const uint8_t *source = b->memory->data + b->offset + off;
            int changed = e->uniform_lengths[index][binding->slot - 1] != length ||
                          memcmp((*res)->map, source, copy);
            for (uint32_t j = copy; !changed && j < length; j++)
                changed = ((const uint8_t *)(*res)->map)[j] != 0;
            if (changed) {
                memset((*res)->map, 0, length);
                memcpy((*res)->map, source, copy);
                if (virgl_res_to_host(gpu, *res, 0, 0, 0, 0, length, 1, 1, 0, 0, 0))
                    return VK_ERROR_DEVICE_LOST;
                e->uniform_lengths[index][binding->slot - 1] = length;
            }
            virgl_set_uniform_buffer(gpu, stage, binding->slot, 0, length, (*res)->id);
        } else if (binding->type == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER ||
                   binding->type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) {
            VkBufferView view = desc->texel;
            if (!view || !view->buffer->memory)
                return VK_ERROR_INITIALIZATION_FAILED;
            uint32_t bytes = (uint32_t)view->range;
            uint32_t bpp = zvk_bpp(view->format), texels = bytes / bpp;
            uint32_t height = (texels + 4095) / 4096;
            constants[(ZVK_PUSH + 16) / 4 + binding->slot] = texels;
            if (!view->gpu) {
                view->gpu = virgl_res_create(gpu, PIPE_TEXTURE_2D, zvk_format(view->format),
                                             VIRGL_BIND_SAMPLER_VIEW, 4096, height, 1, 0,
                                             (size_t)4096 * height * bpp);
                if (!view->gpu)
                    return VK_ERROR_OUT_OF_DEVICE_MEMORY;
            }
            const uint8_t *source = view->buffer->memory->data + view->buffer->offset + view->offset;
            if (!view->uploaded || memcmp(view->gpu->map, source, bytes)) {
                memcpy(view->gpu->map, source, bytes);
                memset((uint8_t *)view->gpu->map + bytes, 0, view->gpu->size - bytes);
                if (virgl_res_to_host(gpu, view->gpu, 0, 0, 0, 0, 4096, height, 1,
                                      4096 * bpp, 4096 * height * bpp, 0))
                    return VK_ERROR_DEVICE_LOST;
                view->uploaded = 1;
            }
            if (!view->sampler_view) {
                view->sampler_view = virgl_handle(gpu);
                virgl_create_sampler_view_ex(gpu, view->sampler_view, view->gpu->id, PIPE_TEXTURE_2D,
                                             zvk_format(view->format), 0, 0, 0, 0,
                                             VIRGL_SWIZZLE_IDENTITY);
                view->sampler = virgl_handle(gpu);
                struct virgl_sampler_info nearest = {.min_img = PIPE_TEX_FILTER_NEAREST,
                                                       .mag_img = PIPE_TEX_FILTER_NEAREST,
                                                       .min_mip = PIPE_TEX_MIPFILTER_NONE};
                virgl_create_sampler_state_ex(gpu, view->sampler, &nearest);
            }
            views[binding->slot] = view->sampler_view;
            samplers[binding->slot] = view->sampler;
            if (binding->slot >= nsamplers)
                nsamplers = binding->slot + 1;
        } else {
            VkImageView view = desc->image;
            VkSampler sampler=binding->uses_sampling?desc->sampler:NULL;
            if (binding->separate_sampler && binding->uses_sampling) {
                VkDescriptorSet samplers = e->sets[binding->sampler_set];
                if (!samplers)
                    return VK_ERROR_INITIALIZATION_FAILED;
                sampler =
                    samplers->descriptors[binding->sampler_index].sampler;
            }
            constants[(ZVK_PUSH+16)/4+3*ZVK_SAMPLERS+binding->slot]=
                sampler?sampler->create.unnormalizedCoordinates:0;
            int input = binding->type == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
            if (!view ||
                (!!binding->image_ms != (view->image->info.samples != VK_SAMPLE_COUNT_1_BIT)) ||
                (!input && ((!binding->image_ms &&
                             binding->uses_sampling &&
                             !sampler) ||
                            view->target != binding->target)) ||
                (input && view->target != PIPE_TEXTURE_2D && view->target != PIPE_TEXTURE_2D_ARRAY))
                return VK_ERROR_INITIALIZATION_FAILED;
            if (input) {
                if (!e->pass)
                    return VK_ERROR_INITIALIZATION_FAILED;
                struct zvk_subpass *sub = e->pass->subpasses + e->subpass;
                uint32_t index = binding->input_attachment;
                if (index >= sub->ninputs || sub->inputs[index] == VK_ATTACHMENT_UNUSED)
                    return VK_ERROR_INITIALIZATION_FAILED;
                VkImageView attachment = e->framebuffer->views[sub->inputs[index]];
                if (view->image != attachment->image ||
                    view->range.baseMipLevel != attachment->range.baseMipLevel ||
                    view->range.baseArrayLayer != attachment->range.baseArrayLayer ||
                    view->range.levelCount != 1 || !view->range.layerCount)
                    return VK_ERROR_INITIALIZATION_FAILED;
            }
            /* With geometry/tessellation and shader layer output disabled,
             * rasterization and input attachment reads select layer zero.
             * Keep its 2D snapshot separate from ordinary sampled views. */
            constants[(ZVK_PUSH + 16) / 4 + binding->slot] = view->image->info.samples;
            constants[(ZVK_PUSH + 16) / 4 + ZVK_SAMPLERS + binding->slot] =
                swizzle(view->swizzle.r, 0) | (swizzle(view->swizzle.g, 1) << 3) |
                (swizzle(view->swizzle.b, 2) << 6) | (swizzle(view->swizzle.a, 3) << 9) |
                (view->range.levelCount << 16);
            constants[(ZVK_PUSH + 16) / 4 + 2 * ZVK_SAMPLERS + binding->slot] =
                zvk_mip(view->image->info.extent.height, view->range.baseMipLevel);
            uint32_t layers = input ? 1 : view->range.layerCount;
            uint32_t target = input ? PIPE_TEXTURE_2D : view->target;
            virgl_res **sample_gpu = input ? &view->input_gpu : &view->sample_gpu;
            uint32_t *sample_view = input ? &view->input_view : &view->sampler_view;
            for (uint32_t m = view->range.baseMipLevel;
                 m < view->range.baseMipLevel + view->range.levelCount; m++)
                for (uint32_t l = view->range.baseArrayLayer;
                     l < view->range.baseArrayLayer + layers; l++)
                    if (!zvk_layout_is(view->image, m, l, desc->layout))
                        return VK_ERROR_INITIALIZATION_FAILED;
            VkResult r = mutable_prepare(e,view);
            if (r != VK_SUCCESS)
                return r;
            VkImage image = view_image(view);
            int snapshot =
                input ||
                (!(image->device->physical->caps.capability_bits & VIRGL_CAP_TEXTURE_VIEW) &&
                 (view->target != image->target || view->range.baseMipLevel ||
                  view->range.levelCount != image->info.mipLevels || view->range.baseArrayLayer ||
                  view->range.layerCount != image->info.arrayLayers));
            if (binding->image_ms && binding->image_kind == 0) {
                VkDeviceMemory memory;
                r = zvk_sample_snapshot(view, &memory);
                if (r != VK_SUCCESS)
                    return r;
                if (!view->compute_view) {
                    view->compute_view = virgl_handle(gpu);
                    virgl_create_sampler_view_ex(
                        gpu, view->compute_view, view->compute_image->id, PIPE_TEXTURE_2D_ARRAY,
                        VIRGL_FORMAT_R32G32B32A32_UINT, 0, view->range.layerCount - 1, 0, 0,
                        0 | (1 << 3) | (2 << 6) | (3 << 9));
                }
                if (!view->compute_sampler) {
                    view->compute_sampler = virgl_handle(gpu);
                    virgl_create_sampler_state(gpu, view->compute_sampler,
                                               PIPE_TEX_WRAP_CLAMP_TO_EDGE,
                                               PIPE_TEX_WRAP_CLAMP_TO_EDGE, PIPE_TEX_FILTER_NEAREST,
                                               PIPE_TEX_MIPFILTER_NONE, PIPE_TEX_FILTER_NEAREST);
                }
                views[binding->slot] = view->compute_view;
                samplers[binding->slot] = view->compute_sampler;
                if (binding->slot >= nsamplers)
                    nsamplers = binding->slot + 1;
                continue;
            }
            if (view->range.aspectMask == VK_IMAGE_ASPECT_STENCIL_BIT) {
                if (binding->image_kind != 0)
                    return VK_ERROR_INITIALIZATION_FAILED;
                r = stencil_view(view, input, views + binding->slot);
                if (r != VK_SUCCESS)
                    return r;
                if (!input && sampler && !sampler->handle) {
                    sampler->handle = virgl_handle(gpu);
                    virgl_create_sampler_state_ex(gpu, sampler->handle, &sampler->info);
                }
                if (input || !sampler) {
                    if (!view->compute_sampler) {
                        view->compute_sampler = virgl_handle(gpu);
                        virgl_create_sampler_state(
                            gpu, view->compute_sampler, PIPE_TEX_WRAP_CLAMP_TO_EDGE,
                            PIPE_TEX_WRAP_CLAMP_TO_EDGE, PIPE_TEX_FILTER_NEAREST,
                            PIPE_TEX_MIPFILTER_NONE, PIPE_TEX_FILTER_NEAREST);
                    }
                    samplers[binding->slot] = view->compute_sampler;
                } else
                    samplers[binding->slot] = sampler->handle;
                if (binding->slot >= nsamplers)
                    nsamplers = binding->slot + 1;
                continue;
            }
            if (snapshot) {
                if (!*sample_gpu) {
                    struct gpu_res_create rc = {
                        .target = target,
                        .format = image->format,
                        .bind = VIRGL_BIND_SAMPLER_VIEW |
                                (view->range.aspectMask & VK_IMAGE_ASPECT_COLOR_BIT
                                     ? VIRGL_BIND_RENDER_TARGET
                                     : VIRGL_BIND_DEPTH_STENCIL),
                        .width = zvk_mip(image->info.extent.width, view->range.baseMipLevel),
                        .height = zvk_mip(image->info.extent.height, view->range.baseMipLevel),
                        .depth = image->info.imageType == VK_IMAGE_TYPE_3D
                                     ? zvk_mip(image->info.extent.depth, view->range.baseMipLevel)
                                     : 1,
                        .array_size = layers,
                        .last_level = view->range.levelCount - 1,
                        .nr_samples = binding->image_ms ? image->info.samples : 0};
                    *sample_gpu = virgl_res_create_ex(gpu, &rc);
                    if (!*sample_gpu)
                        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
                }
                if (input) {
                    struct virgl_blit_info blit = {
                        .mask = view->range.aspectMask & VK_IMAGE_ASPECT_COLOR_BIT ? PIPE_MASK_RGBA
                                                                                   : PIPE_MASK_Z,
                        .filter = PIPE_TEX_FILTER_NEAREST,
                        .src = {image->gpu->id, view->range.baseMipLevel, image->format, 0, 0,
                                (int)view->range.baseArrayLayer,
                                (int)zvk_mip(image->info.extent.width, view->range.baseMipLevel),
                                (int)zvk_mip(image->info.extent.height, view->range.baseMipLevel),
                                1},
                        .dst = {(*sample_gpu)->id, 0, image->format, 0, 0, 0,
                                (int)zvk_mip(image->info.extent.width, view->range.baseMipLevel),
                                (int)zvk_mip(image->info.extent.height, view->range.baseMipLevel),
                                1}};
                    virgl_blit(gpu, &blit);
                } else {
                    for (uint32_t m = 0; m < view->range.levelCount; m++) {
                        unsigned mip = m + view->range.baseMipLevel;
                        unsigned slices = image->info.imageType == VK_IMAGE_TYPE_3D
                                              ? zvk_mip(image->info.extent.depth, mip)
                                              : view->range.layerCount;
                        for (unsigned slice = 0; slice < slices; slice++) {
                            if (binding->image_ms) {
                                struct virgl_blit_info blit = {
                                    .mask = view->range.aspectMask & VK_IMAGE_ASPECT_COLOR_BIT
                                                ? PIPE_MASK_RGBA
                                                : PIPE_MASK_Z,
                                    .filter = PIPE_TEX_FILTER_NEAREST,
                                    .src = {image->gpu->id, mip, image->format, 0, 0,
                                            (int)(view->range.baseArrayLayer + slice),
                                            (int)zvk_mip(image->info.extent.width, mip),
                                            (int)zvk_mip(image->info.extent.height, mip), 1},
                                    .dst = {(*sample_gpu)->id, m, image->format, 0, 0, (int)slice,
                                            (int)zvk_mip(image->info.extent.width, mip),
                                            (int)zvk_mip(image->info.extent.height, mip), 1}};
                                virgl_blit(gpu, &blit);
                            } else
                                virgl_resource_copy_region(
                                    gpu, (*sample_gpu)->id, m, 0, 0, slice, image->gpu->id, mip, 0,
                                    0,
                                    (image->info.imageType == VK_IMAGE_TYPE_3D
                                         ? 0
                                         : view->range.baseArrayLayer) +
                                        slice,
                                    zvk_mip(image->info.extent.width, mip),
                                    zvk_mip(image->info.extent.height, mip), 1);
                        }
                    }
                }
            }
            if (!*sample_view) {
                *sample_view = virgl_handle(gpu);
                uint32_t mapping =
                    swizzle(view->swizzle.r, 0) | (swizzle(view->swizzle.g, 1) << 3) |
                    (swizzle(view->swizzle.b, 2) << 6) | (swizzle(view->swizzle.a, 3) << 9);
                virgl_create_sampler_view_ex(
                    gpu, *sample_view, snapshot ? (*sample_gpu)->id : image->gpu->id, target,
                    image->format, snapshot ? 0 : view->range.baseArrayLayer,
                    (snapshot ? 0 : view->range.baseArrayLayer) + layers - 1,
                    snapshot ? 0 : view->range.baseMipLevel,
                    (snapshot ? 0 : view->range.baseMipLevel) + view->range.levelCount - 1,
                    mapping);
            }
            if (!input && !binding->image_ms && sampler && !sampler->handle) {
                sampler->handle = virgl_handle(gpu);
                virgl_create_sampler_state_ex(gpu, sampler->handle, &sampler->info);
            }
            views[binding->slot] = *sample_view;
            samplers[binding->slot] = input || binding->image_ms || !sampler ? 0 : sampler->handle;
            if (!binding->image_ms && (input || !sampler)) {
                if (!view->compute_sampler) {
                    view->compute_sampler=virgl_handle(gpu);
                    virgl_create_sampler_state(gpu,view->compute_sampler,PIPE_TEX_WRAP_CLAMP_TO_EDGE,
                        PIPE_TEX_WRAP_CLAMP_TO_EDGE,PIPE_TEX_FILTER_NEAREST,PIPE_TEX_MIPFILTER_NONE,
                        PIPE_TEX_FILTER_NEAREST);
                }
                samplers[binding->slot]=view->compute_sampler;
            }
            if (binding->slot >= nsamplers)
                nsamplers = binding->slot + 1;
        }
    }
    if (shader->push_bytes || shader->instance_index || shader->sample_mask_in || shader->sample_mask_out || shader->sample_queries || shader->fixed_depth_bias)
        virgl_set_constants(gpu, stage, (const float *)constants,
                            shader->sample_queries   ? ZVK_ARRAY_SIZE(constants)
                            : shader->instance_index || shader->sample_mask_in || shader->sample_mask_out || shader->fixed_depth_bias ? (ZVK_PUSH + 16) / 4
                                                     : (shader->push_bytes + 15) / 16 * 4);
    virgl_set_sampler_views(gpu, stage, 0, views, nsamplers);
    virgl_bind_sampler_states(gpu, stage, 0, samplers, nsamplers);
    return VK_SUCCESS;
}
/* Integer inputs travel as raw float bits because the graphics frontend
 * preserves SPIR-V integer lanes through TGSI inputs. */
static void vertex_expand(const zvk_format_info *f, const uint8_t *source, uint32_t out[4])
{
    for (unsigned c = 0; c < 4; c++) {
        unsigned bits = f->bits[c];
        uint32_t value = c == 3 ? 1 : 0;
        if (bits) {
            value = 0;
            for (unsigned bit = 0; bit < bits; bit++)
                value |= ((source[(f->shift[c] + bit) / 8] >> ((f->shift[c] + bit) % 8)) & 1u)
                         << bit;
            if (f->type == ZVK_FMT_SINT && bits < 32 && (value & (1u << (bits - 1))))
                value |= UINT32_MAX << bits;
        }
        if (!zvk_format_integer(f->format)) {
            float v = c == 3 ? 1 : 0;
            if (bits && f->type == ZVK_FMT_FLOAT && bits == 32)
                memcpy(&v, &value, 4);
            else if (bits && (f->type == ZVK_FMT_FLOAT || f->type == ZVK_FMT_UFLOAT)) {
                unsigned mantissa = bits - (f->type == ZVK_FMT_FLOAT ? 6 : 5);
                unsigned fraction = value & ((1u << mantissa) - 1);
                unsigned exponent = (value >> mantissa) & 31;
                v = exponent == 31 ? (fraction ? NAN : INFINITY)
                    : exponent ? ldexpf(1.0f + (float)fraction / (1u << mantissa), (int)exponent - 15)
                               : ldexpf((float)fraction, -14 - (int)mantissa);
                if (f->type == ZVK_FMT_FLOAT && (value & (1u << (bits - 1))))
                    v = -v;
            } else if (bits && f->type == ZVK_FMT_SNORM) {
                int32_t signed_value = (int32_t)(value << (32 - bits)) >> (32 - bits);
                v = fmaxf(-1.0f, (float)signed_value / (float)((1u << (bits - 1)) - 1));
            } else if (bits)
                v = (float)value / (float)(bits == 32 ? UINT32_MAX : (1u << bits) - 1);
            memcpy(&value, &v, 4);
        }
        out[c] = value;
    }
}
static VkResult vertex_indices(struct execution *e, const struct zvk_draw *draw, int indexed,
                               int64_t *minimum, int64_t *maximum)
{
    *minimum = draw->first;
    *maximum = (int64_t)draw->first + draw->count - 1;
    if (!indexed)
        return VK_SUCCESS;
    uint32_t bytes = e->index_type == VK_INDEX_TYPE_UINT16 ? 2 : 4;
    if (!e->index || !e->index->memory ||
        !zvk_range(e->index->size, e->index_offset,
                   ((VkDeviceSize)draw->first + draw->count) * bytes))
        return VK_ERROR_INITIALIZATION_FAILED;
    const uint8_t *indices = e->index->memory->data + e->index->offset + e->index_offset +
                             (VkDeviceSize)draw->first * bytes;
    int found = 0;
    for (uint32_t i = 0; i < draw->count; i++) {
        uint32_t index = 0;
        memcpy(&index, indices + (size_t)i * bytes, bytes);
        if (e->pipeline->restart && index == (bytes == 2 ? UINT16_MAX : UINT32_MAX))
            continue;
        int64_t vertex = (int64_t)index + draw->vertex_offset;
        if (!found || vertex < *minimum)
            *minimum = vertex;
        if (!found || vertex > *maximum)
            *maximum = vertex;
        found = 1;
    }
    if (!found)
        *minimum = *maximum = 0;
    return VK_SUCCESS;
}
static VkResult expand_vertices(struct execution *e, unsigned slot, VkBuffer buffer,
                                 const zvk_format_info *format, VkDeviceSize start,
                                 uint32_t source_stride, uint64_t count, uint64_t prefix,
                                 uint64_t copies, int expand)
{
    if (e->vertex_cache[slot].buffer == buffer && e->vertex_cache[slot].start == start &&
        e->vertex_cache[slot].format == format->format &&
        e->vertex_cache[slot].stride == source_stride && e->vertex_cache[slot].count == count &&
        e->vertex_cache[slot].prefix == prefix && e->vertex_cache[slot].copies == copies &&
        e->vertex_cache[slot].generation == e->data_generation)
        return VK_SUCCESS;
    uint32_t element = expand ? 16 : format->bpp;
    if (count > ZVK_MAX_ALLOC / element)
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    uint32_t size = count * element;
    virgl *gpu = e->queue->device->gpu;
    virgl_res **res = e->expanded_vertices + slot;
    if (*res && (*res)->size != size) {
        virgl_res_destroy(gpu, *res);
        *res = NULL;
    }
    if (!*res)
        *res = virgl_res_create(gpu, PIPE_BUFFER, VIRGL_FORMAT_R8_UNORM, 0, size, 1, 1, 0, size);
    if (!*res)
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    memset((*res)->map, 0, size);
    for (uint64_t i = 0; i < copies; i++) {
        const uint8_t *source = buffer->memory->data + buffer->offset + start + i * source_stride;
        uint8_t *destination = (uint8_t *)(*res)->map + (prefix + i) * element;
        if (expand)
            vertex_expand(format, source, (uint32_t *)destination);
        else
            memcpy(destination, source, element);
    }
    if (virgl_res_to_host(gpu, *res, 0, 0, 0, 0, size, 1, 1, 0, 0, 0))
        return VK_ERROR_DEVICE_LOST;
    e->vertex_cache[slot].buffer = buffer;
    e->vertex_cache[slot].start = start;
    e->vertex_cache[slot].format = format->format;
    e->vertex_cache[slot].stride = source_stride;
    e->vertex_cache[slot].count = count;
    e->vertex_cache[slot].prefix = prefix;
    e->vertex_cache[slot].copies = copies;
    e->vertex_cache[slot].generation = e->data_generation;
    return VK_SUCCESS;
}
static VkResult robust_vertex(struct execution *e, const VkVertexInputAttributeDescription *a,
                              const VkVertexInputBindingDescription *binding, VkDeviceSize base,
                              int64_t minimum, int64_t maximum, uint32_t *stride, uint32_t *offset,
                              uint32_t *resource)
{
    VkBuffer b = e->vertices[a->binding];
    const zvk_format_info *f = zvk_get_format_info(a->format);
    int expand =
        !binding->stride || zvk_format_integer(a->format) || (f->bits[0] != 8 && f->bits[0] != 16 && f->bits[0] != 32);
    uint32_t element = expand ? 16 : f->bpp;
    uint64_t valid = 0, start = 0;
    if (zvk_range(b->size, base, a->offset) && zvk_range(b->size, base + a->offset, f->bpp)) {
        start = base + a->offset;
        valid = binding->stride ? (b->size - start - f->bpp) / binding->stride + 1 : 1;
    }
    if (!expand && minimum >= 0 && maximum >= minimum && (uint64_t)maximum < valid) {
        VkResult result = upload_buffer(e, b);
        if (result != VK_SUCCESS)
            return result;
        *resource = b->gpu->id;
        *offset = (uint32_t)start;
        *stride = binding->stride;
        return VK_SUCCESS;
    }
    /* An entirely missing attribute needs a single constant zero, even for
     * UINT_MAX indices. Otherwise preserve original indices and builtins by
     * padding the requested span; negative base-vertex addresses get a prefix. */
    int constant = !binding->stride || !valid || maximum < 0 || minimum >= (int64_t)valid;
    uint64_t prefix = !constant && minimum < 0 ? (uint64_t)-minimum : 0;
    uint64_t count = constant ? 1 : prefix + (maximum > 0 ? (uint64_t)maximum : 0) + 1;
    if (!constant && prefix <= ZVK_MAX_ALLOC / element &&
        valid <= ZVK_MAX_ALLOC / element - prefix && count < prefix + valid)
        count = prefix + valid;
    if (count > ZVK_MAX_ALLOC / element) {
        zvk_error("robust vertex staging exceeds the allocation budget");
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    uint64_t copies = constant                 ? (!binding->stride && valid ? 1 : 0)
                      : valid < count - prefix ? valid
                                               : count - prefix;
    VkResult result = expand_vertices(e, a->location, b, f, start, binding->stride, count,
                                      prefix, copies, expand);
    if (result != VK_SUCCESS)
        return result;
    *resource = e->expanded_vertices[a->location]->id;
    *offset = prefix * element;
    *stride = constant ? 0 : element;
    return VK_SUCCESS;
}
static int attachment_compatible(VkRenderPass a, uint32_t ia, VkRenderPass b, uint32_t ib)
{
    if (ia == VK_ATTACHMENT_UNUSED || ib == VK_ATTACHMENT_UNUSED)
        return ia == ib;
    return a->attachments[ia].format == b->attachments[ib].format &&
           a->attachments[ia].samples == b->attachments[ib].samples;
}
int zvk_passes_compatible(VkRenderPass a, VkRenderPass b)
{
    if (a->nsubpasses != b->nsubpasses)
        return 0;
    for (unsigned s = 0; s < a->nsubpasses; s++) {
        const struct zvk_subpass *x = a->subpasses + s, *y = b->subpasses + s;
        if (x->ncolors != y->ncolors || x->ninputs != y->ninputs ||
            !attachment_compatible(a, x->depth, b, y->depth))
            return 0;
        for (unsigned i = 0; i < x->ncolors; i++) {
            if (!attachment_compatible(a, x->colors[i], b, y->colors[i]))
                return 0;
            /* Resolve references are ignored for single-subpass compatibility. */
            if (a->nsubpasses > 1 && !attachment_compatible(a, x->resolves[i], b, y->resolves[i]))
                return 0;
        }
        for (unsigned i = 0; i < x->ninputs; i++)
            if (!attachment_compatible(a, x->inputs[i], b, y->inputs[i]))
                return 0;
    }
    return 1;
}
static VkResult draw(struct execution *e, const struct zvk_draw *draw, int indexed)
{
    if (!draw->count || !draw->instances)
        return VK_SUCCESS;
    VkPipeline p = e->pipeline;
    VkDevice d = e->queue->device;
    if (!p || !e->pass)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (p->samples == VK_SAMPLE_COUNT_1_BIT && !(p->sample_mask & 1u))
        return VK_SUCCESS;
    if (p->subpass != e->subpass || !zvk_passes_compatible(p->pass, e->pass))
        return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = zvk_pipeline_gpu(p);
    if (r != VK_SUCCESS)
        return r;
    /* Typed MS view transfers use private shaders and constants. Complete
     * them before uploading the application's per-stage resources. */
    const struct zvk_subpass *attachments=e->pass->subpasses+e->subpass;
    for (unsigned i=0;i<=attachments->ncolors;i++) {
        unsigned a=i==attachments->ncolors?attachments->depth:attachments->colors[i];
        if (a!=VK_ATTACHMENT_UNUSED) {
            r=mutable_prepare(e,e->framebuffer->views[a]);
            if (r!=VK_SUCCESS)return r;
        }
    }
    int private_draw = 0;
    const zvk_shader *stages[] = {&p->vs, &p->fs};
    for (unsigned s = 0; s < 2; s++)
        for (unsigned i = 0; i < stages[s]->nbindings; i++)
            private_draw |=
                stages[s]->bindings[i].image_ms && stages[s]->bindings[i].image_kind == 0;
    if (private_draw && e->active_query) {
        r = zvk_query_pause(e->active_query, e->active_query_index);
        if (r != VK_SUCCESS)
            return r;
    }
    r = bind_shader_resources(e, &p->vs, 0, draw->first_instance);
    if (r == VK_SUCCESS)
        r = bind_shader_resources(e, &p->fs, 1, draw->first_instance);
    if (private_draw && e->active_query)
        zvk_query_resume(e->active_query, e->active_query_index);
    if (r != VK_SUCCESS)
        return r;
    r = bind_framebuffer(e);
    if (r != VK_SUCCESS)
        return r;
    virgl *gpu = d->gpu;
    if (getenv("ZVK_DEBUG_RESOURCES"))
        fprintf(stderr, "zvk draw count=%u first=%u indexed=%d instances=%u attrs=%u inputs=%x mask=%x\n",
                draw->count, draw->first, indexed, draw->instances, p->nattrs, p->vs.inputs, p->sample_mask);
    uint32_t stride[ZVK_ATTRS] = {0}, offsets[ZVK_ATTRS] = {0}, resources[ZVK_ATTRS] = {0}, n = 0;
    int64_t minimum = 0, maximum = 0;
    if (d->enabled_features.robustBufferAccess) {
        r = vertex_indices(e, draw, indexed, &minimum, &maximum);
        if (r != VK_SUCCESS)
            return r;
    }
    for (uint32_t i = 0; !p->vs.vertex_pulling && i < p->nattrs; i++) {
        VkVertexInputAttributeDescription *a = p->attrs + i;
        uint32_t slot = p->vs.input_map[a->location];
        if (!(p->vs.inputs & (1u << a->location)))
            continue;
        const VkVertexInputBindingDescription *binding = NULL;
        for (uint32_t j = 0; j < p->nbindings; j++)
            if (p->bindings[j].binding == a->binding)
                binding = p->bindings + j;
        VkBuffer b = e->vertices[a->binding];
        if (!binding || !b || !b->memory)
            return VK_ERROR_INITIALIZATION_FAILED;
        VkDeviceSize base = e->vertex_offsets[a->binding];
        if (getenv("ZVK_DEBUG_RESOURCES") && zvk_range(b->size, base + a->offset, 16)) {
            float values[4];
            memcpy(values, b->memory->data + b->offset + base + a->offset, sizeof values);
            fprintf(stderr, "zvk attribute location=%u binding=%u format=%u offset=%llu stride=%u first=(%g,%g,%g,%g)\n",
                    a->location, a->binding, a->format, (unsigned long long)(base + a->offset),
                    binding->stride, values[0], values[1], values[2], values[3]);
        }
        if (binding->inputRate == VK_VERTEX_INPUT_RATE_INSTANCE)
            base += (VkDeviceSize)draw->first_instance * binding->stride;
        if (d->enabled_features.robustBufferAccess) {
            int instance = binding->inputRate == VK_VERTEX_INPUT_RATE_INSTANCE;
            r = robust_vertex(e, a, binding, base, instance ? 0 : minimum,
                              instance ? (int64_t)draw->instances - 1 : maximum, stride + slot,
                              offsets + slot, resources + slot);
            if (r != VK_SUCCESS)
                return r;
            if (slot >= n)
                n = slot + 1;
            continue;
        }
        if (base >= b->size)
            return VK_ERROR_INITIALIZATION_FAILED;
        stride[slot] = binding->stride;
        const zvk_format_info *f = zvk_get_format_info(a->format);
        if (!binding->stride || zvk_format_integer(a->format) ||
            (f->bits[0] != 8 && f->bits[0] != 16 && f->bits[0] != 32)) {
            if (!zvk_range(b->size, base, a->offset) ||
                !zvk_range(b->size, base + a->offset, f->bpp))
                return VK_ERROR_INITIALIZATION_FAILED;
            VkDeviceSize start = base + a->offset;
            uint64_t count = binding->stride ? (b->size - start - f->bpp) / binding->stride + 1 : 1;
            if (count > ZVK_MAX_ALLOC / 16)
                return VK_ERROR_OUT_OF_DEVICE_MEMORY;
            r = expand_vertices(e, slot, b, f, start, binding->stride, count, 0, count, 1);
            if (r != VK_SUCCESS)
                return r;
            resources[slot] = e->expanded_vertices[slot]->id;
            offsets[slot] = 0;
            stride[slot] = binding->stride ? 16 : 0;
        } else {
            r = upload_buffer(e, b);
            if (r != VK_SUCCESS)
                return r;
            offsets[slot] = (uint32_t)(base + a->offset);
            resources[slot] = b->gpu->id;
        }
        if (slot >= n)
            n = slot + 1;
    }
    virgl_set_vertex_buffers(gpu, n, stride, offsets, resources);
    virgl_bind_object(gpu, p->h_ve, VIRGL_OBJECT_VERTEX_ELEMENTS);
    if (indexed) {
        if (!e->index)
            return VK_ERROR_INITIALIZATION_FAILED;
        uint32_t bytes = e->index_type == VK_INDEX_TYPE_UINT16 ? 2 : 4;
        if (!zvk_range(e->index->size, e->index_offset,
                       ((VkDeviceSize)draw->first + draw->count) * bytes))
            return VK_ERROR_INITIALIZATION_FAILED;
        r = upload_buffer(e, e->index);
        if (r != VK_SUCCESS)
            return r;
        virgl_set_index_buffer(gpu, e->index->gpu->id, bytes,
                               (uint32_t)(e->index_offset + (VkDeviceSize)draw->first * bytes));
    }
    virgl_bind_shader(gpu, p->h_vs, PIPE_SHADER_VERTEX);
    virgl_bind_shader(gpu, p->h_fs, PIPE_SHADER_FRAGMENT);
    virgl_bind_shader(gpu, 0, PIPE_SHADER_GEOMETRY);
    virgl_bind_object(gpu, p->h_blend, VIRGL_OBJECT_BLEND);
    VkPipelineRasterizationStateCreateInfo raster = p->raster;
    if ((p->dynamic_line_width && !e->have_line_width) ||
        (p->dynamic_depth_bias && raster.depthBiasEnable && !e->have_depth_bias))
        return VK_ERROR_INITIALIZATION_FAILED;
    if (p->dynamic_line_width)
        raster.lineWidth = e->line_width;
    if (p->dynamic_depth_bias && raster.depthBiasEnable) {
        raster.depthBiasConstantFactor = e->depth_bias[0];
        raster.depthBiasClamp = e->depth_bias[1];
        raster.depthBiasSlopeFactor = e->depth_bias[2];
    }
    if (!d->enabled_features.depthBiasClamp)
        raster.depthBiasClamp=0;
    if (p->dynamic_line_width || p->dynamic_depth_bias) {
        if (e->dynamic_raster)
            virgl_destroy_object(gpu, e->dynamic_raster, VIRGL_OBJECT_RASTERIZER);
        e->dynamic_raster = virgl_handle(gpu);
        uint32_t words[VIRGL_OBJ_RS_SIZE - 1];
        zvk_raster(p, &raster, words);
        virgl_create_rasterizer(gpu, e->dynamic_raster, words);
        virgl_bind_object(gpu, e->dynamic_raster, VIRGL_OBJECT_RASTERIZER);
    } else
        virgl_bind_object(gpu, p->h_rs, VIRGL_OBJECT_RASTERIZER);
    VkPipelineDepthStencilStateCreateInfo ds = p->depth;
    uint32_t needed = VK_STENCIL_FACE_FRONT_AND_BACK & ~p->raster.cullMode;
    if (ds.stencilTestEnable) {
        int dynamic[] = {p->dynamic_stencil_compare, p->dynamic_stencil_write,
                         p->dynamic_stencil_reference};
        for (int k = 0; k < 3; k++)
            if (dynamic[k] && (e->have_stencil[k] & needed) != needed)
                return VK_ERROR_INITIALIZATION_FAILED;
        VkStencilOpState *faces[] = {&ds.front, &ds.back};
        for (int k = 0; k < 2; k++) {
            if (dynamic[0])
                faces[k]->compareMask = e->stencil[0][k];
            if (dynamic[1])
                faces[k]->writeMask = e->stencil[1][k];
            if (dynamic[2])
                faces[k]->reference = e->stencil[2][k];
        }
    }
    if (p->dynamic_stencil_compare || p->dynamic_stencil_write) {
        if (e->dynamic_dsa)
            virgl_destroy_object(gpu, e->dynamic_dsa, VIRGL_OBJECT_DSA);
        e->dynamic_dsa = virgl_handle(gpu);
        uint32_t words[VIRGL_OBJ_DSA_SIZE - 1];
        zvk_dsa(p, &ds, words);
        virgl_create_dsa(gpu, e->dynamic_dsa, words);
        virgl_bind_object(gpu, e->dynamic_dsa, VIRGL_OBJECT_DSA);
    } else
        virgl_bind_object(gpu, p->h_dsa, VIRGL_OBJECT_DSA);
    virgl_set_stencil_ref(gpu, ds.front.reference, ds.back.reference);
    VkViewport vp = p->dynamic_viewport ? e->viewport : p->viewport;
    VkRect2D sc = p->dynamic_scissor ? e->scissor : p->scissor;
    if ((p->dynamic_viewport && !e->have_viewport) || (p->dynamic_scissor && !e->have_scissor) ||
        vp.width <= 0 || vp.height <= 0)
        return VK_ERROR_INITIALIZATION_FAILED;
    virgl_set_viewport(gpu, vp.x, vp.y, vp.width, vp.height, vp.minDepth, vp.maxDepth, 0);
    int64_t x0 = sc.offset.x > e->area.offset.x ? sc.offset.x : e->area.offset.x,
            y0 = sc.offset.y > e->area.offset.y ? sc.offset.y : e->area.offset.y;
    int64_t x1 = (int64_t)sc.offset.x + sc.extent.width,
            y1 = (int64_t)sc.offset.y + sc.extent.height;
    if (x1 > (int64_t)e->area.offset.x + e->area.extent.width)
        x1 = (int64_t)e->area.offset.x + e->area.extent.width;
    if (y1 > (int64_t)e->area.offset.y + e->area.extent.height)
        y1 = (int64_t)e->area.offset.y + e->area.extent.height;
    if (x0 < 0)
        x0 = 0;
    if (y0 < 0)
        y0 = 0;
    if (x1 < x0 || y1 < y0)
        return VK_SUCCESS;
    virgl_set_scissor(gpu, (uint32_t)x0, (uint32_t)y0, (uint32_t)x1, (uint32_t)y1);
    virgl_set_blend_color(gpu, p->dynamic_blend && e->have_blend ? e->blend : p->blend_constants);
    virgl_set_sample_mask(gpu, p->sample_mask);
    static const uint32_t topology[] = {PIPE_PRIM_POINTS,         PIPE_PRIM_LINES,
                                        PIPE_PRIM_LINE_STRIP,     PIPE_PRIM_TRIANGLES,
                                        PIPE_PRIM_TRIANGLE_STRIP, PIPE_PRIM_TRIANGLE_FAN};
    struct virgl_draw_info info = {.mode = topology[p->topology],
                                   .start = indexed ? 0 : draw->first,
                                   .count = draw->count,
                                   .indexed = indexed,
                                   .instance_count = draw->instances,
                                   .index_bias = draw->vertex_offset,
                                   .primitive_restart = p->restart,
                                   .restart_index =
                                       e->index_type == VK_INDEX_TYPE_UINT16 ? 65535 : UINT32_MAX,
                                   .min_index = 0,
                                   .max_index = UINT32_MAX};
    virgl_draw_ex(gpu, &info);
    const struct zvk_subpass *subpass = e->pass->subpasses + e->subpass;
    for (unsigned i = 0; i <= subpass->ncolors; i++) {
        unsigned attachment = i == subpass->ncolors ? subpass->depth : subpass->colors[i];
        if (attachment != VK_ATTACHMENT_UNUSED) {
            VkImageView view = e->framebuffer->views[attachment];
            VkImage written=view_image(view);
            written->transfer_valid[view->range.baseMipLevel * written->info.arrayLayers +
                                    view->range.baseArrayLayer] = 0;
            r=mutable_commit(e,view);
            if (r!=VK_SUCCESS)return r;
        }
    }
    return VK_SUCCESS;
}
static VkResult bind_sets(struct execution *e, const struct zvk_bind_sets *p, int compute)
{
    uint32_t n = 0;
    VkDescriptorSet *sets = compute ? e->compute_sets : e->sets;
    const uint32_t **dynamic=compute?e->compute_dynamic:e->dynamic;
    for (uint32_t i=0;i<p->count;i++) {
        uint32_t index=p->first+i;VkDescriptorSet set=p->sets[i];
        sets[index]=set;
        if (set->layout->ndynamic>p->ndynamic-n)return VK_ERROR_INITIALIZATION_FAILED;
        dynamic[index]=p->dynamic+n;n+=set->layout->ndynamic;
    }
    return n == p->ndynamic ? VK_SUCCESS : VK_ERROR_INITIALIZATION_FAILED;
}
static VkResult dispatch(struct execution *e, const struct zvk_dispatch *p)
{
    VkPipeline pipeline = e->compute_pipeline;
    if (!pipeline || e->pass)
        return VK_ERROR_INITIALIZATION_FAILED;
    uint32_t grid[3];
    memcpy(grid, p->groups, sizeof grid);
    if (p->indirect) {
        if (!p->indirect->memory)
            return VK_ERROR_INITIALIZATION_FAILED;
        memcpy(grid, p->indirect->memory->data + p->indirect->offset + p->offset, sizeof grid);
    }
    for (unsigned i = 0; i < 3; i++)
        if (grid[i] > 65535)
            return VK_ERROR_FEATURE_NOT_PRESENT;
    if (!grid[0] || !grid[1] || !grid[2])
        return VK_SUCCESS;
    virgl *gpu = e->queue->device->gpu;
    zvk_shader *shader = &pipeline->cs;
    struct virgl_metal_binding bindings[ZVK_RESOURCES + 1] = {0};
    VkDeviceMemory memory[ZVK_RESOURCES] = {0}, backing[ZVK_RESOURCES] = {0};
    VkImageView views[ZVK_RESOURCES] = {0};
    struct compute_metadata metadata={0};
    for (unsigned i=0;shader->sampling && i<shader->nsamplers;i++) {
        zvk_shader_binding *b=shader->samplers+i;
        VkDescriptorSet set=e->compute_sets[b->set];
        if (!set || !set->descriptors[b->index].sampler)
            return VK_ERROR_INITIALIZATION_FAILED;
        compute_sampler(&metadata,ZVK_RESOURCES+i,set->descriptors[b->index].sampler,NULL);
    }
    int written[ZVK_RESOURCES] = {0};
    uint32_t unique = 0;
    /* Collect image contents before uploading any shared allocation: image
     * views and buffer views may alias the same backing and must see writes. */
    for (unsigned i = 0; i < shader->nbindings; i++) {
        zvk_shader_binding *b = shader->bindings + i;
        VkDescriptorSet set = e->compute_sets[b->set];
        if (!set)
            return VK_ERROR_INITIALIZATION_FAILED;
        zvk_descriptor *desc = set->descriptors+b->index;
        VkDeviceMemory mem = NULL;
        VkFormat format = VK_FORMAT_UNDEFINED;
        uint64_t off = 0, range = 0;
        unsigned write = b->type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ||
                         b->type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ||
                         b->type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
        if (b->type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
            b->type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) {
            VkImageView view = desc->image;
            if (!view || view->device != pipeline->device ||
                (b->image_ms && (view->image->info.samples == VK_SAMPLE_COUNT_1_BIT || view->range.levelCount != 1)) ||
                !(view->image->info.usage & VK_IMAGE_USAGE_SAMPLED_BIT) ||
                (desc->layout != VK_IMAGE_LAYOUT_GENERAL &&
                 desc->layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
                 desc->layout != VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL) ||
                (shader->sampling && b->type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER && !desc->sampler) ||
                (b->image_ms && (!!b->image_array != (view->target == PIPE_TEXTURE_2D_ARRAY))))
                return VK_ERROR_INITIALIZATION_FAILED;
            VkSampler sampler=shader->sampling?desc->sampler:NULL;
            if (b->separate_sampler && shader->sampling) {
                VkDescriptorSet ss=e->compute_sets[b->sampler_set];
                if (!ss) return VK_ERROR_INITIALIZATION_FAILED;
                sampler=ss->descriptors[b->sampler_index].sampler;
            }
            compute_sampler(&metadata,i,sampler,view);
            if (!b->image_ms) {
                VkResult r=compute_sampled_image(view,i,&metadata,&mem,&off,&range);
                if (r!=VK_SUCCESS) return r;
            } else {
            for (unsigned layer = view->range.baseArrayLayer;
                 layer < view->range.baseArrayLayer + view->range.layerCount; layer++)
                if (!zvk_layout_is(view->image, view->range.baseMipLevel, layer, desc->layout))
                    return VK_ERROR_INITIALIZATION_FAILED;
            VkResult r = mutable_prepare(e,view);
            if (r == VK_SUCCESS)
                r = zvk_sample_snapshot(view, &mem);
            if (r != VK_SUCCESS)
                return r;
            range = mem->size;
            format = b->image_kind == 0   ? VK_FORMAT_R32G32B32A32_UINT
                     : b->image_kind == 1 ? VK_FORMAT_R32G32B32A32_SINT
                                          : VK_FORMAT_R32G32B32A32_SFLOAT;
            metadata.images[i][0] =
                zvk_mip(view->image->info.extent.width, view->range.baseMipLevel);
            metadata.images[i][1] =
                zvk_mip(view->image->info.extent.height, view->range.baseMipLevel);
            metadata.images[i][2] = view->range.layerCount * view->image->info.samples;
            }
        } else if (b->type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) {
            VkImageView view = desc->image;
            if (!view || view->device != pipeline->device || view->range.levelCount != 1 ||
                desc->layout != VK_IMAGE_LAYOUT_GENERAL ||
                !(view->image->info.usage & VK_IMAGE_USAGE_STORAGE_BIT) ||
                view->target != (b->image_dim == SpvDim1D
                                     ? (b->image_array ? PIPE_TEXTURE_1D_ARRAY : PIPE_TEXTURE_1D)
                                 : b->image_dim == SpvDim3D   ? PIPE_TEXTURE_3D
                                 : b->image_dim == SpvDimCube ? PIPE_TEXTURE_CUBE
                                 : b->image_array             ? PIPE_TEXTURE_2D_ARRAY
                                                              : PIPE_TEXTURE_2D))
                return VK_ERROR_INITIALIZATION_FAILED;
            const VkComponentSwizzle *swizzle = &view->swizzle.r;
            for (unsigned k = 0; k < 4; k++)
                if (swizzle[k] != VK_COMPONENT_SWIZZLE_IDENTITY &&
                    swizzle[k] != VK_COMPONENT_SWIZZLE_R + k)
                    return VK_ERROR_INITIALIZATION_FAILED;
            views[i] = view;
            VkImage image = view->image;
            if (!image->memory)
                return VK_ERROR_INITIALIZATION_FAILED;
            unsigned mip = view->range.baseMipLevel, base = view->range.baseArrayLayer;
            metadata.images[i][0] = zvk_mip(image->info.extent.width, mip);
            metadata.images[i][1] = zvk_mip(image->info.extent.height, mip);
            metadata.images[i][2] = image->info.imageType == VK_IMAGE_TYPE_3D
                                        ? zvk_mip(image->info.extent.depth, mip)
                                        : view->range.layerCount;
            size_t layer_bytes = (size_t)metadata.images[i][0] * metadata.images[i][1] *
                                 image->bpp * zvk_mip(image->info.extent.depth, mip);
            for (unsigned l = base; l < base + view->range.layerCount; l++) {
                if (!zvk_layout_is(image, mip, l, VK_IMAGE_LAYOUT_GENERAL))
                    return VK_ERROR_INITIALIZATION_FAILED;
                VkResult r = image_read(image, mip, l);
                if (r != VK_SUCCESS)
                    return r;
            }
            size_t begin = zvk_image_offset(image, mip, base);
            range = layer_bytes * view->range.layerCount;
            memcpy(image->memory->data + image->offset + begin,
                   (uint8_t *)zvk_image_map(image) + begin, range);
            mem = image->memory;
            off = image->offset + begin;
            format = view->format;
        } else if (b->type == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER ||
                   b->type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) {
            VkBufferView view = desc->texel;
            if (!view || view->device != pipeline->device || !view->buffer->memory ||
                !(view->buffer->usage & (write ? VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT
                                               : VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT)))
                return VK_ERROR_INITIALIZATION_FAILED;
            mem = view->buffer->memory;
            off = view->buffer->offset + view->offset;
            range = view->range;
            format = view->format;
            metadata.images[i][0] = range / zvk_bpp(format);
            metadata.images[i][1] = metadata.images[i][2] = 1;
        } else {
            VkBuffer buffer = desc->buffer;
            if (!buffer || !buffer->memory)
                return VK_ERROR_INITIALIZATION_FAILED;
            off = desc->offset;
            if (desc->type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC ||
                desc->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC) {
                uint32_t extra = e->compute_dynamic[b->set][b->dynamic_index];
                if (off > UINT64_MAX - extra)
                    return VK_ERROR_INITIALIZATION_FAILED;
                off += extra;
            }
            range = desc->range == VK_WHOLE_SIZE ? (off <= buffer->size ? buffer->size - off : 0)
                                                 : desc->range;
            if (!range || (off & 3) || !zvk_range(buffer->size, off, range))
                return VK_ERROR_INITIALIZATION_FAILED;
            mem = buffer->memory;
            off += buffer->offset;
        }
        if (format != VK_FORMAT_UNDEFINED) {
            uint32_t kind, components;
            /* The descriptor's actual byte layout bounds every GPU read.
             * Mismatched SPIR-V format annotations have undefined texel values,
             * but must not make the resource transport access invalid memory. */
            if (!zvk_texel_format(format,&kind,&components))
                return VK_ERROR_INITIALIZATION_FAILED;
            const zvk_format_info *f = zvk_get_format_info(format);
            /* Metal bindings are word-aligned; narrow-format mips need a
             * descriptor-local byte bias without changing alias addresses. */
            metadata.images[i][3] = (uint32_t)off & 3u;
            metadata.images[i][4] = f->bpp;
            metadata.images[i][5] =
                f->type | (b->image_ms ? desc->image->image->info.samples << 8 : 0);
            for (unsigned c = 0; c < 4; c++) {
                metadata.images[i][6] |= (uint32_t)f->bits[c] << (8 * c);
                metadata.images[i][7] |= (uint32_t)f->shift[c] << (8 * c);
            }
        }
        if (!zvk_range(mem->size, off, range))
            return VK_ERROR_INITIALIZATION_FAILED;
        unsigned m = 0;
        while (m < unique && memory[m] != mem)
            m++;
        if (m == unique)
            memory[unique++] = mem;
        written[m] |= write;
        backing[i] = mem;
        bindings[i] = (struct virgl_metal_binding){0, (uint32_t)off, (uint32_t)range, write};
    }
    virgl_res *info = NULL, *arena = NULL;
    uint32_t arena_offsets[ZVK_RESOURCES] = {0}, arena_size = 0;
    struct virgl_metal_binding packed[2], direct[29];
    struct virgl_metal_binding *physical = direct;
    uint32_t count = shader->nbindings;
    VkResult result = VK_SUCCESS;
    if (shader->packed_buffers) {
        /* One range per descriptor, one arena region per allocation: aliases
         * keep the same address and shader bounds remain descriptor-relative. */
        uint64_t total = 0;
        for (unsigned m = 0; m < unique; m++) {
            total = (total + 15) & ~15ull;
            arena_offsets[m] = total;
            total += memory[m]->size;
            if (total > ZVK_MAX_ALLOC)
                return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        arena_size = total;
        arena = virgl_res_create(gpu, PIPE_BUFFER, VIRGL_FORMAT_R8_UNORM, VIRGL_BIND_VERTEX_BUFFER,
                                 arena_size, 1, 1, 0, arena_size);
        if (!arena)
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        for (unsigned m = 0; m < unique; m++)
            memcpy((uint8_t *)arena->map + arena_offsets[m], memory[m]->data, memory[m]->size);
        for (unsigned i = 0; i < shader->nbindings; i++) {
            unsigned m = 0;
            while (memory[m] != backing[i])
                m++;
            metadata.ranges[i][0] = arena_offsets[m] + (bindings[i].offset & ~3u);
            metadata.ranges[i][1] = bindings[i].length + (bindings[i].offset & 3u);
        }
        if (virgl_res_to_host(gpu, arena, 0, 0, 0, 0, arena_size, 1, 1, 0, 0, 0)) {
            result = VK_ERROR_DEVICE_LOST;
            goto done;
        }
        packed[0] = (struct virgl_metal_binding){arena->id, 0, arena_size, 1};
        physical = packed;
        count = 1;
    } else {
        for (unsigned m = 0; m < unique; m++) {
            VkDeviceMemory mem = memory[m];
            if (!mem->compute_gpu)
                mem->compute_gpu =
                    virgl_res_create(gpu, PIPE_BUFFER, VIRGL_FORMAT_R8_UNORM,
                                     VIRGL_BIND_VERTEX_BUFFER, mem->size, 1, 1, 0, mem->size);
            if (!mem->compute_gpu) {
                result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
                goto done;
            }
            memcpy(mem->compute_gpu->map, mem->data, mem->size);
            if (virgl_res_to_host(gpu, mem->compute_gpu, 0, 0, 0, 0, mem->size, 1, 1, 0, 0, 0)) {
                result = VK_ERROR_DEVICE_LOST;
                goto done;
            }
        }
        for (unsigned i = 0; i < shader->nbindings; i++) {
            bindings[i].resource = backing[i]->compute_gpu->id;
            direct[i] = bindings[i];
            direct[i].offset &= ~3u;
            direct[i].length += bindings[i].offset & 3u;
        }
    }
    if (shader->image_metadata) {
        info = virgl_res_create(gpu, PIPE_BUFFER, VIRGL_FORMAT_R8_UNORM, VIRGL_BIND_VERTEX_BUFFER,
                                sizeof metadata, 1, 1, 0, sizeof metadata);
        if (!info) {
            result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
            goto done;
        }
        memcpy(info->map, &metadata, sizeof metadata);
        if (virgl_res_to_host(gpu, info, 0, 0, 0, 0, sizeof metadata, 1, 1, 0, 0, 0)) {
            result = VK_ERROR_DEVICE_LOST;
            goto done;
        }
        physical[count++] = (struct virgl_metal_binding){info->id, 0, sizeof metadata, 0};
    }
    virgl_metal_dispatch(gpu, pipeline->h_cs, shader->local_size, grid, count, physical,
                         e->push[2]);
    if (virgl_flush_fenced(gpu)) {
        result = VK_ERROR_DEVICE_LOST;
        goto done;
    }
    if (arena && virgl_res_from_host(gpu, arena, 0, 0, 0, 0, arena_size, 1, 1, 0, 0, 0)) {
        result = VK_ERROR_DEVICE_LOST;
        goto done;
    }
    for (unsigned m = 0; m < unique; m++) {
        if (!written[m])
            continue;
        VkDeviceMemory mem = memory[m];
        if (arena)
            memcpy(mem->data, (uint8_t *)arena->map + arena_offsets[m], mem->size);
        else {
            if (virgl_res_from_host(gpu, mem->compute_gpu, 0, 0, 0, 0, mem->size, 1, 1, 0, 0, 0)) {
                result = VK_ERROR_DEVICE_LOST;
                goto done;
            }
            memcpy(mem->data, mem->compute_gpu->map, mem->size);
        }
    }
    for (unsigned i = 0; i < shader->nbindings; i++) {
        if (!views[i] || !bindings[i].write)
            continue;
        VkImageView view = views[i];
        VkImage image = view->image;
        unsigned mip = view->range.baseMipLevel, base = view->range.baseArrayLayer;
        size_t begin = zvk_image_offset(image, mip, base);
        memcpy((uint8_t *)zvk_image_map(image) + begin, image->memory->data + bindings[i].offset,
               bindings[i].length);
        for (unsigned l = base; l < base + view->range.layerCount; l++) {
            result = image_write(image, mip, l);
            if (result != VK_SUCCESS)
                goto done;
        }
    }
done:
    if (arena)
        virgl_res_destroy(gpu, arena);
    if (info)
        virgl_res_destroy(gpu, info);
    return result;
}
static VkResult image_buffer(const struct zvk_image_buffer *p, int to_image)
{
    VkImage image = p->image;
    VkBuffer buffer = p->buffer;
    const VkBufferImageCopy *r = &p->region;
    uint32_t mip = r->imageSubresource.mipLevel, layer = r->imageSubresource.baseArrayLayer;
    if (!buffer->memory || !zvk_layout_is(image, mip, layer, p->layout))
        return VK_ERROR_INITIALIZATION_FAILED;
    /* Uploading a subrectangle must preserve pixels last written by the GPU. */
    int full = image->info.format != VK_FORMAT_D24_UNORM_S8_UINT && !r->imageOffset.x &&
               !r->imageOffset.y &&
               r->imageExtent.width == zvk_mip(image->info.extent.width, mip) &&
               r->imageExtent.height == zvk_mip(image->info.extent.height, mip) &&
               !r->imageOffset.z && r->imageExtent.depth == zvk_mip(image->info.extent.depth, mip);
    VkResult result = to_image && full ? zvk_image_gpu(image) : image_read(image, mip, layer);
    if (result != VK_SUCCESS)
        return result;
    uint32_t bpp = zvk_aspect_bpp(image, r->imageSubresource.aspectMask);
    unsigned block = zvk_block(image->info.format);
    uint32_t image_width =
        zvk_block_count(image->info.format, zvk_mip(image->info.extent.width, mip));
    uint32_t columns = zvk_block_count(image->info.format, r->imageExtent.width);
    uint32_t rows = zvk_block_count(image->info.format, r->imageExtent.height);
    size_t pitch =
        (size_t)zvk_block_count(image->info.format,
                                r->bufferRowLength ? r->bufferRowLength : r->imageExtent.width) *
        bpp;
    uint8_t *bytes = buffer->memory->data + buffer->offset + r->bufferOffset;
    uint8_t *pixels = (uint8_t *)zvk_image_map(image) + zvk_image_offset(image, mip, layer) +
                      ((size_t)(uint32_t)r->imageOffset.y / block * image_width +
                       (uint32_t)r->imageOffset.x / block) *
                          image->bpp;
    pixels += (VkDeviceSize)(uint32_t)r->imageOffset.z * image_width *
              zvk_block_count(image->info.format, zvk_mip(image->info.extent.height, mip)) * image->bpp;
    size_t buffer_slice =
        pitch * zvk_block_count(image->info.format, r->bufferImageHeight ? r->bufferImageHeight
                                                                         : r->imageExtent.height);
    size_t image_slice =
        (size_t)image_width *
        zvk_block_count(image->info.format, zvk_mip(image->info.extent.height, mip)) * image->bpp;
    for (uint32_t z = 0; z < r->imageExtent.depth; z++)
        for (uint32_t row = 0; row < rows; row++) {
            void *b = bytes + (size_t)z * buffer_slice + row * pitch,
                 *im = pixels + (size_t)z * image_slice + (size_t)row * image_width * image->bpp;
            if (image->info.format == VK_FORMAT_D24_UNORM_S8_UINT) {
                for (uint32_t x = 0; x < r->imageExtent.width; x++) {
                    uint32_t packed;
                    memcpy(&packed, (uint8_t *)im + x * 4, 4);
                    if (r->imageSubresource.aspectMask == VK_IMAGE_ASPECT_STENCIL_BIT) {
                        if (to_image)
                            packed = (packed & 0xffffffu) | ((uint32_t)((uint8_t *)b)[x] << 24);
                        else
                            ((uint8_t *)b)[x] = (uint8_t)(packed >> 24);
                    } else {
                        if (to_image) {
                            uint32_t depth;
                            memcpy(&depth, (uint8_t *)b + x * 4, 4);
                            packed = (packed & 0xff000000u) | (depth & 0xffffffu);
                        } else {
                            uint32_t depth = packed & 0xffffffu;
                            memcpy((uint8_t *)b + x * 4, &depth, 4);
                        }
                    }
                    if (to_image)
                        memcpy((uint8_t *)im + x * 4, &packed, 4);
                }
            } else if (to_image)
                memcpy(im, b, (size_t)columns * image->bpp);
            else
                memcpy(b, im, (size_t)columns * image->bpp);
        }
    return to_image ? image_write(image, mip, layer) : VK_SUCCESS;
}
static VkResult execute(VkQueue queue, VkCommandBuffer command, uint32_t depth,
                        const struct execution *parent)
{
    if (depth > 16 || command->state != ZVK_PENDING || !command->pending)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct execution *e = calloc(1, sizeof *e);
    if (!e)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    e->queue = queue;
    if (command->level == VK_COMMAND_BUFFER_LEVEL_SECONDARY && command->in_pass) {
        if (!parent || !parent->pass || command->subpass != parent->subpass ||
            !zvk_passes_compatible(command->pass, parent->pass) ||
            (command->inherited_framebuffer && command->inherited_framebuffer != parent->framebuffer)) {
            free(e);
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        e->pass = parent->pass;
        e->framebuffer = parent->framebuffer;
        e->subpass = parent->subpass;
        e->area = parent->area;
        e->begin = parent->begin;
    }
    VkResult result = VK_SUCCESS;
    for (size_t offset = 0; offset < command->size;) {
        const struct zvk_command *header = (const void *)(command->commands + offset);
        const void *data = header + 1;
        offset += header->size;
        switch ((enum zvk_op)header->op) {
        case ZVK_DISPATCH:
        case ZVK_COPY_QUERY:
        case ZVK_COPY_BUFFER:
        case ZVK_FILL_BUFFER:
        case ZVK_UPDATE_BUFFER:
        case ZVK_COPY_IMAGE_BUFFER:
        case ZVK_COPY_BUFFER_IMAGE:
        case ZVK_CLEAR_IMAGE:
        case ZVK_COPY_IMAGE:
        case ZVK_BARRIER:
        case ZVK_EXECUTE:
        case ZVK_WAIT_EVENTS:
            e->nuploaded = 0;
            e->data_generation++;
            break;
        default:
            break;
        }
        switch ((enum zvk_op)header->op) {
        case ZVK_BEGIN_PASS:
            result = begin_pass(e, data);
            break;
        case ZVK_NEXT_SUBPASS:
            if (!e->pass || e->subpass + 1 >= e->pass->nsubpasses) {
                result = VK_ERROR_INITIALIZATION_FAILED;
                break;
            }
            result = finish_subpass(e);
            if (result == VK_SUCCESS) {
                e->subpass++;
                result = start_subpass(e);
            }
            break;
        case ZVK_END_PASS:
            if (!e->pass || e->subpass + 1 != e->pass->nsubpasses) {
                result = VK_ERROR_INITIALIZATION_FAILED;
                break;
            }
            result = finish_subpass(e);
            if (result != VK_SUCCESS)
                break;
            /* Unreferenced attachments ignore load/store operations, but
             * still perform their initial-to-final layout transition. */
            for (uint32_t i = 0; i < e->pass->count; i++) {
                VkImageView view = e->framebuffer->views[i];
                VkImageSubresourceRange range = framebuffer_range(e->framebuffer, view);
                zvk_layout_set(view->image, &range, e->pass->attachments[i].finalLayout);
            }
            e->pass = NULL;
            e->framebuffer = NULL;
            break;
        case ZVK_BIND_COMPUTE:
            e->compute_pipeline = *(const VkPipeline *)data;
            break;
        case ZVK_DISPATCH:
            result = dispatch(e, data);
            break;
        case ZVK_BIND_COMPUTE_SETS:
            result = bind_sets(e, data, 1);
            break;
        case ZVK_BIND_PIPELINE:
            e->pipeline = *(const VkPipeline *)data;
            break;
        case ZVK_VIEWPORT:
            e->viewport = *(const VkViewport *)data;
            e->have_viewport = 1;
            break;
        case ZVK_SCISSOR:
            e->scissor = *(const VkRect2D *)data;
            e->have_scissor = 1;
            break;
        case ZVK_BIND_VERTEX: {
            const struct zvk_bind_vertex *p = data;
            for (uint32_t i = 0; i < p->count; i++) {
                e->vertices[p->first + i] = p->buffers[i];
                e->vertex_offsets[p->first + i] = p->offsets[i];
            }
            break;
        }
        case ZVK_BIND_INDEX: {
            const struct zvk_bind_index *p = data;
            e->index = p->buffer;
            e->index_offset = p->offset;
            e->index_type = p->type;
            break;
        }
        case ZVK_BIND_SETS:
            result = bind_sets(e, data, 0);
            break;
        case ZVK_PUSH_CONSTANTS: {
            const struct zvk_push *p = data;
            if (p->stages & VK_SHADER_STAGE_COMPUTE_BIT)
                memcpy((uint8_t *)e->push[2] + p->offset, p->data, p->size);
            if (p->stages & VK_SHADER_STAGE_VERTEX_BIT)
                memcpy((uint8_t *)e->push[0] + p->offset, p->data, p->size);
            if (p->stages & VK_SHADER_STAGE_FRAGMENT_BIT)
                memcpy((uint8_t *)e->push[1] + p->offset, p->data, p->size);
            break;
        }
        case ZVK_BLEND_CONSTANTS:
            memcpy(e->blend, data, sizeof e->blend);
            e->have_blend = 1;
            break;
        case ZVK_DRAW:
        case ZVK_DRAW_INDEXED:
            result = draw(e, data, header->op == ZVK_DRAW_INDEXED);
            break;
        case ZVK_STENCIL: {
            const struct zvk_stencil *p = data;
            for (int face = 0; face < 2; face++)
                if (p->faces & (1u << face)) {
                    e->stencil[p->kind][face] = p->value;
                    e->have_stencil[p->kind] |= 1u << face;
                }
            break;
        }
        case ZVK_LINE_WIDTH:
            e->line_width = *(const float *)data;
            e->have_line_width = 1;
            break;
        case ZVK_DEPTH_BIAS:
            memcpy(e->depth_bias, data, sizeof e->depth_bias);
            e->have_depth_bias = 1;
            break;
        case ZVK_DRAW_INDIRECT: {
            const struct zvk_indirect *p = data;
            if (!p->count)
                break;
            if (!p->buffer->memory) {
                result = VK_ERROR_INITIALIZATION_FAILED;
                break;
            }
            for (uint32_t i = 0; i < p->count && result == VK_SUCCESS; i++) {
                struct zvk_draw command = {0};
                const void *bytes = p->buffer->memory->data + p->buffer->offset + p->offset +
                                    (VkDeviceSize)i * p->stride;
                if (p->indexed) {
                    VkDrawIndexedIndirectCommand indirect;
                    memcpy(&indirect, bytes, sizeof indirect);
                    command = (struct zvk_draw){indirect.indexCount, indirect.instanceCount,
                                                indirect.firstIndex, indirect.firstInstance,
                                                indirect.vertexOffset};
                } else {
                    VkDrawIndirectCommand indirect;
                    memcpy(&indirect, bytes, sizeof indirect);
                    command = (struct zvk_draw){indirect.vertexCount, indirect.instanceCount,
                                                indirect.firstVertex, indirect.firstInstance, 0};
                }
                result = draw(e, &command, p->indexed);
            }
            break;
        }
        case ZVK_WAIT_EVENTS: {
            const struct zvk_wait_events *p = data;
            if (virgl_flush_fenced(queue->device->gpu)) {
                result = VK_ERROR_DEVICE_LOST;
                break;
            }
            for (;;) {
                int ready = 1;
                for (uint32_t i = 0; i < p->count; i++)
                    if (!p->events[i]->signaled)
                        ready = 0;
                if (ready || queue->device->lost)
                    break;
                pthread_cond_wait(&queue->device->condition, &queue->device->mutex);
            }
            result = queue->device->lost;
            break;
        }
        case ZVK_BLIT_IMAGE: {
            const struct zvk_blit *p = data;
            if (!zvk_layout_is(p->src, p->region.srcSubresource.mipLevel,
                               p->region.srcSubresource.baseArrayLayer, p->src_layout) ||
                !zvk_layout_is(p->dst, p->region.dstSubresource.mipLevel,
                               p->region.dstSubresource.baseArrayLayer, p->dst_layout)) {
                result = VK_ERROR_INITIALIZATION_FAILED;
                break;
            }
            result = zvk_image_gpu(p->src);
            if (result != VK_SUCCESS)
                break;
            result = zvk_image_gpu(p->dst);
            if (result != VK_SUCCESS)
                break;
            if (p->src->info.samples == VK_SAMPLE_COUNT_1_BIT &&
                p->region.srcSubresource.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT) {
                if (e->active_query) {
                    result=zvk_query_pause(e->active_query,e->active_query_index);
                    if (result!=VK_SUCCESS)break;
                }
                result = zvk_blit_volume(p);
                if (e->active_query)zvk_query_resume(e->active_query,e->active_query_index);
                p->dst->transfer_valid[p->region.dstSubresource.mipLevel * p->dst->info.arrayLayers +
                                       p->region.dstSubresource.baseArrayLayer] = 0;
                break;
            }
            if (p->src->info.samples == VK_SAMPLE_COUNT_1_BIT &&
                p->region.srcSubresource.aspectMask != VK_IMAGE_ASPECT_COLOR_BIT) {
                result = depth_blit(p);
                break;
            }
            const VkImageBlit *r = &p->region;
            struct virgl_blit_info blit = {
                .mask =
                    r->srcSubresource.aspectMask & VK_IMAGE_ASPECT_COLOR_BIT
                        ? PIPE_MASK_RGBA
                        : ((r->srcSubresource.aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT ? PIPE_MASK_Z
                                                                                     : 0) |
                           (r->srcSubresource.aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT ? PIPE_MASK_S
                                                                                       : 0)),
                .filter = p->filter == VK_FILTER_LINEAR ? PIPE_TEX_FILTER_LINEAR
                                                        : PIPE_TEX_FILTER_NEAREST,
                .src = {p->src->gpu->id, r->srcSubresource.mipLevel, p->src->format,
                        r->srcOffsets[0].x, r->srcOffsets[0].y,
                        p->src->info.imageType == VK_IMAGE_TYPE_3D
                            ? r->srcOffsets[0].z
                            : (int)r->srcSubresource.baseArrayLayer,
                        r->srcOffsets[1].x - r->srcOffsets[0].x,
                        r->srcOffsets[1].y - r->srcOffsets[0].y,
                        r->srcOffsets[1].z - r->srcOffsets[0].z},
                .dst = {p->dst->gpu->id, r->dstSubresource.mipLevel, p->dst->format,
                        r->dstOffsets[0].x, r->dstOffsets[0].y,
                        p->dst->info.imageType == VK_IMAGE_TYPE_3D
                            ? r->dstOffsets[0].z
                            : (int)r->dstSubresource.baseArrayLayer,
                        r->dstOffsets[1].x - r->dstOffsets[0].x,
                        r->dstOffsets[1].y - r->dstOffsets[0].y,
                        r->dstOffsets[1].z - r->dstOffsets[0].z}};
            virgl_blit(queue->device->gpu, &blit);
            p->dst->transfer_valid[r->dstSubresource.mipLevel * p->dst->info.arrayLayers +
                                   r->dstSubresource.baseArrayLayer] = 0;
            break;
        }
        case ZVK_CLEAR_ATTACHMENT: {
            const struct zvk_attachment_clear *p = data;
            if (!e->pass) {
                result = VK_ERROR_INITIALIZATION_FAILED;
                break;
            }
            const VkRect2D *r = &p->rect.rect;
            if (!zvk_range(e->framebuffer->layers, p->rect.baseArrayLayer, p->rect.layerCount) ||
                r->offset.x < e->area.offset.x || r->offset.y < e->area.offset.y ||
                (uint64_t)r->offset.x + r->extent.width >
                    (uint64_t)e->area.offset.x + e->area.extent.width ||
                (uint64_t)r->offset.y + r->extent.height >
                    (uint64_t)e->area.offset.y + e->area.extent.height) {
                result = VK_ERROR_INITIALIZATION_FAILED;
                break;
            }
            int depth = p->attachment.aspectMask != VK_IMAGE_ASPECT_COLOR_BIT;
            uint32_t slot =
                depth ? e->pass->subpasses[e->subpass].depth
                : p->attachment.colorAttachment < e->pass->subpasses[e->subpass].ncolors
                    ? e->pass->subpasses[e->subpass].colors[p->attachment.colorAttachment]
                    : VK_ATTACHMENT_UNUSED;
            if (slot == VK_ATTACHMENT_UNUSED) {
                /* Clearing an unused attachment reference has no effect. */
                break;
            }
            VkImageView view = e->framebuffer->views[slot];
            VkImageAspectFlags aspects=p->attachment.aspectMask;
            if (depth)aspects &= VK_IMAGE_ASPECT_DEPTH_BIT |
                (view->format==VK_FORMAT_D24_UNORM_S8_UINT?VK_IMAGE_ASPECT_STENCIL_BIT:0);
            if (!aspects)break;
            result=mutable_prepare(e,view);
            if (result!=VK_SUCCESS)break;
            for (uint32_t layer = 0; layer < p->rect.layerCount; layer++) {
                result =
                    clear_image(view_image(view), view->range.baseMipLevel,
                                view->range.baseArrayLayer + p->rect.baseArrayLayer + layer, *r,
                                &p->attachment.clearValue, depth, aspects);
                if (result != VK_SUCCESS)
                    break;
            }
            if (result==VK_SUCCESS)result=mutable_commit(e,view);
            if (result == VK_SUCCESS && view->image->info.samples != VK_SAMPLE_COUNT_1_BIT)
                result = bind_framebuffer(e);
            break;
        }
        case ZVK_RESET_QUERY: {
            const struct zvk_query_command *p = data;
            result = zvk_query_reset(p->pool, p->first, p->count);
            break;
        }
        case ZVK_BEGIN_QUERY: {
            const struct zvk_query_command *p = data;
            result = e->active_query ? VK_ERROR_INITIALIZATION_FAILED
                                     : zvk_query_begin(p->pool, p->first);
            if (result == VK_SUCCESS) {
                e->active_query = p->pool;
                e->active_query_index = p->first;
            }
            break;
        }
        case ZVK_END_QUERY: {
            const struct zvk_query_command *p = data;
            result = zvk_query_end(p->pool, p->first);
            if (result == VK_SUCCESS)
                e->active_query = NULL;
            break;
        }
        case ZVK_COPY_QUERY: {
            const struct zvk_query_copy *p = data;
            if (!p->buffer->memory) {
                result = VK_ERROR_INITIALIZATION_FAILED;
                break;
            }
            result = zvk_query_results(
                p->pool, p->first, p->count, (size_t)(p->buffer->size - p->offset),
                p->buffer->memory->data + p->buffer->offset + p->offset, p->stride, p->flags);
            if (result == VK_NOT_READY && !(p->flags & VK_QUERY_RESULT_WAIT_BIT))
                result = VK_SUCCESS;
            break;
        }
        case ZVK_COPY_BUFFER: {
            const struct zvk_copy_buffer *p = data;
            if (!p->src->memory || !p->dst->memory) {
                result = VK_ERROR_INITIALIZATION_FAILED;
                break;
            }
            memmove(p->dst->memory->data + p->dst->offset + p->region.dstOffset,
                    p->src->memory->data + p->src->offset + p->region.srcOffset,
                    (size_t)p->region.size);
            break;
        }
        case ZVK_FILL_BUFFER:
        case ZVK_UPDATE_BUFFER: {
            const struct zvk_fill *p = data;
            if (!p->buffer->memory) {
                result = VK_ERROR_INITIALIZATION_FAILED;
                break;
            }
            uint8_t *dst = p->buffer->memory->data + p->buffer->offset + p->offset;
            if (header->op == ZVK_UPDATE_BUFFER)
                memcpy(dst, p + 1, (size_t)p->size);
            else
                for (size_t i = 0; i < (size_t)p->size; i += 4)
                    memcpy(dst + i, &p->value, 4);
            break;
        }
        case ZVK_CLEAR_IMAGE: {
            const struct zvk_clear *p = data;
            VkImageSubresourceRange range;
            zvk_image_range(p->image, &p->range, &range);
            for (uint32_t m = range.baseMipLevel;
                 m < range.baseMipLevel + range.levelCount && result == VK_SUCCESS; m++)
                for (uint32_t l = range.baseArrayLayer;
                     l < range.baseArrayLayer + range.layerCount && result == VK_SUCCESS; l++) {
                    if (!zvk_layout_is(p->image, m, l, p->layout)) {
                        result = VK_ERROR_INITIALIZATION_FAILED;
                        break;
                    }
                    VkRect2D area = {{0, 0},
                                     {zvk_mip(p->image->info.extent.width, m),
                                      zvk_mip(p->image->info.extent.height, m)}};
                    result =
                        clear_image(p->image, m, l, area, &p->value, p->depth, p->range.aspectMask);
                }
            break;
        }
        case ZVK_COPY_IMAGE_BUFFER:
        case ZVK_COPY_BUFFER_IMAGE:
            result = image_buffer(data, header->op == ZVK_COPY_BUFFER_IMAGE);
            break;
        case ZVK_COPY_IMAGE: {
            const struct zvk_copy_image *p = data;
            if (!zvk_layout_is(p->src, p->region.srcSubresource.mipLevel,
                               p->region.srcSubresource.baseArrayLayer, p->src_layout) ||
                !zvk_layout_is(p->dst, p->region.dstSubresource.mipLevel,
                               p->region.dstSubresource.baseArrayLayer, p->dst_layout)) {
                result = VK_ERROR_INITIALIZATION_FAILED;
                break;
            }
            if (p->src->info.samples != VK_SAMPLE_COUNT_1_BIT) {
                result = zvk_image_gpu(p->src);
                if (result != VK_SUCCESS)
                    break;
                result = zvk_image_gpu(p->dst);
                if (result != VK_SUCCESS)
                    break;
                const VkImageCopy *r = &p->region;
                VkImageAspectFlags a = r->srcSubresource.aspectMask;
                struct virgl_blit_info blit = {
                    .mask = a == VK_IMAGE_ASPECT_COLOR_BIT
                                ? PIPE_MASK_RGBA
                                : ((a & VK_IMAGE_ASPECT_DEPTH_BIT ? PIPE_MASK_Z : 0) |
                                   (a & VK_IMAGE_ASPECT_STENCIL_BIT ? PIPE_MASK_S : 0)),
                    .filter = PIPE_TEX_FILTER_NEAREST,
                    .src = {p->src->gpu->id, r->srcSubresource.mipLevel, p->src->format,
                            r->srcOffset.x, r->srcOffset.y, (int)r->srcSubresource.baseArrayLayer,
                            (int)r->extent.width, (int)r->extent.height, 1},
                    .dst = {p->dst->gpu->id, r->dstSubresource.mipLevel, p->dst->format,
                            r->dstOffset.x, r->dstOffset.y, (int)r->dstSubresource.baseArrayLayer,
                            (int)r->extent.width, (int)r->extent.height, 1}};
                virgl_blit(queue->device->gpu, &blit);
                break;
            }
            result = image_read(p->src, p->region.srcSubresource.mipLevel,
                                p->region.srcSubresource.baseArrayLayer);
            if (result != VK_SUCCESS)
                break;
            result = image_read(p->dst, p->region.dstSubresource.mipLevel,
                                p->region.dstSubresource.baseArrayLayer);
            if (result != VK_SUCCESS)
                break;
            unsigned block = zvk_block(p->src->info.format);
            unsigned dst_block = zvk_block(p->dst->info.format);
            size_t row =
                (size_t)zvk_block_count(p->src->info.format, p->region.extent.width) * p->src->bpp;
            unsigned rows = zvk_block_count(p->src->info.format, p->region.extent.height);
            for (uint32_t z = 0; z < p->region.extent.depth; z++)
                for (uint32_t y = 0; y < rows; y++) {
                    uint8_t *src =
                        (uint8_t *)zvk_image_map(p->src) +
                        zvk_image_offset(p->src, p->region.srcSubresource.mipLevel,
                                         p->region.srcSubresource.baseArrayLayer) +
                        ((((size_t)(uint32_t)p->region.srcOffset.z + z) *
                              zvk_block_count(p->src->info.format,
                                              zvk_mip(p->src->info.extent.height,
                                                      p->region.srcSubresource.mipLevel)) +
                          (uint32_t)p->region.srcOffset.y / block + y) *
                             zvk_block_count(p->src->info.format,
                                             zvk_mip(p->src->info.extent.width,
                                                     p->region.srcSubresource.mipLevel)) +
                         (uint32_t)p->region.srcOffset.x / block) *
                            p->src->bpp;
                    uint8_t *dst =
                        (uint8_t *)zvk_image_map(p->dst) +
                        zvk_image_offset(p->dst, p->region.dstSubresource.mipLevel,
                                         p->region.dstSubresource.baseArrayLayer) +
                        ((((size_t)(uint32_t)p->region.dstOffset.z + z) *
                              zvk_block_count(p->dst->info.format,
                                              zvk_mip(p->dst->info.extent.height,
                                                      p->region.dstSubresource.mipLevel)) +
                          (uint32_t)p->region.dstOffset.y / dst_block + y) *
                             zvk_block_count(p->dst->info.format,
                                             zvk_mip(p->dst->info.extent.width,
                                                     p->region.dstSubresource.mipLevel)) +
                         (uint32_t)p->region.dstOffset.x / dst_block) *
                            p->dst->bpp;
                    if (p->src->info.format == VK_FORMAT_D24_UNORM_S8_UINT) {
                        uint32_t mask =
                            p->region.srcSubresource.aspectMask == VK_IMAGE_ASPECT_DEPTH_BIT
                                ? 0xffffffu
                            : p->region.srcSubresource.aspectMask == VK_IMAGE_ASPECT_STENCIL_BIT
                                ? 0xff000000u
                                : UINT32_MAX;
                        for (uint32_t x = 0; x < p->region.extent.width; x++) {
                            uint32_t a, b;
                            memcpy(&a, src + x * 4, 4);
                            memcpy(&b, dst + x * 4, 4);
                            b = (b & ~mask) | (a & mask);
                            memcpy(dst + x * 4, &b, 4);
                        }
                    } else
                        memmove(dst, src, row);
                }
            result = image_write(p->dst, p->region.dstSubresource.mipLevel,
                                 p->region.dstSubresource.baseArrayLayer);
            break;
        }
        case ZVK_BARRIER:
            if (virgl_flush_fenced(queue->device->gpu)) {
                result = VK_ERROR_DEVICE_LOST;
                break;
            }
            if (header->size >= sizeof *header + sizeof(struct zvk_transition)) {
                const struct zvk_transition *p = data;
                for (uint32_t m = p->range.baseMipLevel;
                     m < p->range.baseMipLevel + p->range.levelCount; m++)
                    for (uint32_t l = p->range.baseArrayLayer;
                         l < p->range.baseArrayLayer + p->range.layerCount; l++)
                        if (p->old_layout != VK_IMAGE_LAYOUT_UNDEFINED &&
                            !zvk_layout_is(p->image, m, l, p->old_layout))
                            result = VK_ERROR_INITIALIZATION_FAILED;
                if (result == VK_SUCCESS)
                    zvk_layout_set(p->image, &p->range, p->layout);
                else
                    zvk_error("image barrier oldLayout does not match execution state");
            }
            break;
        case ZVK_EXECUTE:
            result = execute(queue, *(const VkCommandBuffer *)data, depth + 1, e);
            break;
        case ZVK_SET_EVENT:
        case ZVK_RESET_EVENT:
            if (virgl_flush_fenced(queue->device->gpu)) {
                result = VK_ERROR_DEVICE_LOST;
                break;
            }
            (*(const VkEvent *)data)->signaled = header->op == ZVK_SET_EVENT;
            pthread_cond_broadcast(&queue->device->condition);
            break;
        default:
            result = VK_ERROR_FEATURE_NOT_PRESENT;
            break;
        }
        if (result != VK_SUCCESS) {
            if (getenv("ZVK_DEBUG_COMMANDS"))
                fprintf(stderr,"zvk command %u failed: %d (%s)\n",header->op,result,zvkGetLastError());
            break;
        }
    }
    if (virgl_flush_fenced(queue->device->gpu))
        result = VK_ERROR_DEVICE_LOST;
    for (unsigned s=0;s<2;s++)
        if (e->atlas[s]) {
            virgl_destroy_object(queue->device->gpu,e->atlas_view[s],VIRGL_OBJECT_SAMPLER_VIEW);
            virgl_destroy_object(queue->device->gpu,e->atlas_sampler[s],VIRGL_OBJECT_SAMPLER_STATE);
            virgl_res_destroy(queue->device->gpu,e->atlas[s]);
        }
    for (int s = 0; s < 2; s++)
        for (int i = 0; i < ZVK_UBOS; i++)
            if (e->uniforms[s][i])
                virgl_res_destroy(queue->device->gpu, e->uniforms[s][i]);
    if (e->empty_gpu) {
        virgl_destroy_object(queue->device->gpu, e->empty_surface, VIRGL_OBJECT_SURFACE);
        virgl_res_destroy(queue->device->gpu, e->empty_gpu);
    }
    for (unsigned i = 0; i < ZVK_ATTRS; i++)
        if (e->expanded_vertices[i])
            virgl_res_destroy(queue->device->gpu, e->expanded_vertices[i]);
    if (e->dynamic_dsa)
        virgl_destroy_object(queue->device->gpu, e->dynamic_dsa, VIRGL_OBJECT_DSA);
    if (e->dynamic_raster)
        virgl_destroy_object(queue->device->gpu, e->dynamic_raster, VIRGL_OBJECT_RASTERIZER);
    free(e);
    return result;
}
VkResult zvk_execute(VkQueue queue, VkCommandBuffer command, uint32_t depth)
{
    return execute(queue, command, depth, NULL);
}
VKAPI_ATTR VkResult VKAPI_CALL vkQueueBindSparse(VkQueue queue, uint32_t count,
                                                 const VkBindSparseInfo *bind, VkFence fence)
{
    (void)queue;
    (void)count;
    (void)bind;
    (void)fence;
    return VK_ERROR_FEATURE_NOT_PRESENT;
}
