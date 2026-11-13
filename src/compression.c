/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* BC blocks remain canonical for transfers. Only texture upload decodes them;
 * all shader evaluation, filtering and rasterization execute on the GPU. */
#include "zvk_internal.h"
#define BCDEC_IMPLEMENTATION
#define BCDEC_BC4BC5_PRECISE
#define BCDECDEF static __attribute__((unused))
#include "../vendor/bcdec/bcdec.h"

void zvk_decode_bc(VkFormat format, const uint8_t *blocks, void *pixels, uint32_t width,
                   uint32_t height)
{
    unsigned bytes = zvk_bpp(format), blocks_x = (width + 3) / 4;
    int hdr = format >= VK_FORMAT_BC4_UNORM_BLOCK && format <= VK_FORMAT_BC6H_SFLOAT_BLOCK;
    int bc4 = format == VK_FORMAT_BC4_UNORM_BLOCK || format == VK_FORMAT_BC4_SNORM_BLOCK;
    int bc5 = format == VK_FORMAT_BC5_UNORM_BLOCK || format == VK_FORMAT_BC5_SNORM_BLOCK;
    int signed_format = format == VK_FORMAT_BC4_SNORM_BLOCK ||
                        format == VK_FORMAT_BC5_SNORM_BLOCK ||
                        format == VK_FORMAT_BC6H_SFLOAT_BLOCK;
    for (unsigned by = 0; by < (height + 3) / 4; by++)
        for (unsigned bx = 0; bx < blocks_x; bx++) {
            const void *in = blocks + (by * blocks_x + bx) * bytes;
            uint8_t rgba[64];
            float values[48];
            if (format <= VK_FORMAT_BC1_RGBA_SRGB_BLOCK)
                bcdec_bc1(in, rgba, 16);
            else if (format <= VK_FORMAT_BC2_SRGB_BLOCK)
                bcdec_bc2(in, rgba, 16);
            else if (format <= VK_FORMAT_BC3_SRGB_BLOCK)
                bcdec_bc3(in, rgba, 16);
            else if (bc4)
                bcdec_bc4_float(in, values, 4, signed_format);
            else if (bc5)
                bcdec_bc5_float(in, values, 8, signed_format);
            else if (format <= VK_FORMAT_BC6H_SFLOAT_BLOCK)
                bcdec_bc6h_float(in, values, 12, signed_format);
            else
                bcdec_bc7(in, rgba, 16);
            for (unsigned y = 0; y < 4 && by * 4 + y < height; y++)
                for (unsigned x = 0; x < 4 && bx * 4 + x < width; x++) {
                    unsigned i = y * 4 + x;
                    size_t out = (size_t)(by * 4 + y) * width + bx * 4 + x;
                    if (hdr) {
                        unsigned n = bc4 ? 1 : bc5 ? 2 : 3;
                        float *p = (float *)pixels + out * 4;
                        for (unsigned c = 0; c < 4; c++)
                            p[c] = c < n ? values[i * n + c] : c == 3 ? 1 : 0;
                    } else {
                        uint8_t *p = (uint8_t *)pixels + out * 4;
                        memcpy(p, rgba + i * 4, 4);
                        if (format == VK_FORMAT_BC1_RGB_UNORM_BLOCK ||
                            format == VK_FORMAT_BC1_RGB_SRGB_BLOCK)
                            p[3] = 255;
                    }
                }
        }
}
