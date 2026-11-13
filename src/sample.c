/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Preserve every multisample texel for the Metal buffer transport. A GPU
 * fetch pass writes one plane per sample, without resolving or CPU shading.
 * The CPU only transfers the resulting bytes between the GL/Metal buffers. */
#include "zvk_internal.h"

/* Stencil is not texture-addressable on the Mac GL host. Reconstruct its
 * eight bits using read-only per-sample stencil tests and additive color
 * writes. No depth/stencil sample is modified and all evaluation is on GPU. */
static VkResult stencil_samples(VkImageView view, virgl_res *vertices)
{
    VkImage image = view->reinterpret ? view->reinterpret : view->image;
    virgl *gpu = view->device->gpu;
    unsigned w = zvk_mip(image->info.extent.width, view->range.baseMipLevel);
    unsigned h = zvk_mip(image->info.extent.height, view->range.baseMipLevel);
    if (!view->stencil_gpu) {
        struct gpu_res_create rc = {.target = PIPE_TEXTURE_2D_ARRAY,
                                    .format = VIRGL_FORMAT_R8G8B8A8_UNORM,
                                    .bind = VIRGL_BIND_RENDER_TARGET | VIRGL_BIND_SAMPLER_VIEW,
                                    .width = w,
                                    .height = h,
                                    .depth = 1,
                                    .array_size = view->range.layerCount,
                                    .nr_samples = image->info.samples};
        view->stencil_gpu = virgl_res_create_ex(gpu, &rc);
        if (!view->stencil_gpu)
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    unsigned vs = virgl_handle(gpu), fs = virgl_handle(gpu), ve = virgl_handle(gpu);
    unsigned blend = virgl_handle(gpu), rs = virgl_handle(gpu);
    virgl_create_shader(gpu, vs, PIPE_SHADER_VERTEX,
                        "VERT\nDCL IN[0]\nDCL OUT[0], POSITION\n0: MOV OUT[0], IN[0]\n1: END\n");
    virgl_create_shader(
        gpu, fs, PIPE_SHADER_FRAGMENT,
        "FRAG\nDCL OUT[0], COLOR[0]\nDCL CONST[0][0]\n0: MOV OUT[0], CONST[0][0]\n1: END\n");
    unsigned zero = 0, format = VIRGL_FORMAT_R32G32B32A32_FLOAT;
    virgl_create_vertex_elements(gpu, ve, 1, &zero, &zero, &format);
    uint32_t b[VIRGL_OBJ_BLEND_SIZE - 1] = {0}, r[VIRGL_OBJ_RS_SIZE - 1] = {0};
    b[2] = VIRGL_OBJ_BLEND_S2_RT_BLEND_ENABLE(1) |
           VIRGL_OBJ_BLEND_S2_RT_RGB_SRC_FACTOR(PIPE_BLENDFACTOR_ONE) |
           VIRGL_OBJ_BLEND_S2_RT_RGB_DST_FACTOR(PIPE_BLENDFACTOR_ONE) |
           VIRGL_OBJ_BLEND_S2_RT_ALPHA_SRC_FACTOR(PIPE_BLENDFACTOR_ONE) |
           VIRGL_OBJ_BLEND_S2_RT_ALPHA_DST_FACTOR(PIPE_BLENDFACTOR_ONE) |
           VIRGL_OBJ_BLEND_S2_RT_COLORMASK(1);
    r[0] = VIRGL_OBJ_RS_S0_HALF_PIXEL_CENTER(1) | VIRGL_OBJ_RS_S0_MULTISAMPLE(1);
    virgl_create_blend(gpu, blend, b);
    virgl_create_rasterizer(gpu, rs, r);
    virgl_bind_shader(gpu, vs, PIPE_SHADER_VERTEX);
    virgl_bind_shader(gpu, fs, PIPE_SHADER_FRAGMENT);
    virgl_bind_shader(gpu, 0, PIPE_SHADER_GEOMETRY);
    virgl_bind_object(gpu, ve, VIRGL_OBJECT_VERTEX_ELEMENTS);
    virgl_bind_object(gpu, rs, VIRGL_OBJECT_RASTERIZER);
    unsigned stride = 16, resource = vertices->id;
    virgl_set_vertex_buffers(gpu, 1, &stride, &zero, &resource);
    virgl_set_viewport(gpu, 0, 0, w, h, 0, 1, 0);
    virgl_set_scissor(gpu, 0, 0, w, h);
    virgl_set_sample_mask(gpu, UINT32_MAX);
    virgl_set_min_samples(gpu, 0);
    virgl_set_stencil_ref(gpu, 0, 0);
    for (unsigned layer = 0; layer < view->range.layerCount; layer++) {
        unsigned color = virgl_handle(gpu), depth = virgl_handle(gpu);
        unsigned base = view->range.baseArrayLayer + layer;
        virgl_create_surface(gpu, color, view->stencil_gpu->id, VIRGL_FORMAT_R8G8B8A8_UNORM, 0,
                             layer | (layer << 16));
        virgl_create_surface(gpu, depth, image->gpu->id, image->format, view->range.baseMipLevel,
                             base | (base << 16));
        virgl_set_framebuffer(gpu, depth, &color, 1);
        const float clear[4] = {0, 0, 0, 1};
        virgl_clear(gpu, PIPE_CLEAR_COLOR0, clear, 0, 0);
        virgl_bind_object(gpu, blend, VIRGL_OBJECT_BLEND);
        for (unsigned bit = 0; bit < 8; bit++) {
            unsigned ds = virgl_handle(gpu);
            uint32_t words[VIRGL_OBJ_DSA_SIZE - 1] = {0};
            words[1] = words[2] = VIRGL_OBJ_DSA_S1_STENCIL_ENABLED(1) |
                                  VIRGL_OBJ_DSA_S1_STENCIL_FUNC(VK_COMPARE_OP_NOT_EQUAL) |
                                  VIRGL_OBJ_DSA_S1_STENCIL_VALUEMASK(1u << bit);
            virgl_create_dsa(gpu, ds, words);
            virgl_bind_object(gpu, ds, VIRGL_OBJECT_DSA);
            float value[4] = {(float)(1u << bit) / 255.0f, 0, 0, 1};
            virgl_set_constants(gpu, PIPE_SHADER_FRAGMENT, value, 4);
            virgl_draw(gpu, PIPE_PRIM_TRIANGLE_STRIP, 0, 4, 0, 0, 3);
            virgl_destroy_object(gpu, ds, VIRGL_OBJECT_DSA);
        }
        virgl_destroy_object(gpu, color, VIRGL_OBJECT_SURFACE);
        virgl_destroy_object(gpu, depth, VIRGL_OBJECT_SURFACE);
    }
    virgl_destroy_object(gpu, vs, VIRGL_OBJECT_SHADER);
    virgl_destroy_object(gpu, fs, VIRGL_OBJECT_SHADER);
    virgl_destroy_object(gpu, ve, VIRGL_OBJECT_VERTEX_ELEMENTS);
    virgl_destroy_object(gpu, blend, VIRGL_OBJECT_BLEND);
    virgl_destroy_object(gpu, rs, VIRGL_OBJECT_RASTERIZER);
    return VK_SUCCESS;
}

VkResult zvk_sample_snapshot(VkImageView view, VkDeviceMemory *memory)
{
    VkImage image = view->reinterpret ? view->reinterpret : view->image;
    virgl *gpu = view->device->gpu;
    unsigned w = zvk_mip(image->info.extent.width, view->range.baseMipLevel);
    unsigned h = zvk_mip(image->info.extent.height, view->range.baseMipLevel);
    unsigned samples = image->info.samples, layers = view->range.layerCount;
    uint64_t size = (uint64_t)w * h * samples * layers * 16;
    unsigned limit = view->device->physical->caps.max_texture_2d_size;
    if (size > ZVK_MAX_ALLOC || (limit && (uint64_t)h * samples > limit))
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    int stencil = view->range.aspectMask == VK_IMAGE_ASPECT_STENCIL_BIT;
    int kind = stencil ? 1
                       : ((view->range.aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT)
                              ? 0
                              : zvk_format_integer(view->format));
    unsigned format = kind == 1   ? VIRGL_FORMAT_R32G32B32A32_UINT
                      : kind == 2 ? VIRGL_FORMAT_R32G32B32A32_SINT
                                  : VIRGL_FORMAT_R32G32B32A32_FLOAT;
    if (!view->compute_image) {
        struct gpu_res_create rc = {.target = PIPE_TEXTURE_2D_ARRAY,
                                    .format = format,
                                    .bind = VIRGL_BIND_RENDER_TARGET | VIRGL_BIND_SAMPLER_VIEW,
                                    .width = w,
                                    .height = h * samples,
                                    .depth = 1,
                                    .array_size = layers,
                                    .size = size};
        view->compute_image = virgl_res_create_ex(gpu, &rc);
        if (!view->compute_image)
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    if (!view->compute_memory) {
        view->compute_memory = zvk_alloc(&view->alloc, sizeof *view->compute_memory);
        if (!view->compute_memory)
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        view->compute_memory->device = view->device;
        view->compute_memory->data = view->compute_image->map;
        view->compute_memory->size = size;
    }
    virgl_res *vertices = virgl_res_create(gpu, PIPE_BUFFER, VIRGL_FORMAT_R8_UNORM,
                                           VIRGL_BIND_VERTEX_BUFFER, 4 * 4 * 4, 1, 1, 0, 4 * 4 * 4);
    if (!vertices)
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    const float positions[4][4] = {{-1, -1, 0, 1}, {1, -1, 0, 1}, {-1, 1, 0, 1}, {1, 1, 0, 1}};
    memcpy(vertices->map, positions, sizeof positions);
    if (virgl_res_to_host(gpu, vertices, 0, 0, 0, 0, sizeof positions, 1, 1, 0, 0, 0)) {
        virgl_res_destroy(gpu, vertices);
        return VK_ERROR_DEVICE_LOST;
    }
    if (stencil) {
        VkResult r = stencil_samples(view, vertices);
        if (r != VK_SUCCESS) {
            virgl_res_destroy(gpu, vertices);
            return r;
        }
    }
    virgl_res *source = stencil ? view->stencil_gpu : image->gpu;
    unsigned vs = virgl_handle(gpu), fs = virgl_handle(gpu), ve = virgl_handle(gpu);
    unsigned blend = virgl_handle(gpu), raster = virgl_handle(gpu), depth = virgl_handle(gpu);
    unsigned sampler_view = virgl_handle(gpu);
    const char *target = source->target == PIPE_TEXTURE_2D_ARRAY ? "2D_ARRAY_MSAA" : "2D_MSAA";
    char shader[1600];
    int n = snprintf(
        shader, sizeof shader,
        "FRAG\nPROPERTY FS_COORD_ORIGIN UPPER_LEFT\nDCL IN[0], POSITION\nDCL OUT[0], COLOR[0]\n"
        "DCL CONST[0][0]\nDCL SAMP[0]\nDCL SVIEW[0], %s, %s\nDCL TEMP[0..1]\n"
        "IMM[0] UINT32 {0, %u, 0, 0}\nIMM[1] FLT32 {255,255,255,255}\n"
        "0: F2U TEMP[0].xy, IN[0]\n"
        "1: UADD TEMP[0].y, TEMP[0].yyyy, CONST[0][0].zzzz\n"
        "2: MOV TEMP[0].z, CONST[0][0].yyyy\n"
        "3: MOV TEMP[0].w, CONST[0][0].xxxx\n"
        "4: TXF TEMP[1], TEMP[0], SAMP[0], %s\n",
        target,
        stencil     ? "FLOAT"
        : kind == 1 ? "UINT"
        : kind == 2 ? "SINT"
                    : "FLOAT",
        kind ? 1u : 0x3f800000u, target);
    if (stencil)
        n += snprintf(shader + n, sizeof shader - n,
                      "5: MUL TEMP[1].x, TEMP[1].xxxx, IMM[1].xxxx\n"
                      "6: ROUND TEMP[1].x, TEMP[1].xxxx\n"
                      "7: F2U TEMP[1].x, TEMP[1].xxxx\n"
                      "8: MOV TEMP[1].yz, IMM[0].xxxx\n"
                      "9: MOV TEMP[1].w, IMM[0].yyyy\n");
    const VkComponentSwizzle *mapping = &view->swizzle.r;
    for (unsigned c = 0; c < 4; c++) {
        unsigned sw =
            mapping[c] == VK_COMPONENT_SWIZZLE_IDENTITY ? VK_COMPONENT_SWIZZLE_R + c : mapping[c];
        if (sw == VK_COMPONENT_SWIZZLE_ZERO || sw == VK_COMPONENT_SWIZZLE_ONE)
            n += snprintf(shader + n, sizeof shader - n, "%u: MOV OUT[0].%c, IMM[0].%c%c%c%c\n",
                          10 + c, "xyzw"[c], sw == VK_COMPONENT_SWIZZLE_ZERO ? 'x' : 'y',
                          sw == VK_COMPONENT_SWIZZLE_ZERO ? 'x' : 'y',
                          sw == VK_COMPONENT_SWIZZLE_ZERO ? 'x' : 'y',
                          sw == VK_COMPONENT_SWIZZLE_ZERO ? 'x' : 'y');
        else {
            char src = "xyzw"[sw - VK_COMPONENT_SWIZZLE_R];
            n += snprintf(shader + n, sizeof shader - n, "%u: MOV OUT[0].%c, TEMP[1].%c%c%c%c\n",
                          10 + c, "xyzw"[c], src, src, src, src);
        }
    }
    snprintf(shader + n, sizeof shader - n, "14: END\n");
    virgl_create_shader(gpu, vs, PIPE_SHADER_VERTEX,
                        "VERT\nDCL IN[0]\nDCL OUT[0], POSITION\n0: MOV OUT[0], IN[0]\n1: END\n");
    virgl_create_shader(gpu, fs, PIPE_SHADER_FRAGMENT, shader);
    unsigned zero = 0, vf = VIRGL_FORMAT_R32G32B32A32_FLOAT;
    virgl_create_vertex_elements(gpu, ve, 1, &zero, &zero, &vf);
    uint32_t b[VIRGL_OBJ_BLEND_SIZE - 1] = {0}, rs[VIRGL_OBJ_RS_SIZE - 1] = {0},
                                      ds[VIRGL_OBJ_DSA_SIZE - 1] = {0};
    b[2] = VIRGL_OBJ_BLEND_S2_RT_COLORMASK(15);
    rs[0] = VIRGL_OBJ_RS_S0_HALF_PIXEL_CENTER(1);
    virgl_create_blend(gpu, blend, b);
    virgl_create_rasterizer(gpu, raster, rs);
    virgl_create_dsa(gpu, depth, ds);
    virgl_bind_object(gpu, blend, VIRGL_OBJECT_BLEND);
    virgl_bind_object(gpu, raster, VIRGL_OBJECT_RASTERIZER);
    virgl_bind_object(gpu, depth, VIRGL_OBJECT_DSA);
    virgl_bind_object(gpu, ve, VIRGL_OBJECT_VERTEX_ELEMENTS);
    virgl_bind_shader(gpu, vs, PIPE_SHADER_VERTEX);
    virgl_bind_shader(gpu, fs, PIPE_SHADER_FRAGMENT);
    virgl_bind_shader(gpu, 0, PIPE_SHADER_GEOMETRY);
    virgl_create_sampler_view_ex(gpu, sampler_view, source->id, source->target,
                                 stencil ? VIRGL_FORMAT_R8G8B8A8_UNORM : image->format, 0,
                                 (stencil ? layers : image->info.arrayLayers) - 1, 0, 0,
                                 0 | (1 << 3) | (2 << 6) | (3 << 9));
    virgl_set_sampler_views(gpu, PIPE_SHADER_FRAGMENT, 0, &sampler_view, 1);
    virgl_bind_sampler_states(gpu, PIPE_SHADER_FRAGMENT, 0, &zero, 1);
    virgl_set_sample_mask(gpu, UINT32_MAX);
    virgl_set_min_samples(gpu, 0);
    unsigned stride = 16, resource = vertices->id;
    virgl_set_vertex_buffers(gpu, 1, &stride, &zero, &resource);
    for (unsigned layer = 0; layer < layers; layer++) {
        unsigned surface = virgl_handle(gpu);
        virgl_create_surface(gpu, surface, view->compute_image->id, format, 0,
                             layer | (layer << 16));
        virgl_set_framebuffer(gpu, 0, &surface, 1);
        for (unsigned sample = 0; sample < samples; sample++) {
            uint32_t constants[4] = {sample, (stencil ? 0 : view->range.baseArrayLayer) + layer,
                                     (uint32_t)-(sample * h), 0};
            virgl_set_constants(gpu, PIPE_SHADER_FRAGMENT, (float *)constants, 4);
            virgl_set_viewport(gpu, 0, sample * h, w, h, 0, 1, 0);
            virgl_set_scissor(gpu, 0, sample * h, w, (sample + 1) * h);
            virgl_draw(gpu, PIPE_PRIM_TRIANGLE_STRIP, 0, 4, 0, 0, 3);
        }
        virgl_destroy_object(gpu, surface, VIRGL_OBJECT_SURFACE);
    }
    VkResult result = VK_SUCCESS;
    if (virgl_flush_fenced(gpu))
        result = VK_ERROR_DEVICE_LOST;
    for (unsigned layer = 0; result == VK_SUCCESS && layer < layers; layer++)
        if (virgl_res_from_host(gpu, view->compute_image, 0, 0, 0, layer, w, h * samples, 1, w * 16,
                                w * h * samples * 16, (uint64_t)layer * w * h * samples * 16))
            result = VK_ERROR_DEVICE_LOST;
    virgl_destroy_object(gpu, sampler_view, VIRGL_OBJECT_SAMPLER_VIEW);
    virgl_destroy_object(gpu, vs, VIRGL_OBJECT_SHADER);
    virgl_destroy_object(gpu, fs, VIRGL_OBJECT_SHADER);
    virgl_destroy_object(gpu, ve, VIRGL_OBJECT_VERTEX_ELEMENTS);
    virgl_destroy_object(gpu, blend, VIRGL_OBJECT_BLEND);
    virgl_destroy_object(gpu, raster, VIRGL_OBJECT_RASTERIZER);
    virgl_destroy_object(gpu, depth, VIRGL_OBJECT_DSA);
    virgl_res_destroy(gpu, vertices);
    *memory = view->compute_memory;
    return result;
}
