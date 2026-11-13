/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "zvk_internal.h"
#include <math.h>
static unsigned passed, failed;
#define CHECK(v, name)                                                                             \
    do {                                                                                           \
        if (v) {                                                                                   \
            passed++;                                                                              \
            puts("PASS " name);                                                                    \
        } else {                                                                                   \
            failed++;                                                                              \
            puts("FAIL " name);                                                                    \
        }                                                                                          \
    } while (0)
static uint32_t random_state = 0x83f4ba15u;
static uint32_t random_word(void)
{
    random_state ^= random_state << 13;
    random_state ^= random_state >> 17;
    random_state ^= random_state << 5;
    return random_state;
}
int main(void)
{
    const struct {
        float value;
        uint16_t bits;
    } halves[] = {{0, 0},
                  {-0.0f, 0x8000},
                  {1, 0x3c00},
                  {-2, 0xc000},
                  {65504, 0x7bff},
                  {0x1p-24f, 1},
                  {0x1p-25f, 0},
                  {0x1.8p-24f, 2},
                  {0x1.002p0f, 0x3c00},
                  {0x1.006p0f, 0x3c02},
                  {INFINITY, 0x7c00},
                  {-INFINITY, 0xfc00}};
    int ok = 1;
    for (unsigned i = 0; i < ZVK_ARRAY_SIZE(halves); i++) {
        VkClearColorValue color = {.float32 = {halves[i].value, 0, 0, 0}};
        uint8_t texel[16];
        uint16_t actual;
        zvk_pack_clear(VK_FORMAT_R16_SFLOAT, &color, texel);
        memcpy(&actual, texel, 2);
        if (actual != halves[i].bits)
            ok = 0;
    }
    CHECK(ok, "half conversion handles IEEE specials, subnormals and ties to even");
    VkClearColorValue color = {.float32 = {-INFINITY, INFINITY, NAN, 1}};
    uint8_t texel[16];
    uint32_t packed;
    zvk_pack_clear(VK_FORMAT_B10G11R11_UFLOAT_PACK32, &color, texel);
    memcpy(&packed, texel, 4);
    CHECK(!(packed & 2047) && ((packed >> 11) & 2047) == 1984 && ((packed >> 22) & 992) == 992 &&
              (packed >> 22 & 31),
          "unsigned packed float clamps negative infinity and preserves positive infinity/NaN");
    color = (VkClearColorValue){.int32 = {-1000, 1000, -128, 127}};
    zvk_pack_clear(VK_FORMAT_R8G8B8A8_SINT, &color, texel);
    CHECK(texel[0] == 128 && texel[1] == 127 && texel[2] == 128 && texel[3] == 127,
          "signed narrow clears clamp to representable endpoints");
    color = (VkClearColorValue){.uint32 = {256, UINT32_MAX, 3, 1}};
    zvk_pack_clear(VK_FORMAT_R8G8B8A8_UINT, &color, texel);
    CHECK(texel[0] == 255 && texel[1] == 255 && texel[2] == 3 && texel[3] == 1,
          "unsigned narrow clears clamp to representable endpoints");

    unsigned mutations = 0;
    ok = 1;
    for (VkFormat format = VK_FORMAT_BC1_RGB_UNORM_BLOCK; format <= VK_FORMAT_BC7_SRGB_BLOCK;
         format++) {
        unsigned bytes = zvk_bpp(format);
        uint8_t *block = malloc(bytes);
        for (unsigned n = 0; n < 2048; n++) {
            for (unsigned i = 0; i < bytes; i++)
                block[i] = (uint8_t)random_word();
            unsigned width = 1 + (random_word() % 4), height = 1 + (random_word() % 4);
            union {
                long double align;
                uint8_t bytes[16 + 256 + 16];
            } guarded;
            memset(&guarded, 0xa5, sizeof guarded);
            int hdr = format >= VK_FORMAT_BC4_UNORM_BLOCK && format <= VK_FORMAT_BC6H_SFLOAT_BLOCK;
            unsigned output_bytes = width * height * (hdr ? 16 : 4);
            zvk_decode_bc(format, block, guarded.bytes + 16, width, height);
            for (unsigned i = 0; i < 16; i++)
                if (guarded.bytes[i] != 0xa5)
                    ok = 0;
            for (unsigned i = 16 + output_bytes; i < sizeof guarded.bytes; i++)
                if (guarded.bytes[i] != 0xa5)
                    ok = 0;
            if (hdr)
                for (unsigned i = 0; i < width * height * 4; i++)
                    if (!isfinite(((float *)(guarded.bytes + 16))[i]))
                        ok = 0;
            mutations++;
        }
        free(block);
    }
    CHECK(ok, "all BC decoders handle random/reserved modes and clipped edge blocks without "
              "touching guards");
    printf("FORMAT_RESULT %u passed %u failed (%u random blocks)\n", passed, failed, mutations);
    return failed ? 1 : 0;
}
