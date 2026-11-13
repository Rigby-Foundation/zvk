/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Color blits sample on the GPU, including linear-space sRGB filtering and
 * fractional volume coordinates. No CPU texel evaluation is used. */
#include "zvk_internal.h"
#include <math.h>

VkResult zvk_blit_volume(const struct zvk_blit *p)
{
    virgl *gpu = p->src->device->gpu;
    const VkImageBlit *r = &p->region;
    int dx = r->dstOffsets[1].x - r->dstOffsets[0].x, dy = r->dstOffsets[1].y - r->dstOffsets[0].y;
    int dz = r->dstOffsets[1].z - r->dstOffsets[0].z;
    if (!dx || !dy || !dz)
        return VK_SUCCESS;
    unsigned smip = r->srcSubresource.mipLevel, dmip = r->dstSubresource.mipLevel;
    float sw = zvk_mip(p->src->info.extent.width, smip),
          sh = zvk_mip(p->src->info.extent.height, smip);
    float sd = zvk_mip(p->src->info.extent.depth, smip);
    struct gpu_res_create rc = {.target = PIPE_BUFFER,
                                .format = VIRGL_FORMAT_R8_UNORM,
                                .bind = VIRGL_BIND_VERTEX_BUFFER,
                                .width = 4 * 7 * 4,
                                .height = 1,
                                .depth = 1,
                                .array_size = 1,
                                .size = 4 * 7 * 4};
    virgl_res *vertices = virgl_res_create_ex(gpu, &rc), *snapshot = NULL;
    if (!vertices)
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    uint32_t source = p->src->gpu->id;
    uint32_t target = p->src->target;
    int cube = target == PIPE_TEXTURE_CUBE;
    if (cube)
        target = PIPE_TEXTURE_2D_ARRAY;
    int volume = p->src->info.imageType == VK_IMAGE_TYPE_3D;
    unsigned layers = volume ? (unsigned)sd : p->src->info.arrayLayers;
    const char *texture = target == PIPE_TEXTURE_1D ? "1D"
                          : target == PIPE_TEXTURE_1D_ARRAY ? "1D_ARRAY"
                          : target == PIPE_TEXTURE_2D_ARRAY ? "2D_ARRAY"
                          : target == PIPE_TEXTURE_3D ? "3D" : "2D";
    if (cube || (p->src == p->dst && smip == dmip)) {
        rc = (struct gpu_res_create){.target = target,
                                     .format = p->src->format,
                                     .bind = VIRGL_BIND_SAMPLER_VIEW | VIRGL_BIND_RENDER_TARGET,
                                     .width = (uint32_t)sw,
                                     .height = (uint32_t)sh,
                                     .depth = volume ? (uint32_t)sd : 1,
                                     .array_size = volume ? 1 : layers};
        snapshot = virgl_res_create_ex(gpu, &rc);
        if (!snapshot) {
            virgl_res_destroy(gpu, vertices);
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        for (unsigned z = 0; z < layers; z++)
            virgl_resource_copy_region(gpu, snapshot->id, 0, 0, 0, z, source, smip, 0, 0, z,
                                       (uint32_t)sw, (uint32_t)sh, 1);
        source = snapshot->id;
        smip = 0;
    }
    uint32_t vs = virgl_handle(gpu), fs = virgl_handle(gpu), ve = virgl_handle(gpu),
             blend = virgl_handle(gpu);
    uint32_t raster = virgl_handle(gpu), depth = virgl_handle(gpu), view = virgl_handle(gpu),
             sampler = virgl_handle(gpu);
    static const char vertex_shader[] =
        "VERT\nDCL IN[0]\nDCL IN[1]\nDCL OUT[0], POSITION\nDCL OUT[1], GENERIC[0]\n"
        "0: MOV OUT[0], IN[0]\n1: MOV OUT[1], IN[1]\n2: END\n";
    char fragment_shader[384];
    const char *type = zvk_format_integer(p->src->info.format) == 1   ? "UINT"
                       : zvk_format_integer(p->src->info.format) == 2 ? "SINT"
                                                                      : "FLOAT";
    snprintf(fragment_shader, sizeof fragment_shader,
             "FRAG\nDCL IN[0], GENERIC[0], LINEAR\nDCL OUT[0], COLOR[0]\n"
             "DCL SAMP[0]\nDCL SVIEW[0], %s, %s\nDCL TEMP[0]\n"
             "0: TEX TEMP[0], IN[0], SAMP[0], %s\n1: MOV OUT[0], TEMP[0]\n2: END\n",
             texture, type, texture);
    virgl_create_shader(gpu, vs, PIPE_SHADER_VERTEX, vertex_shader);
    virgl_create_shader(gpu, fs, PIPE_SHADER_FRAGMENT, fragment_shader);
    uint32_t offsets[2] = {0, 16}, bindings[2] = {0, 0},
             formats[2] = {VIRGL_FORMAT_R32G32B32A32_FLOAT, VIRGL_FORMAT_R32G32B32_FLOAT};
    virgl_create_vertex_elements(gpu, ve, 2, offsets, bindings, formats);
    uint32_t b[VIRGL_OBJ_BLEND_SIZE - 1] = {0}, rs[VIRGL_OBJ_RS_SIZE - 1] = {0},
                                      ds[VIRGL_OBJ_DSA_SIZE - 1] = {0};
    b[2] = VIRGL_OBJ_BLEND_S2_RT_COLORMASK(15);
    rs[0] = VIRGL_OBJ_RS_S0_HALF_PIXEL_CENTER(1);
    virgl_create_blend(gpu, blend, b);
    virgl_create_rasterizer(gpu, raster, rs);
    virgl_create_dsa(gpu, depth, ds);
    virgl_create_sampler_view_ex(gpu, view, source, target, p->src->format, 0,
                                 volume ? 0 : layers - 1, smip, smip, VIRGL_SWIZZLE_IDENTITY);
    unsigned filter =
        p->filter == VK_FILTER_LINEAR ? PIPE_TEX_FILTER_LINEAR : PIPE_TEX_FILTER_NEAREST;
    virgl_create_sampler_state(gpu, sampler, PIPE_TEX_WRAP_CLAMP_TO_EDGE,
                               PIPE_TEX_WRAP_CLAMP_TO_EDGE, filter, PIPE_TEX_MIPFILTER_NONE,
                               filter);
    virgl_bind_shader(gpu, vs, PIPE_SHADER_VERTEX);
    virgl_bind_shader(gpu, fs, PIPE_SHADER_FRAGMENT);
    virgl_bind_shader(gpu, 0, PIPE_SHADER_GEOMETRY);
    virgl_bind_object(gpu, ve, VIRGL_OBJECT_VERTEX_ELEMENTS);
    virgl_bind_object(gpu, blend, VIRGL_OBJECT_BLEND);
    virgl_bind_object(gpu, raster, VIRGL_OBJECT_RASTERIZER);
    virgl_bind_object(gpu, depth, VIRGL_OBJECT_DSA);
    virgl_set_sampler_views(gpu, PIPE_SHADER_FRAGMENT, 0, &view, 1);
    virgl_bind_sampler_states(gpu, PIPE_SHADER_FRAGMENT, 0, &sampler, 1);
    unsigned xmin = (unsigned)(dx > 0 ? r->dstOffsets[0].x : r->dstOffsets[1].x);
    unsigned ymin = (unsigned)(dy > 0 ? r->dstOffsets[0].y : r->dstOffsets[1].y);
    virgl_set_viewport(gpu, xmin, ymin, abs(dx), abs(dy), 0, 1, 0);
    virgl_set_scissor(gpu, xmin, ymin, xmin + abs(dx), ymin + abs(dy));
    uint32_t stride = 28, zero = 0, resource = vertices->id;
    virgl_set_vertex_buffers(gpu, 1, &stride, &zero, &resource);
    float u[2] = {(float)r->srcOffsets[dx > 0 ? 0 : 1].x / sw,
                  (float)r->srcOffsets[dx > 0 ? 1 : 0].x / sw};
    float v[2] = {(float)r->srcOffsets[dy > 0 ? 0 : 1].y / sh,
                  (float)r->srcOffsets[dy > 0 ? 1 : 0].y / sh};
    VkResult result = VK_SUCCESS;
    for (unsigned z = 0; z < (unsigned)abs(dz); z++) {
        int dst_z = p->dst->info.imageType == VK_IMAGE_TYPE_3D
                        ? (dz > 0 ? r->dstOffsets[0].z + (int)z : r->dstOffsets[0].z - 1 - (int)z)
                        : (int)r->dstSubresource.baseArrayLayer;
        float w =
            (r->srcOffsets[0].z + (z + .5f) * (r->srcOffsets[1].z - r->srcOffsets[0].z) / abs(dz)) /
            sd;
        float data[4][7];
        for (unsigned i = 0; i < 4; i++) {
            data[i][0] = (i & 1) ? 1 : -1;
            data[i][1] = (i & 2) ? 1 : -1;
            data[i][2] = 0;
            data[i][3] = 1;
            data[i][4] = u[i & 1];
            data[i][5] = target == PIPE_TEXTURE_1D_ARRAY
                              ? (float)r->srcSubresource.baseArrayLayer : v[(i >> 1) & 1];
            data[i][6] = volume ? w : (float)r->srcSubresource.baseArrayLayer;
        }
        memcpy(vertices->map, data, sizeof data);
        if (virgl_res_to_host(gpu, vertices, 0, 0, 0, 0, sizeof data, 1, 1, 0, 0, 0)) {
            result = VK_ERROR_DEVICE_LOST;
            break;
        }
        uint32_t surface = virgl_handle(gpu);
        virgl_create_surface(gpu, surface, p->dst->gpu->id, p->dst->format, dmip,
                             (uint32_t)dst_z | ((uint32_t)dst_z << 16));
        virgl_set_framebuffer(gpu, 0, &surface, 1);
        virgl_draw(gpu, PIPE_PRIM_TRIANGLE_STRIP, 0, 4, 0, 0, 3);
        virgl_destroy_object(gpu, surface, VIRGL_OBJECT_SURFACE);
    }
    if (virgl_flush_fenced(gpu))
        result = VK_ERROR_DEVICE_LOST;
    virgl_destroy_object(gpu, view, VIRGL_OBJECT_SAMPLER_VIEW);
    virgl_destroy_object(gpu, sampler, VIRGL_OBJECT_SAMPLER_STATE);
    virgl_destroy_object(gpu, ve, VIRGL_OBJECT_VERTEX_ELEMENTS);
    virgl_destroy_object(gpu, blend, VIRGL_OBJECT_BLEND);
    virgl_destroy_object(gpu, raster, VIRGL_OBJECT_RASTERIZER);
    virgl_destroy_object(gpu, depth, VIRGL_OBJECT_DSA);
    virgl_destroy_object(gpu, vs, VIRGL_OBJECT_SHADER);
    virgl_destroy_object(gpu, fs, VIRGL_OBJECT_SHADER);
    virgl_res_destroy(gpu, vertices);
    if (snapshot)
        virgl_res_destroy(gpu, snapshot);
    return result;
}
