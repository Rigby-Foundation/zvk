/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (C) 2026 Rigby Foundation */
#include "zvk_internal.h"
#include <math.h>

/* Vulkan packed names run from most to least significant component; virgl
 * array formats describe components in memory order. Offsets below are bits
 * from the least significant byte of a little-endian texel. */
static const zvk_format_info formats[] = {
    {VK_FORMAT_BC1_RGB_UNORM_BLOCK,
     VIRGL_FORMAT_R8G8B8A8_UNORM,
     ZVK_FMT_BC,
     8,
     {0, 0, 0, 0},
     {0, 0, 0, 0},
     VK_IMAGE_ASPECT_COLOR_BIT},
    {VK_FORMAT_BC1_RGB_SRGB_BLOCK,
     VIRGL_FORMAT_R8G8B8A8_SRGB,
     ZVK_FMT_BC,
     8,
     {0, 0, 0, 0},
     {0, 0, 0, 0},
     VK_IMAGE_ASPECT_COLOR_BIT},
    {VK_FORMAT_BC1_RGBA_UNORM_BLOCK,
     VIRGL_FORMAT_R8G8B8A8_UNORM,
     ZVK_FMT_BC,
     8,
     {0, 0, 0, 0},
     {0, 0, 0, 0},
     VK_IMAGE_ASPECT_COLOR_BIT},
    {VK_FORMAT_BC1_RGBA_SRGB_BLOCK,
     VIRGL_FORMAT_R8G8B8A8_SRGB,
     ZVK_FMT_BC,
     8,
     {0, 0, 0, 0},
     {0, 0, 0, 0},
     VK_IMAGE_ASPECT_COLOR_BIT},
    {VK_FORMAT_BC2_UNORM_BLOCK,
     VIRGL_FORMAT_R8G8B8A8_UNORM,
     ZVK_FMT_BC,
     16,
     {0, 0, 0, 0},
     {0, 0, 0, 0},
     VK_IMAGE_ASPECT_COLOR_BIT},
    {VK_FORMAT_BC2_SRGB_BLOCK,
     VIRGL_FORMAT_R8G8B8A8_SRGB,
     ZVK_FMT_BC,
     16,
     {0, 0, 0, 0},
     {0, 0, 0, 0},
     VK_IMAGE_ASPECT_COLOR_BIT},
    {VK_FORMAT_BC3_UNORM_BLOCK,
     VIRGL_FORMAT_R8G8B8A8_UNORM,
     ZVK_FMT_BC,
     16,
     {0, 0, 0, 0},
     {0, 0, 0, 0},
     VK_IMAGE_ASPECT_COLOR_BIT},
    {VK_FORMAT_BC3_SRGB_BLOCK,
     VIRGL_FORMAT_R8G8B8A8_SRGB,
     ZVK_FMT_BC,
     16,
     {0, 0, 0, 0},
     {0, 0, 0, 0},
     VK_IMAGE_ASPECT_COLOR_BIT},
    {VK_FORMAT_BC4_UNORM_BLOCK,
     VIRGL_FORMAT_R32G32B32A32_FLOAT,
     ZVK_FMT_BC,
     8,
     {0, 0, 0, 0},
     {0, 0, 0, 0},
     VK_IMAGE_ASPECT_COLOR_BIT},
    {VK_FORMAT_BC4_SNORM_BLOCK,
     VIRGL_FORMAT_R32G32B32A32_FLOAT,
     ZVK_FMT_BC,
     8,
     {0, 0, 0, 0},
     {0, 0, 0, 0},
     VK_IMAGE_ASPECT_COLOR_BIT},
    {VK_FORMAT_BC5_UNORM_BLOCK,
     VIRGL_FORMAT_R32G32B32A32_FLOAT,
     ZVK_FMT_BC,
     16,
     {0, 0, 0, 0},
     {0, 0, 0, 0},
     VK_IMAGE_ASPECT_COLOR_BIT},
    {VK_FORMAT_BC5_SNORM_BLOCK,
     VIRGL_FORMAT_R32G32B32A32_FLOAT,
     ZVK_FMT_BC,
     16,
     {0, 0, 0, 0},
     {0, 0, 0, 0},
     VK_IMAGE_ASPECT_COLOR_BIT},
    {VK_FORMAT_BC6H_UFLOAT_BLOCK,
     VIRGL_FORMAT_R32G32B32A32_FLOAT,
     ZVK_FMT_BC,
     16,
     {0, 0, 0, 0},
     {0, 0, 0, 0},
     VK_IMAGE_ASPECT_COLOR_BIT},
    {VK_FORMAT_BC6H_SFLOAT_BLOCK,
     VIRGL_FORMAT_R32G32B32A32_FLOAT,
     ZVK_FMT_BC,
     16,
     {0, 0, 0, 0},
     {0, 0, 0, 0},
     VK_IMAGE_ASPECT_COLOR_BIT},
    {VK_FORMAT_BC7_UNORM_BLOCK,
     VIRGL_FORMAT_R8G8B8A8_UNORM,
     ZVK_FMT_BC,
     16,
     {0, 0, 0, 0},
     {0, 0, 0, 0},
     VK_IMAGE_ASPECT_COLOR_BIT},
    {VK_FORMAT_BC7_SRGB_BLOCK,
     VIRGL_FORMAT_R8G8B8A8_SRGB,
     ZVK_FMT_BC,
     16,
     {0, 0, 0, 0},
     {0, 0, 0, 0},
     VK_IMAGE_ASPECT_COLOR_BIT},
    {VK_FORMAT_R8_UNORM,
     VIRGL_FORMAT_R8_UNORM,
     ZVK_FMT_UNORM,
     1,
     {8, 0, 0, 0},
     {0, 0, 0, 0},
     VK_IMAGE_ASPECT_COLOR_BIT},
    {VK_FORMAT_R8_SNORM,
     VIRGL_FORMAT_R8_SNORM,
     ZVK_FMT_SNORM,
     1,
     {8, 0, 0, 0},
     {0, 0, 0, 0},
     VK_IMAGE_ASPECT_COLOR_BIT},
    {VK_FORMAT_R8_UINT,
     VIRGL_FORMAT_R8_UINT,
     ZVK_FMT_UINT,
     1,
     {8, 0, 0, 0},
     {0, 0, 0, 0},
     VK_IMAGE_ASPECT_COLOR_BIT},
    {VK_FORMAT_R8_SINT,
     VIRGL_FORMAT_R8_SINT,
     ZVK_FMT_SINT,
     1,
     {8, 0, 0, 0},
     {0, 0, 0, 0},
     VK_IMAGE_ASPECT_COLOR_BIT},
    {VK_FORMAT_R8_SRGB,
     VIRGL_FORMAT_R8_SRGB,
     ZVK_FMT_SRGB,
     1,
     {8, 0, 0, 0},
     {0, 0, 0, 0},
     VK_IMAGE_ASPECT_COLOR_BIT},
    {VK_FORMAT_R8G8_UNORM,
     VIRGL_FORMAT_R8G8_UNORM,
     ZVK_FMT_UNORM,
     2,
     {8, 8, 0, 0},
     {0, 8, 0, 0},
     VK_IMAGE_ASPECT_COLOR_BIT},
    {VK_FORMAT_R8G8_SNORM,
     VIRGL_FORMAT_R8G8_SNORM,
     ZVK_FMT_SNORM,
     2,
     {8, 8, 0, 0},
     {0, 8, 0, 0},
     VK_IMAGE_ASPECT_COLOR_BIT},
    {VK_FORMAT_R8G8_UINT,
     VIRGL_FORMAT_R8G8_UINT,
     ZVK_FMT_UINT,
     2,
     {8, 8, 0, 0},
     {0, 8, 0, 0},
     VK_IMAGE_ASPECT_COLOR_BIT},
    {VK_FORMAT_R8G8_SINT,
     VIRGL_FORMAT_R8G8_SINT,
     ZVK_FMT_SINT,
     2,
     {8, 8, 0, 0},
     {0, 8, 0, 0},
     VK_IMAGE_ASPECT_COLOR_BIT},
    {VK_FORMAT_R8G8_SRGB,
     VIRGL_FORMAT_R8G8_SRGB,
     ZVK_FMT_SRGB,
     2,
     {8, 8, 0, 0},
     {0, 8, 0, 0},
     VK_IMAGE_ASPECT_COLOR_BIT},
    {VK_FORMAT_R8G8B8_UNORM,
     VIRGL_FORMAT_R8G8B8_UNORM,
     ZVK_FMT_UNORM,
     3,
     {8, 8, 8, 0},
     {0, 8, 16, 0},
     VK_IMAGE_ASPECT_COLOR_BIT},
    {VK_FORMAT_R8G8B8_SNORM,
     VIRGL_FORMAT_R8G8B8_SNORM,
     ZVK_FMT_SNORM,
     3,
     {8, 8, 8, 0},
     {0, 8, 16, 0},
     VK_IMAGE_ASPECT_COLOR_BIT},
    {VK_FORMAT_R8G8B8_UINT,
     VIRGL_FORMAT_R8G8B8_UINT,
     ZVK_FMT_UINT,
     3,
     {8, 8, 8, 0},
     {0, 8, 16, 0},
     VK_IMAGE_ASPECT_COLOR_BIT},
    {VK_FORMAT_R8G8B8_SINT,
     VIRGL_FORMAT_R8G8B8_SINT,
     ZVK_FMT_SINT,
     3,
     {8, 8, 8, 0},
     {0, 8, 16, 0},
     VK_IMAGE_ASPECT_COLOR_BIT},
    {VK_FORMAT_R8G8B8_SRGB,
     VIRGL_FORMAT_R8G8B8_SRGB,
     ZVK_FMT_SRGB,
     3,
     {8, 8, 8, 0},
     {0, 8, 16, 0},
     VK_IMAGE_ASPECT_COLOR_BIT},
    {VK_FORMAT_R8G8B8A8_UNORM,
     VIRGL_FORMAT_R8G8B8A8_UNORM,
     ZVK_FMT_UNORM,
     4,
     {8, 8, 8, 8},
     {0, 8, 16, 24},
     VK_IMAGE_ASPECT_COLOR_BIT},
    {VK_FORMAT_R8G8B8A8_SNORM,
     VIRGL_FORMAT_R8G8B8A8_SNORM,
     ZVK_FMT_SNORM,
     4,
     {8, 8, 8, 8},
     {0, 8, 16, 24},
     VK_IMAGE_ASPECT_COLOR_BIT},
    {VK_FORMAT_R8G8B8A8_UINT,
     VIRGL_FORMAT_R8G8B8A8_UINT,
     ZVK_FMT_UINT,
     4,
     {8, 8, 8, 8},
     {0, 8, 16, 24},
     VK_IMAGE_ASPECT_COLOR_BIT},
    {VK_FORMAT_R8G8B8A8_SINT,
     VIRGL_FORMAT_R8G8B8A8_SINT,
     ZVK_FMT_SINT,
     4,
     {8, 8, 8, 8},
     {0, 8, 16, 24},
     VK_IMAGE_ASPECT_COLOR_BIT},
    {VK_FORMAT_R8G8B8A8_SRGB,
     VIRGL_FORMAT_R8G8B8A8_SRGB,
     ZVK_FMT_SRGB,
     4,
     {8, 8, 8, 8},
     {0, 8, 16, 24},
     VK_IMAGE_ASPECT_COLOR_BIT},
    {VK_FORMAT_R16_UNORM,
     VIRGL_FORMAT_R16_UNORM,
     ZVK_FMT_UNORM,
     2,
     {16, 0, 0, 0},
     {0, 0, 0, 0},
     VK_IMAGE_ASPECT_COLOR_BIT},
    {VK_FORMAT_R16_SNORM,
     VIRGL_FORMAT_R16_SNORM,
     ZVK_FMT_SNORM,
     2,
     {16, 0, 0, 0},
     {0, 0, 0, 0},
     VK_IMAGE_ASPECT_COLOR_BIT},
    {VK_FORMAT_R16_UINT,
     VIRGL_FORMAT_R16_UINT,
     ZVK_FMT_UINT,
     2,
     {16, 0, 0, 0},
     {0, 0, 0, 0},
     VK_IMAGE_ASPECT_COLOR_BIT},
    {VK_FORMAT_R16_SINT,
     VIRGL_FORMAT_R16_SINT,
     ZVK_FMT_SINT,
     2,
     {16, 0, 0, 0},
     {0, 0, 0, 0},
     VK_IMAGE_ASPECT_COLOR_BIT},
    {VK_FORMAT_R16_SFLOAT,
     VIRGL_FORMAT_R16_FLOAT,
     ZVK_FMT_FLOAT,
     2,
     {16, 0, 0, 0},
     {0, 0, 0, 0},
     VK_IMAGE_ASPECT_COLOR_BIT},
    {VK_FORMAT_R16G16_UNORM,
     VIRGL_FORMAT_R16G16_UNORM,
     ZVK_FMT_UNORM,
     4,
     {16, 16, 0, 0},
     {0, 16, 0, 0},
     VK_IMAGE_ASPECT_COLOR_BIT},
    {VK_FORMAT_R16G16_SNORM,
     VIRGL_FORMAT_R16G16_SNORM,
     ZVK_FMT_SNORM,
     4,
     {16, 16, 0, 0},
     {0, 16, 0, 0},
     VK_IMAGE_ASPECT_COLOR_BIT},
    {VK_FORMAT_R16G16_UINT,
     VIRGL_FORMAT_R16G16_UINT,
     ZVK_FMT_UINT,
     4,
     {16, 16, 0, 0},
     {0, 16, 0, 0},
     VK_IMAGE_ASPECT_COLOR_BIT},
    {VK_FORMAT_R16G16_SINT,
     VIRGL_FORMAT_R16G16_SINT,
     ZVK_FMT_SINT,
     4,
     {16, 16, 0, 0},
     {0, 16, 0, 0},
     VK_IMAGE_ASPECT_COLOR_BIT},
    {VK_FORMAT_R16G16_SFLOAT,
     VIRGL_FORMAT_R16G16_FLOAT,
     ZVK_FMT_FLOAT,
     4,
     {16, 16, 0, 0},
     {0, 16, 0, 0},
     VK_IMAGE_ASPECT_COLOR_BIT},
    {VK_FORMAT_R16G16B16_UNORM,
     VIRGL_FORMAT_R16G16B16_UNORM,
     ZVK_FMT_UNORM,
     6,
     {16, 16, 16, 0},
     {0, 16, 32, 0},
     VK_IMAGE_ASPECT_COLOR_BIT},
    {VK_FORMAT_R16G16B16_SNORM,
     VIRGL_FORMAT_R16G16B16_SNORM,
     ZVK_FMT_SNORM,
     6,
     {16, 16, 16, 0},
     {0, 16, 32, 0},
     VK_IMAGE_ASPECT_COLOR_BIT},
    {VK_FORMAT_R16G16B16_UINT,
     VIRGL_FORMAT_R16G16B16_UINT,
     ZVK_FMT_UINT,
     6,
     {16, 16, 16, 0},
     {0, 16, 32, 0},
     VK_IMAGE_ASPECT_COLOR_BIT},
    {VK_FORMAT_R16G16B16_SINT,
     VIRGL_FORMAT_R16G16B16_SINT,
     ZVK_FMT_SINT,
     6,
     {16, 16, 16, 0},
     {0, 16, 32, 0},
     VK_IMAGE_ASPECT_COLOR_BIT},
    {VK_FORMAT_R16G16B16_SFLOAT,
     VIRGL_FORMAT_R16G16B16_FLOAT,
     ZVK_FMT_FLOAT,
     6,
     {16, 16, 16, 0},
     {0, 16, 32, 0},
     VK_IMAGE_ASPECT_COLOR_BIT},
    {VK_FORMAT_R16G16B16A16_UNORM,
     VIRGL_FORMAT_R16G16B16A16_UNORM,
     ZVK_FMT_UNORM,
     8,
     {16, 16, 16, 16},
     {0, 16, 32, 48},
     VK_IMAGE_ASPECT_COLOR_BIT},
    {VK_FORMAT_R16G16B16A16_SNORM,
     VIRGL_FORMAT_R16G16B16A16_SNORM,
     ZVK_FMT_SNORM,
     8,
     {16, 16, 16, 16},
     {0, 16, 32, 48},
     VK_IMAGE_ASPECT_COLOR_BIT},
    {VK_FORMAT_R16G16B16A16_UINT,
     VIRGL_FORMAT_R16G16B16A16_UINT,
     ZVK_FMT_UINT,
     8,
     {16, 16, 16, 16},
     {0, 16, 32, 48},
     VK_IMAGE_ASPECT_COLOR_BIT},
    {VK_FORMAT_R16G16B16A16_SINT,
     VIRGL_FORMAT_R16G16B16A16_SINT,
     ZVK_FMT_SINT,
     8,
     {16, 16, 16, 16},
     {0, 16, 32, 48},
     VK_IMAGE_ASPECT_COLOR_BIT},
    {VK_FORMAT_R16G16B16A16_SFLOAT,
     VIRGL_FORMAT_R16G16B16A16_FLOAT,
     ZVK_FMT_FLOAT,
     8,
     {16, 16, 16, 16},
     {0, 16, 32, 48},
     VK_IMAGE_ASPECT_COLOR_BIT},
    {VK_FORMAT_R32_UINT,
     VIRGL_FORMAT_R32_UINT,
     ZVK_FMT_UINT,
     4,
     {32, 0, 0, 0},
     {0, 0, 0, 0},
     VK_IMAGE_ASPECT_COLOR_BIT},
    {VK_FORMAT_R32_SINT,
     VIRGL_FORMAT_R32_SINT,
     ZVK_FMT_SINT,
     4,
     {32, 0, 0, 0},
     {0, 0, 0, 0},
     VK_IMAGE_ASPECT_COLOR_BIT},
    {VK_FORMAT_R32_SFLOAT,
     VIRGL_FORMAT_R32_FLOAT,
     ZVK_FMT_FLOAT,
     4,
     {32, 0, 0, 0},
     {0, 0, 0, 0},
     VK_IMAGE_ASPECT_COLOR_BIT},
    {VK_FORMAT_R32G32_UINT,
     VIRGL_FORMAT_R32G32_UINT,
     ZVK_FMT_UINT,
     8,
     {32, 32, 0, 0},
     {0, 32, 0, 0},
     VK_IMAGE_ASPECT_COLOR_BIT},
    {VK_FORMAT_R32G32_SINT,
     VIRGL_FORMAT_R32G32_SINT,
     ZVK_FMT_SINT,
     8,
     {32, 32, 0, 0},
     {0, 32, 0, 0},
     VK_IMAGE_ASPECT_COLOR_BIT},
    {VK_FORMAT_R32G32_SFLOAT,
     VIRGL_FORMAT_R32G32_FLOAT,
     ZVK_FMT_FLOAT,
     8,
     {32, 32, 0, 0},
     {0, 32, 0, 0},
     VK_IMAGE_ASPECT_COLOR_BIT},
    {VK_FORMAT_R32G32B32_UINT,
     VIRGL_FORMAT_R32G32B32_UINT,
     ZVK_FMT_UINT,
     12,
     {32, 32, 32, 0},
     {0, 32, 64, 0},
     VK_IMAGE_ASPECT_COLOR_BIT},
    {VK_FORMAT_R32G32B32_SINT,
     VIRGL_FORMAT_R32G32B32_SINT,
     ZVK_FMT_SINT,
     12,
     {32, 32, 32, 0},
     {0, 32, 64, 0},
     VK_IMAGE_ASPECT_COLOR_BIT},
    {VK_FORMAT_R32G32B32_SFLOAT,
     VIRGL_FORMAT_R32G32B32_FLOAT,
     ZVK_FMT_FLOAT,
     12,
     {32, 32, 32, 0},
     {0, 32, 64, 0},
     VK_IMAGE_ASPECT_COLOR_BIT},
    {VK_FORMAT_R32G32B32A32_UINT,
     VIRGL_FORMAT_R32G32B32A32_UINT,
     ZVK_FMT_UINT,
     16,
     {32, 32, 32, 32},
     {0, 32, 64, 96},
     VK_IMAGE_ASPECT_COLOR_BIT},
    {VK_FORMAT_R32G32B32A32_SINT,
     VIRGL_FORMAT_R32G32B32A32_SINT,
     ZVK_FMT_SINT,
     16,
     {32, 32, 32, 32},
     {0, 32, 64, 96},
     VK_IMAGE_ASPECT_COLOR_BIT},
    {VK_FORMAT_R32G32B32A32_SFLOAT,
     VIRGL_FORMAT_R32G32B32A32_FLOAT,
     ZVK_FMT_FLOAT,
     16,
     {32, 32, 32, 32},
     {0, 32, 64, 96},
     VK_IMAGE_ASPECT_COLOR_BIT},
    {VK_FORMAT_B8G8R8A8_UNORM,
     VIRGL_FORMAT_B8G8R8A8_UNORM,
     ZVK_FMT_UNORM,
     4,
     {8, 8, 8, 8},
     {16, 8, 0, 24},
     VK_IMAGE_ASPECT_COLOR_BIT},
    {VK_FORMAT_B8G8R8A8_SRGB,
     VIRGL_FORMAT_B8G8R8A8_SRGB,
     ZVK_FMT_SRGB,
     4,
     {8, 8, 8, 8},
     {16, 8, 0, 24},
     VK_IMAGE_ASPECT_COLOR_BIT},
    {VK_FORMAT_A8B8G8R8_UNORM_PACK32,
     VIRGL_FORMAT_R8G8B8A8_UNORM,
     ZVK_FMT_UNORM,
     4,
     {8, 8, 8, 8},
     {0, 8, 16, 24},
     VK_IMAGE_ASPECT_COLOR_BIT},
    {VK_FORMAT_A8B8G8R8_SNORM_PACK32,
     VIRGL_FORMAT_R8G8B8A8_SNORM,
     ZVK_FMT_SNORM,
     4,
     {8, 8, 8, 8},
     {0, 8, 16, 24},
     VK_IMAGE_ASPECT_COLOR_BIT},
    {VK_FORMAT_A8B8G8R8_UINT_PACK32,
     VIRGL_FORMAT_R8G8B8A8_UINT,
     ZVK_FMT_UINT,
     4,
     {8, 8, 8, 8},
     {0, 8, 16, 24},
     VK_IMAGE_ASPECT_COLOR_BIT},
    {VK_FORMAT_A8B8G8R8_SINT_PACK32,
     VIRGL_FORMAT_R8G8B8A8_SINT,
     ZVK_FMT_SINT,
     4,
     {8, 8, 8, 8},
     {0, 8, 16, 24},
     VK_IMAGE_ASPECT_COLOR_BIT},
    {VK_FORMAT_A8B8G8R8_SRGB_PACK32,
     VIRGL_FORMAT_R8G8B8A8_SRGB,
     ZVK_FMT_SRGB,
     4,
     {8, 8, 8, 8},
     {0, 8, 16, 24},
     VK_IMAGE_ASPECT_COLOR_BIT},
    {VK_FORMAT_R5G6B5_UNORM_PACK16,
     VIRGL_FORMAT_B5G6R5_UNORM,
     ZVK_FMT_UNORM,
     2,
     {5, 6, 5, 0},
     {11, 5, 0, 0},
     VK_IMAGE_ASPECT_COLOR_BIT},
    {VK_FORMAT_A1R5G5B5_UNORM_PACK16,
     VIRGL_FORMAT_B5G5R5A1_UNORM,
     ZVK_FMT_UNORM,
     2,
     {5, 5, 5, 1},
     {10, 5, 0, 15},
     VK_IMAGE_ASPECT_COLOR_BIT},
    {VK_FORMAT_B4G4R4A4_UNORM_PACK16,
     VIRGL_FORMAT_A4R4G4B4_UNORM,
     ZVK_FMT_UNORM,
     2,
     {4, 4, 4, 4},
     {4, 8, 12, 0},
     VK_IMAGE_ASPECT_COLOR_BIT},
    {VK_FORMAT_A2B10G10R10_UNORM_PACK32,
     VIRGL_FORMAT_R10G10B10A2_UNORM,
     ZVK_FMT_UNORM,
     4,
     {10, 10, 10, 2},
     {0, 10, 20, 30},
     VK_IMAGE_ASPECT_COLOR_BIT},
    {VK_FORMAT_A2B10G10R10_UINT_PACK32,
     VIRGL_FORMAT_R10G10B10A2_UINT,
     ZVK_FMT_UINT,
     4,
     {10, 10, 10, 2},
     {0, 10, 20, 30},
     VK_IMAGE_ASPECT_COLOR_BIT},
    {VK_FORMAT_B10G11R11_UFLOAT_PACK32,
     VIRGL_FORMAT_R11G11B10_FLOAT,
     ZVK_FMT_UFLOAT,
     4,
     {11, 11, 10, 0},
     {0, 11, 22, 0},
     VK_IMAGE_ASPECT_COLOR_BIT},
    {VK_FORMAT_E5B9G9R9_UFLOAT_PACK32,
     VIRGL_FORMAT_R9G9B9E5_FLOAT,
     ZVK_FMT_SHARED,
     4,
     {9, 9, 9, 0},
     {0, 9, 18, 0},
     VK_IMAGE_ASPECT_COLOR_BIT},
    {VK_FORMAT_D16_UNORM,
     VIRGL_FORMAT_Z16_UNORM,
     ZVK_FMT_UNORM,
     2,
     {16, 0, 0, 0},
     {0, 0, 0, 0},
     VK_IMAGE_ASPECT_DEPTH_BIT},
    {VK_FORMAT_D32_SFLOAT,
     VIRGL_FORMAT_Z32_FLOAT,
     ZVK_FMT_FLOAT,
     4,
     {32, 0, 0, 0},
     {0, 0, 0, 0},
     VK_IMAGE_ASPECT_DEPTH_BIT},
    {VK_FORMAT_D24_UNORM_S8_UINT,
     VIRGL_FORMAT_Z24_UNORM_S8_UINT,
     ZVK_FMT_UINT,
     4,
     {24, 8, 0, 0},
     {0, 24, 0, 0},
     VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT},
};
const zvk_format_info *zvk_get_format_info(VkFormat format)
{
    for (unsigned i = 0; i < ZVK_ARRAY_SIZE(formats); i++)
        if (formats[i].format == format)
            return formats + i;
    return NULL;
}
uint32_t zvk_format(VkFormat format)
{
    const zvk_format_info *f = zvk_get_format_info(format);
    return f ? f->backend : 0;
}
uint32_t zvk_bpp(VkFormat format)
{
    const zvk_format_info *f = zvk_get_format_info(format);
    return f ? f->bpp : 0;
}
VkImageAspectFlags zvk_format_aspects(VkFormat format)
{
    const zvk_format_info *f = zvk_get_format_info(format);
    return f ? f->aspects : 0;
}
int zvk_format_integer(VkFormat format)
{
    const zvk_format_info *f = zvk_get_format_info(format);
    return !f ? 0 : f->type == ZVK_FMT_UINT ? 1 : f->type == ZVK_FMT_SINT ? 2 : 0;
}
static uint32_t round_shift(uint32_t value, unsigned shift)
{
    if (!shift)
        return value;
    if (shift > 31)
        return 0;
    uint32_t q = value >> shift, mask = (1u << shift) - 1, half = 1u << (shift - 1);
    return q + ((value & mask) > half || ((value & mask) == half && (q & 1)));
}
static uint32_t float_pack(float value, unsigned bits, int sign)
{
    uint32_t raw;
    memcpy(&raw, &value, 4);
    unsigned mantissa = bits - 5 - sign;
    uint32_t s = sign ? (raw >> 31) << (bits - 1) : 0;
    unsigned exp = (raw >> 23) & 255, mant = raw & 0x7fffff;
    if (!sign && (raw >> 31) && !(exp == 255 && mant))
        return 0;
    if (exp == 255)
        return s | (31u << mantissa) | (mant ? 1u << (mantissa - 1) : 0);
    int adjusted = (int)exp - 127 + 15;
    if (adjusted >= 31)
        return s | (31u << mantissa);
    if (adjusted <= 0)
        return s | round_shift(mant | (exp ? 0x800000u : 0), 24 - mantissa - adjusted);
    uint32_t q = round_shift(mant, 23 - mantissa);
    return s | (((uint32_t)adjusted << mantissa) + q);
}
void zvk_pack_clear(VkFormat format, const VkClearColorValue *color, uint8_t texel[16])
{
    const zvk_format_info *f = zvk_get_format_info(format);
    memset(texel, 0, 16);
    if (!f)
        return;
    if (f->type == ZVK_FMT_SHARED) {
        float v[3], max = 0;
        for (unsigned i = 0; i < 3; i++) {
            v[i] = fminf(fmaxf(color->float32[i], 0), 65408);
            if (v[i] > max)
                max = v[i];
        }
        int exp = max > 0 ? (int)floorf(log2f(max)) : -16;
        if (exp < -16)
            exp = -16;
        exp += 16;
        float scale = ldexpf(1, exp - 24);
        if (floorf(max / scale + 0.5f) == 512) {
            exp++;
            scale *= 2;
        }
        uint32_t packed = (uint32_t)exp << 27;
        for (unsigned i = 0; i < 3; i++)
            packed |= (uint32_t)floorf(v[i] / scale + 0.5f) << (9 * i);
        memcpy(texel, &packed, 4);
        return;
    }
    for (unsigned c = 0; c < 4; c++) {
        unsigned bits = f->bits[c];
        if (!bits)
            continue;
        uint32_t max = bits == 32 ? UINT32_MAX : (1u << bits) - 1, value = 0;
        if (f->type == ZVK_FMT_UINT)
            value = color->uint32[c] > max ? max : color->uint32[c];
        else if (f->type == ZVK_FMT_SINT) {
            int64_t lo = -(1ll << (bits - 1)), hi = (1ll << (bits - 1)) - 1, v = color->int32[c];
            if (v < lo)
                v = lo;
            if (v > hi)
                v = hi;
            value = (uint32_t)v & max;
        } else if (f->type == ZVK_FMT_FLOAT) {
            if (bits == 32)
                value = color->uint32[c];
            else
                value = float_pack(color->float32[c], bits, 1);
        } else if (f->type == ZVK_FMT_UFLOAT)
            value = float_pack(color->float32[c], bits, 0);
        else {
            float v = color->float32[c];
            if (f->type == ZVK_FMT_SRGB && c < 3)
                v = v <= 0.0031308f ? 12.92f * v : 1.055f * powf(v, 1.0f / 2.4f) - 0.055f;
            if (f->type == ZVK_FMT_SNORM) {
                int32_t hi = (int32_t)(max >> 1);
                v = fminf(fmaxf(v, -1), 1);
                value = (uint32_t)(int32_t)roundf(v * hi) & max;
            } else {
                v = fminf(fmaxf(v, 0), 1);
                value = (uint32_t)floorf(v * (float)max + 0.5f);
            }
        }
        for (unsigned b = 0; b < bits; b++)
            if (value & (1u << b))
                texel[(f->shift[c] + b) / 8] |= 1u << ((f->shift[c] + b) % 8);
    }
}

unsigned zvk_block(VkFormat format)
{
    const zvk_format_info *f = zvk_get_format_info(format);
    return f && f->type == ZVK_FMT_BC ? 4 : 1;
}
int zvk_bc_supported(VkPhysicalDevice physical)
{
    const unsigned required[] = {VIRGL_FORMAT_R8G8B8A8_UNORM, VIRGL_FORMAT_R8G8B8A8_SRGB,
                                 VIRGL_FORMAT_R32G32B32A32_FLOAT};
    for (unsigned i = 0; i < ZVK_ARRAY_SIZE(required); i++)
        if (!(physical->caps.v1.sampler.bitmask[required[i] / 32] & (1u << (required[i] % 32))))
            return 0;
    return 1;
}
uint32_t zvk_block_count(VkFormat format, uint32_t pixels)
{
    unsigned block = zvk_block(format);
    return (pixels + block - 1) / block;
}
VkDeviceSize zvk_level_size(VkImage image, uint32_t mip)
{
    return (VkDeviceSize)zvk_block_count(image->info.format,
                                         zvk_mip(image->info.extent.width, mip)) *
           zvk_block_count(image->info.format, zvk_mip(image->info.extent.height, mip)) *
           zvk_mip(image->info.extent.depth, mip) * image->bpp;
}
int zvk_block_region(VkImage image, uint32_t mip, int32_t x, int32_t y, uint32_t width,
                     uint32_t height)
{
    unsigned block = zvk_block(image->info.format);
    return !(x % block) && !(y % block) &&
           (!(width % block) || (uint32_t)x + width == zvk_mip(image->info.extent.width, mip)) &&
           (!(height % block) || (uint32_t)y + height == zvk_mip(image->info.extent.height, mip));
}
unsigned zvk_gpu_bpp(VkImage image)
{
    if (zvk_block(image->info.format) == 4)
        return image->format == VIRGL_FORMAT_R32G32B32A32_FLOAT ? 16 : 4;
    return image->transfer_map ? 4 : image->bpp;
}
VkDeviceSize zvk_gpu_image_offset(VkImage image, uint32_t mip, uint32_t layer)
{
    VkDeviceSize offset = 0;
    unsigned bpp = zvk_gpu_bpp(image);
    for (unsigned m = 0; m < mip; m++)
        offset += (VkDeviceSize)zvk_mip(image->info.extent.width, m) *
                  zvk_mip(image->info.extent.height, m) * zvk_mip(image->info.extent.depth, m) *
                  bpp * image->info.arrayLayers;
    return offset + (VkDeviceSize)layer * zvk_mip(image->info.extent.width, mip) *
                        zvk_mip(image->info.extent.height, mip) *
                        zvk_mip(image->info.extent.depth, mip) * bpp;
}
