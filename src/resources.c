/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (C) 2026 Rigby Foundation */
#include "zvk_internal.h"
#include <math.h>

uint32_t zvk_mip(uint32_t size, uint32_t level)
{
    return level < 32 && (size >> level) ? size >> level : 1;
}
VkDeviceSize zvk_image_offset(VkImage image, uint32_t mip, uint32_t layer)
{
    return image->level_offsets[mip] + layer * zvk_level_size(image, mip);
}
int zvk_image_range(VkImage image, const VkImageSubresourceRange *range,
                    VkImageSubresourceRange *out)
{
    *out = *range;
    if (range->baseMipLevel >= image->info.mipLevels ||
        range->baseArrayLayer >= image->info.arrayLayers)
        return 0;
    if (out->levelCount == VK_REMAINING_MIP_LEVELS)
        out->levelCount = image->info.mipLevels - range->baseMipLevel;
    if (out->layerCount == VK_REMAINING_ARRAY_LAYERS)
        out->layerCount = image->info.arrayLayers - range->baseArrayLayer;
    VkImageAspectFlags valid = zvk_format_aspects(image->info.format);
    return out->levelCount && out->layerCount && out->aspectMask && !(out->aspectMask & ~valid) &&
           zvk_range(image->info.mipLevels, out->baseMipLevel, out->levelCount) &&
           zvk_range(image->info.arrayLayers, out->baseArrayLayer, out->layerCount);
}
int zvk_layout_is(VkImage image, uint32_t mip, uint32_t layer, VkImageLayout layout)
{
    return image->layouts[mip * image->info.arrayLayers + layer] == layout;
}
void zvk_layout_set(VkImage image, const VkImageSubresourceRange *range, VkImageLayout layout)
{
    for (uint32_t m = range->baseMipLevel; m < range->baseMipLevel + range->levelCount; m++)
        for (uint32_t l = range->baseArrayLayer; l < range->baseArrayLayer + range->layerCount; l++)
            image->layouts[m * image->info.arrayLayers + l] = layout;
}

static int supported_format(const struct virgl_supported_format_mask *mask, uint32_t format)
{
    return format < 512 && ((mask->bitmask[format / 32] >> (format % 32)) & 1);
}
static uint32_t image_format(VkPhysicalDevice physical, VkFormat format)
{
    if (zvk_block(format) == 4 && !zvk_bc_supported(physical))
        return 0;
    uint32_t backend = zvk_format(format);
    /* GL on macOS does not expose this mandatory Vulkan packed format.
     * Keep canonical 4-bit transfer data and sample an expanded RGBA8 texture. */
    if (format == VK_FORMAT_B4G4R4A4_UNORM_PACK16)
        return VIRGL_FORMAT_R8G8B8A8_UNORM;
    if (format == VK_FORMAT_D24_UNORM_S8_UINT) {
        if ((supported_format(&physical->caps.v1.depthstencil, backend) ||
             supported_format(&physical->caps.v1.sampler, backend)))
            return backend;
        backend = VIRGL_FORMAT_S8_UINT_Z24_UNORM;
        return (supported_format(&physical->caps.v1.depthstencil, backend) ||
                supported_format(&physical->caps.v1.sampler, backend))
                   ? backend
                   : 0;
    }
    if (format == VK_FORMAT_D32_SFLOAT || format == VK_FORMAT_D16_UNORM)
        return (supported_format(&physical->caps.v1.depthstencil, backend) ||
                supported_format(&physical->caps.v1.sampler, backend))
                   ? backend
                   : 0;
    return backend && (supported_format(&physical->caps.v1.render, backend) ||
                       supported_format(&physical->caps.v1.sampler, backend))
               ? backend
               : 0;
}
VkSampleCountFlags zvk_samples(VkPhysicalDevice physical, VkFormat format)
{
    VkSampleCountFlags result = VK_SAMPLE_COUNT_1_BIT;
    if (format == VK_FORMAT_B4G4R4A4_UNORM_PACK16 || zvk_block(format) == 4)
        return result;
    uint32_t backend = image_format(physical, format);
    /* Multisample clears and resolves need an attachment-capable native
     * format. The sampler/MS mask alone also contains non-renderable formats
     * such as RGB9E5, which cannot represent this driver's MS image storage. */
    if (zvk_format_aspects(format)==VK_IMAGE_ASPECT_COLOR_BIT &&
        !supported_format(&physical->caps.v1.render,backend))return result;
    if (backend && physical->caps.v1.bset.texture_multisample &&
        supported_format(&physical->caps.supported_multisample_formats, backend)) {
        if (physical->caps.v1.max_samples >= 2)
            result |= VK_SAMPLE_COUNT_2_BIT;
        if (physical->caps.v1.max_samples >= 4)
            result |= VK_SAMPLE_COUNT_4_BIT;
    }
    return result;
}
uint32_t zvk_aspect_bpp(VkImage image, VkImageAspectFlags aspect)
{
    if (image->info.format == VK_FORMAT_D24_UNORM_S8_UINT)
        return aspect == VK_IMAGE_ASPECT_STENCIL_BIT ? 1
               : aspect == VK_IMAGE_ASPECT_DEPTH_BIT ? 4
                                                     : 0;
    if (zvk_format_aspects(image->info.format) == VK_IMAGE_ASPECT_DEPTH_BIT)
        return aspect == VK_IMAGE_ASPECT_DEPTH_BIT ? image->bpp : 0;
    return aspect == VK_IMAGE_ASPECT_COLOR_BIT ? image->bpp : 0;
}
static int color_format(VkFormat f)
{
    return zvk_format_aspects(f) == VK_IMAGE_ASPECT_COLOR_BIT;
}
static int depth_format(VkFormat f)
{
    return (zvk_format_aspects(f) & VK_IMAGE_ASPECT_DEPTH_BIT) != 0;
}
/* The compute frontend accesses texels on the GPU through their packed
 * backing. Kind 0/1/2 is uint/sint/float32; 3/4 is packed RGBA/BGRA8 UNORM. */
int zvk_texel_format(VkFormat f, uint32_t *kind, uint32_t *components)
{
    const zvk_format_info *info = zvk_get_format_info(f);
    if (!info || info->aspects != VK_IMAGE_ASPECT_COLOR_BIT || info->type == ZVK_FMT_BC)
        return 0;
    *kind = info->type;
    *components = 0;
    for (unsigned c = 0; c < 4; c++)
        if (info->bits[c])
            (*components)++;
    return 1;
}
int zvk_storage_format(VkFormat f)
{
    const zvk_format_info *info = zvk_get_format_info(f);
    if (!info || info->aspects != VK_IMAGE_ASPECT_COLOR_BIT || info->type == ZVK_FMT_SRGB ||
        info->type == ZVK_FMT_SHARED || info->type == ZVK_FMT_BC)
        return 0;
    if (f == VK_FORMAT_B10G11R11_UFLOAT_PACK32 || f == VK_FORMAT_A2B10G10R10_UNORM_PACK32 ||
        f == VK_FORMAT_A2B10G10R10_UINT_PACK32)
        return 1;
    return (info->bits[0] == 8 || info->bits[0] == 16 || info->bits[0] == 32) &&
           info->shift[0] == 0 && (!info->bits[2] || info->bits[3]);
}
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFormatProperties(VkPhysicalDevice physical,
                                                               VkFormat format,
                                                               VkFormatProperties *p)
{
    memset(p, 0, sizeof *p);
    const zvk_format_info *f = zvk_get_format_info(format);
    uint32_t backend = image_format(physical, format);
    int integer = zvk_format_integer(format);
    if (f && color_format(format) && f->type != ZVK_FMT_SRGB && f->type != ZVK_FMT_BC &&
        (f->type != ZVK_FMT_UFLOAT && f->type != ZVK_FMT_SHARED))
        p->bufferFeatures = VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT;
    if (backend) {
        p->optimalTilingFeatures =
            VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
        if (supported_format(&physical->caps.v1.sampler, backend)) {
            p->optimalTilingFeatures |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
            if (!integer && !depth_format(format))
                p->optimalTilingFeatures |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        }
        if (color_format(format)) {
            p->optimalTilingFeatures |= VK_FORMAT_FEATURE_BLIT_SRC_BIT;
            if (zvk_block(format) != 4 && format != VK_FORMAT_B4G4R4A4_UNORM_PACK16 &&
                supported_format(&physical->caps.v1.render, backend)) {
                p->optimalTilingFeatures |=
                    VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
                if (!integer)
                    p->optimalTilingFeatures |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT;
            }
        } else if (depth_format(format))
            p->optimalTilingFeatures |= VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                        VK_FORMAT_FEATURE_BLIT_SRC_BIT |
                                        VK_FORMAT_FEATURE_BLIT_DST_BIT;
        /* Linear transfer images use their bound host-coherent allocation
         * directly, with a published row/array/depth layout. */
        if (color_format(format) && zvk_block(format) == 1)
            p->linearTilingFeatures = VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
                                       VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    }
    uint32_t kind, components;
    if ((physical->caps.capability_bits_v2 & VIRGL_CAP_V2_ZVK_METAL_IMAGES) &&
        zvk_texel_format(format, &kind, &components)) {
        p->bufferFeatures |= VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT;
        if (zvk_storage_format(format))
            p->bufferFeatures |= VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT;
        if (format == VK_FORMAT_R32_UINT || format == VK_FORMAT_R32_SINT)
            p->bufferFeatures |= VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_ATOMIC_BIT;
        if (zvk_storage_format(format) && image_format(physical, format)) {
            p->optimalTilingFeatures |= VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT |
                                        VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
                                        VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
            if (format == VK_FORMAT_R32_UINT || format == VK_FORMAT_R32_SINT)
                p->optimalTilingFeatures |= VK_FORMAT_FEATURE_STORAGE_IMAGE_ATOMIC_BIT;
        }
    }
}
VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceImageFormatProperties(
    VkPhysicalDevice physical, VkFormat format, VkImageType type, VkImageTiling tiling,
    VkImageUsageFlags usage, VkImageCreateFlags flags, VkImageFormatProperties *p)
{
    (void)physical;
    memset(p, 0, sizeof *p);
    if (!image_format(physical, format))
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    VkFormatProperties features;
    vkGetPhysicalDeviceFormatProperties(physical, format, &features);
    VkFormatFeatureFlags bits = tiling == VK_IMAGE_TILING_LINEAR ? features.linearTilingFeatures
                                                                : features.optimalTilingFeatures;
    VkImageUsageFlags valid = 0;
    if (bits & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT)
        valid |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (bits & VK_FORMAT_FEATURE_TRANSFER_DST_BIT)
        valid |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (bits & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)
        valid |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (bits & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)
        valid |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT |
                 VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
    if (bits & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
        valid |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT |
                 VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
    if (bits & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)
        valid |= VK_IMAGE_USAGE_STORAGE_BIT;
    if ((type != VK_IMAGE_TYPE_1D && type != VK_IMAGE_TYPE_2D && type != VK_IMAGE_TYPE_3D) ||
        (type == VK_IMAGE_TYPE_3D && depth_format(format)) ||
        (tiling != VK_IMAGE_TILING_OPTIMAL && tiling != VK_IMAGE_TILING_LINEAR) ||
        (tiling == VK_IMAGE_TILING_LINEAR &&
         (type != VK_IMAGE_TYPE_2D || (flags & ~VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) || !bits)) ||
        (flags & ~(VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT | VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT)) ||
        ((flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) &&
         !(physical->caps.capability_bits_v2 & VIRGL_CAP_V2_ZVK_MUTABLE_VIEWS)) ||
        ((flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) && type != VK_IMAGE_TYPE_2D) ||
        ((usage & VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT) &&
         (usage & ~(VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                    VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT))) ||
        (usage & ~valid))
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    p->maxExtent = type == VK_IMAGE_TYPE_3D   ? (VkExtent3D){256, 256, 256}
                   : type == VK_IMAGE_TYPE_1D ? (VkExtent3D){4096, 1, 1}
                                              : (VkExtent3D){4096, 4096, 1};
    p->maxMipLevels = type == VK_IMAGE_TYPE_3D ? 9 : 13;
    p->maxArrayLayers = type == VK_IMAGE_TYPE_3D ? 1 : 256;
    p->sampleCounts = type != VK_IMAGE_TYPE_2D ||
                              (!color_format(format) && !depth_format(format)) ||
                              (flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) ||
                              (usage & VK_IMAGE_USAGE_STORAGE_BIT)
                          ? VK_SAMPLE_COUNT_1_BIT
                          : zvk_samples(physical, format);
    p->maxResourceSize = ZVK_MAX_ALLOC;
    if (tiling == VK_IMAGE_TILING_LINEAR) {
        p->maxMipLevels = p->maxArrayLayers = 1;
        p->sampleCounts = VK_SAMPLE_COUNT_1_BIT;
    }
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceSparseImageFormatProperties(
    VkPhysicalDevice p, VkFormat f, VkImageType t, VkSampleCountFlagBits s, VkImageUsageFlags u,
    VkImageTiling tiling, uint32_t *count, VkSparseImageFormatProperties *properties)
{
    (void)p;
    (void)f;
    (void)t;
    (void)s;
    (void)u;
    (void)tiling;
    (void)properties;
    *count = 0;
}

VKAPI_ATTR VkResult VKAPI_CALL vkAllocateMemory(VkDevice device, const VkMemoryAllocateInfo *info,
                                                const VkAllocationCallbacks *allocator,
                                                VkDeviceMemory *result)
{
    *result = VK_NULL_HANDLE;
    if (info->memoryTypeIndex || !info->allocationSize || info->allocationSize > ZVK_MAX_ALLOC)
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    zvk_allocator a;
    zvk_allocator_init(&a, allocator);
    VkDeviceMemory memory = zvk_alloc(&a, sizeof *memory);
    if (!memory)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    /* Reserve before allocating so simultaneous host calls cannot overcommit
     * the advertised heap or exhaust the host during allocation stress. This
     * mutex is separate from the GPU lock (WSI can allocate while holding it). */
    pthread_mutex_lock(&device->memory_mutex);
    if (info->allocationSize > ZVK_MAX_ALLOC - device->allocated_bytes ||
        device->allocation_count >= 4096) {
        pthread_mutex_unlock(&device->memory_mutex);
        zvk_free(&a, memory);
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    device->allocated_bytes += info->allocationSize;
    device->allocation_count++;
    pthread_mutex_unlock(&device->memory_mutex);
    memory->data = zvk_alloc_aligned(&a, (size_t)info->allocationSize, 64);
    if (!memory->data) {
        pthread_mutex_lock(&device->memory_mutex);
        device->allocated_bytes -= info->allocationSize;
        device->allocation_count--;
        pthread_mutex_unlock(&device->memory_mutex);
        zvk_free(&a, memory);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    memory->alloc = a;
    memory->device = device;
    memory->size = info->allocationSize;
    *result = memory;
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkFreeMemory(VkDevice device, VkDeviceMemory memory,
                                        const VkAllocationCallbacks *allocator)
{
    (void)device;
    (void)allocator;
    if (!memory)
        return;
    if (memory->compute_gpu) {
        pthread_mutex_lock(&device->mutex);
        virgl_res_destroy(device->gpu, memory->compute_gpu);
        pthread_mutex_unlock(&device->mutex);
    }
    zvk_free(&memory->alloc, memory->data);
    pthread_mutex_lock(&device->memory_mutex);
    device->allocated_bytes -= memory->size;
    device->allocation_count--;
    pthread_mutex_unlock(&device->memory_mutex);
    zvk_free(&memory->alloc, memory);
}
VKAPI_ATTR VkResult VKAPI_CALL vkMapMemory(VkDevice device, VkDeviceMemory memory,
                                           VkDeviceSize offset, VkDeviceSize size,
                                           VkMemoryMapFlags flags, void **data)
{
    (void)device;
    *data = NULL;
    if (flags || !memory || memory->mapped || offset >= memory->size ||
        (size != VK_WHOLE_SIZE && !zvk_range(memory->size, offset, size)))
        return VK_ERROR_MEMORY_MAP_FAILED;
    memory->mapped = 1;
    *data = memory->data + offset;
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkUnmapMemory(VkDevice device, VkDeviceMemory memory)
{
    (void)device;
    memory->mapped = 0;
}
static VkResult mapped_ranges(uint32_t count, const VkMappedMemoryRange *ranges)
{
    for (uint32_t i = 0; i < count; i++) {
        const VkMappedMemoryRange *r = ranges + i;
        if (!r->memory || !r->memory->mapped || r->offset >= r->memory->size ||
            (r->size != VK_WHOLE_SIZE && !zvk_range(r->memory->size, r->offset, r->size)))
            return VK_ERROR_MEMORY_MAP_FAILED;
    }
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkFlushMappedMemoryRanges(VkDevice device, uint32_t count,
                                                         const VkMappedMemoryRange *ranges)
{
    (void)device;
    return mapped_ranges(count, ranges);
}
VKAPI_ATTR VkResult VKAPI_CALL vkInvalidateMappedMemoryRanges(VkDevice device, uint32_t count,
                                                              const VkMappedMemoryRange *ranges)
{
    (void)device;
    return mapped_ranges(count, ranges);
}
VKAPI_ATTR void VKAPI_CALL vkGetDeviceMemoryCommitment(VkDevice device, VkDeviceMemory memory,
                                                       VkDeviceSize *size)
{
    (void)device;
    *size = memory->size;
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateBuffer(VkDevice device, const VkBufferCreateInfo *info,
                                              const VkAllocationCallbacks *allocator,
                                              VkBuffer *result)
{
    *result = VK_NULL_HANDLE;
    /* Buffer storage is untyped; extension commands remain gated by their
     * extension. Uniform-buffer bindings retain the advertised alignment. */
    if (info->flags)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    if (!info->size || info->size > ZVK_MAX_ALLOC)
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    zvk_allocator a;
    zvk_allocator_init(&a, allocator);
    VkBuffer b = zvk_alloc(&a, sizeof *b);
    if (!b)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    b->alloc = a;
    b->device = device;
    b->size = info->size;
    b->usage = info->usage;
    *result = b;
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyBuffer(VkDevice device, VkBuffer b,
                                           const VkAllocationCallbacks *allocator)
{
    (void)allocator;
    if (!b)
        return;
    pthread_mutex_lock(&device->mutex);
    if (b->gpu)
        virgl_res_destroy(device->gpu, b->gpu);
    pthread_mutex_unlock(&device->mutex);
    zvk_free(&b->alloc, b);
}
static VkDeviceSize buffer_alignment(VkBuffer b)
{
    VkDeviceSize alignment=16;
    if ((b->usage&VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) &&
        b->device->physical->caps.uniform_buffer_offset_alignment>alignment)
        alignment=b->device->physical->caps.uniform_buffer_offset_alignment;
    return alignment;
}
VKAPI_ATTR void VKAPI_CALL vkGetBufferMemoryRequirements(VkDevice device, VkBuffer b,
                                                         VkMemoryRequirements *r)
{
    (void)device;
    VkDeviceSize alignment=buffer_alignment(b);
    *r = (VkMemoryRequirements){.size=(b->size+alignment-1)&~(alignment-1),
                                .alignment=alignment,.memoryTypeBits=1};
}
VKAPI_ATTR VkResult VKAPI_CALL vkBindBufferMemory(VkDevice device, VkBuffer b,
                                                  VkDeviceMemory memory, VkDeviceSize offset)
{
    VkDeviceSize alignment=buffer_alignment(b);
    if (!memory || memory->device != device || b->memory || (offset & (alignment-1)) ||
        !zvk_range(memory->size, offset, (b->size+alignment-1)&~(alignment-1)))
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    b->memory = memory;
    b->offset = offset;
    return VK_SUCCESS;
}
VkResult zvk_buffer_upload(VkBuffer b)
{
    if (!b || !b->memory)
        return VK_ERROR_INITIALIZATION_FAILED;
    VkDevice d = b->device;
    if (!b->gpu) {
        /* Virgl buffer bind flags are a single class, not a bitmask.
         * An untyped buffer can be rebound for vertex and index accesses. */
        uint32_t bind = 0;
        b->gpu = virgl_res_create(d->gpu, PIPE_BUFFER, VIRGL_FORMAT_R8_UNORM, bind,
                                  (uint32_t)b->size, 1, 1, 0, (size_t)b->size);
        if (!b->gpu || !b->gpu->map) {
            if (b->gpu)
                virgl_res_destroy(d->gpu, b->gpu);
            b->gpu = NULL;
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
    }
    memcpy(b->gpu->map, b->memory->data + b->offset, (size_t)b->size);
    if (virgl_res_to_host(d->gpu, b->gpu, 0, 0, 0, 0, (uint32_t)b->size, 1, 1, 0, 0, 0))
        return VK_ERROR_DEVICE_LOST;
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateImage(VkDevice device, const VkImageCreateInfo *info,
                                             const VkAllocationCallbacks *allocator,
                                             VkImage *result)
{
    *result = VK_NULL_HANDLE;
    VkImageFormatProperties props;
    VkResult r =
        vkGetPhysicalDeviceImageFormatProperties(device->physical, info->format, info->imageType,
                                                 info->tiling, info->usage, info->flags, &props);
    if (getenv("ZVK_DEBUG_RESOURCES"))
        fprintf(stderr, "zvk image format=%u type=%u tiling=%u flags=%x usage=%x extent=%ux%ux%u mips=%u layers=%u samples=%x query=%d\n",
                info->format, info->imageType, info->tiling, info->flags, info->usage,
                info->extent.width, info->extent.height, info->extent.depth, info->mipLevels,
                info->arrayLayers, info->samples, r);
    if (r != VK_SUCCESS)
        return r;
    if (!info->extent.width || !info->extent.height || !info->extent.depth ||
        info->extent.width > props.maxExtent.width ||
        info->extent.height > props.maxExtent.height ||
        info->extent.depth > props.maxExtent.depth || !info->mipLevels ||
        info->mipLevels > props.maxMipLevels || !info->arrayLayers ||
        info->arrayLayers > props.maxArrayLayers || !(props.sampleCounts & info->samples) ||
        !info->samples || (info->samples & (info->samples - 1)) ||
        (info->samples != VK_SAMPLE_COUNT_1_BIT && info->mipLevels != 1) ||
        ((info->flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) &&
         (info->arrayLayers < 6 || info->extent.width != info->extent.height)))
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    uint32_t levels = 1, largest = info->extent.width > info->extent.height ? info->extent.width
                                                                            : info->extent.height;
    if (info->extent.depth > largest)
        largest = info->extent.depth;
    while (largest > 1) {
        levels++;
        largest >>= 1;
    }
    if (info->mipLevels > levels)
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    zvk_allocator a;
    zvk_allocator_init(&a, allocator);
    VkImage image = zvk_alloc(&a, sizeof *image);
    if (!image)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    image->alloc = a;
    image->device = device;
    image->info = *info;
    image->info.pNext = NULL;
    image->info.pQueueFamilyIndices = NULL;
    image->format = image_format(device->physical, info->format);
    image->bpp = zvk_bpp(info->format);
    image->target = info->imageType == VK_IMAGE_TYPE_3D ? PIPE_TEXTURE_3D
                    : info->imageType == VK_IMAGE_TYPE_1D
                        ? (info->arrayLayers > 1 ? PIPE_TEXTURE_1D_ARRAY : PIPE_TEXTURE_1D)
                    : ((info->flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) && info->arrayLayers == 6)
                        ? PIPE_TEXTURE_CUBE
                    : info->arrayLayers > 1                               ? PIPE_TEXTURE_2D_ARRAY
                                                                          : PIPE_TEXTURE_2D;
    for (uint32_t m = 0; m < info->mipLevels; m++) {
        image->level_offsets[m] = image->size;
        image->size += zvk_level_size(image, m) * info->arrayLayers;
    }
    image->size *= info->samples;
    if (image->size > ZVK_MAX_ALLOC) {
        zvk_free(&a, image);
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    image->layouts = zvk_alloc(&a, info->mipLevels * info->arrayLayers * (sizeof(VkImageLayout) + 1));
    if (!image->layouts) {
        zvk_free(&a, image);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    image->transfer_valid = (uint8_t *)(image->layouts + info->mipLevels * info->arrayLayers);
    for (uint32_t i = 0; i < info->mipLevels * info->arrayLayers; i++)
        image->layouts[i] = info->initialLayout;
    *result = image;
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyImage(VkDevice device, VkImage image,
                                          const VkAllocationCallbacks *allocator)
{
    (void)allocator;
    if (!image)
        return;
    pthread_mutex_lock(&device->mutex);
    if (image->gpu)
        virgl_res_destroy(device->gpu, image->gpu);
    pthread_mutex_unlock(&device->mutex);
    zvk_free(&image->alloc, image->transfer_map);
    zvk_free(&image->alloc, image->layouts);
    zvk_free(&image->alloc, image);
}
VKAPI_ATTR void VKAPI_CALL vkGetImageMemoryRequirements(VkDevice device, VkImage image,
                                                        VkMemoryRequirements *r)
{
    (void)device;
    *r = (VkMemoryRequirements){
        .size = (image->size + 15) & ~(VkDeviceSize)15, .alignment = 16, .memoryTypeBits = 1};
}
VKAPI_ATTR VkResult VKAPI_CALL vkBindImageMemory(VkDevice d, VkImage image, VkDeviceMemory memory,
                                                 VkDeviceSize offset)
{
    if (!memory || memory->device != d || image->memory || (offset & 15) ||
        !zvk_range(memory->size, offset, (image->size + 15) & ~(VkDeviceSize)15))
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    image->memory = memory;
    image->offset = offset;
    return VK_SUCCESS;
}
void *zvk_image_map(VkImage image)
{
    if (image->info.tiling == VK_IMAGE_TILING_LINEAR)
        return image->memory->data + image->offset;
    return image->transfer_map ? image->transfer_map : image->gpu->map;
}
VkResult zvk_image_gpu(VkImage image)
{
    if (!image || !image->memory)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (image->gpu)
        return VK_SUCCESS;
    int expanded =
        image->info.format == VK_FORMAT_B4G4R4A4_UNORM_PACK16 || zvk_block(image->info.format) == 4;
    if (expanded && !image->transfer_map) {
        image->transfer_map = zvk_alloc(&image->alloc, (size_t)image->size);
        if (!image->transfer_map)
            return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    uint32_t bind = VIRGL_BIND_SAMPLER_VIEW;
    if (depth_format(image->info.format))
        bind |= VIRGL_BIND_DEPTH_STENCIL;
    else if (supported_format(&image->device->physical->caps.v1.render, image->format))
        bind |= VIRGL_BIND_RENDER_TARGET;
    VkDeviceSize gpu_size = zvk_gpu_image_offset(image, image->info.mipLevels, 0);
    if (gpu_size > UINT32_MAX)
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    struct gpu_res_create rc = {
        .target = image->target,
        .format = image->format,
        .bind = bind,
        .width = image->info.extent.width,
        .height = image->info.extent.height,
        .depth = image->info.extent.depth,
        .array_size = image->info.arrayLayers,
        .last_level = image->info.mipLevels - 1,
        .nr_samples = image->info.samples == VK_SAMPLE_COUNT_1_BIT ? 0 : image->info.samples,
        .size = image->info.samples == VK_SAMPLE_COUNT_1_BIT ? (uint32_t)gpu_size : 0};
    image->gpu = virgl_res_create_ex(image->device->gpu, &rc);
    if (!image->gpu || (image->info.samples == VK_SAMPLE_COUNT_1_BIT && !image->gpu->map)) {
        if (image->gpu)
            virgl_res_destroy(image->device->gpu, image->gpu);
        image->gpu = NULL;
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkGetImageSubresourceLayout(VkDevice d, VkImage image,
                                                       const VkImageSubresource *subresource,
                                                       VkSubresourceLayout *layout)
{
    (void)d;
    memset(layout, 0, sizeof *layout);
    if (image->info.tiling != VK_IMAGE_TILING_LINEAR || subresource->mipLevel ||
        subresource->arrayLayer || subresource->aspectMask != VK_IMAGE_ASPECT_COLOR_BIT)
        return;
    layout->offset = 0;
    layout->rowPitch = (VkDeviceSize)image->info.extent.width * image->bpp;
    layout->depthPitch = layout->rowPitch * image->info.extent.height;
    layout->arrayPitch = layout->size = layout->depthPitch;
}
VKAPI_ATTR void VKAPI_CALL vkGetImageSparseMemoryRequirements(
    VkDevice d, VkImage image, uint32_t *count, VkSparseImageMemoryRequirements *requirements)
{
    (void)d;
    (void)image;
    (void)requirements;
    *count = 0;
}
static int view_formats_compatible(VkFormat image, VkFormat view)
{
    if (image==view)
        return 1;
    const zvk_format_info *a=zvk_get_format_info(image), *b=zvk_get_format_info(view);
    if (!a || !b || a->aspects!=VK_IMAGE_ASPECT_COLOR_BIT || b->aspects!=a->aspects)
        return 0;
    if (a->type==ZVK_FMT_BC || b->type==ZVK_FMT_BC)
        return a->type==b->type && (image-VK_FORMAT_BC1_RGB_UNORM_BLOCK)/2==
                                   (view-VK_FORMAT_BC1_RGB_UNORM_BLOCK)/2;
    return a->bpp==b->bpp;
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateImageView(VkDevice d, const VkImageViewCreateInfo *info,
                                                 const VkAllocationCallbacks *allocator,
                                                 VkImageView *result)
{
    *result = VK_NULL_HANDLE;
    VkImageSubresourceRange range;
    if ((info->format != info->image->info.format &&
         (!(info->image->info.flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) ||
          !view_formats_compatible(info->image->info.format,info->format))) ||
        !image_format(d->physical,info->format) ||
        !zvk_image_range(info->image, &info->subresourceRange, &range))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    uint32_t target;
    if (info->viewType == VK_IMAGE_VIEW_TYPE_1D &&
        info->image->info.imageType == VK_IMAGE_TYPE_1D && range.layerCount == 1)
        target = PIPE_TEXTURE_1D;
    else if (info->viewType == VK_IMAGE_VIEW_TYPE_1D_ARRAY &&
             info->image->info.imageType == VK_IMAGE_TYPE_1D)
        target = PIPE_TEXTURE_1D_ARRAY;
    else if (info->viewType == VK_IMAGE_VIEW_TYPE_3D &&
             info->image->info.imageType == VK_IMAGE_TYPE_3D)
        target = PIPE_TEXTURE_3D;
    else if (info->viewType == VK_IMAGE_VIEW_TYPE_2D &&
             info->image->info.imageType == VK_IMAGE_TYPE_2D && range.layerCount == 1)
        target = PIPE_TEXTURE_2D;
    else if (info->viewType == VK_IMAGE_VIEW_TYPE_2D_ARRAY &&
             info->image->info.imageType == VK_IMAGE_TYPE_2D)
        target = PIPE_TEXTURE_2D_ARRAY;
    else if (info->viewType == VK_IMAGE_VIEW_TYPE_CUBE &&
             (info->image->info.flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) &&
             range.layerCount == 6)
        target = PIPE_TEXTURE_CUBE;
    else
        return VK_ERROR_FEATURE_NOT_PRESENT;
    zvk_allocator a;
    zvk_allocator_init(&a, allocator);
    VkImageView view = zvk_alloc(&a, sizeof *view);
    if (!view)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    view->alloc = a;
    view->device = d;
    view->image = info->image;
    view->format = info->format;
    view->range = range;
    view->target = target;
    view->swizzle = info->components;
    *result = view;
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyImageView(VkDevice d, VkImageView view,
                                              const VkAllocationCallbacks *allocator)
{
    (void)allocator;
    if (!view)
        return;
    pthread_mutex_lock(&d->mutex);
    if (view->reinterpret) {
        VkImage image=view->reinterpret;
        if (image->gpu)
            virgl_res_destroy(d->gpu,image->gpu);
        zvk_free(&image->alloc,image->transfer_map);
        zvk_free(&image->alloc,image->layouts);
        zvk_free(&image->alloc,image);
    }
    if (view->surface)
        virgl_destroy_object(d->gpu, view->surface, VIRGL_OBJECT_SURFACE);
    if (view->sample_gpu)
        virgl_res_destroy(d->gpu, view->sample_gpu);
    if (view->sampler_view)
        virgl_destroy_object(d->gpu, view->sampler_view, VIRGL_OBJECT_SAMPLER_VIEW);
    if (view->input_view)
        virgl_destroy_object(d->gpu, view->input_view, VIRGL_OBJECT_SAMPLER_VIEW);
    if (view->input_gpu)
        virgl_res_destroy(d->gpu, view->input_gpu);
    if (view->compute_memory) {
        if (view->compute_memory->compute_gpu)
            virgl_res_destroy(d->gpu, view->compute_memory->compute_gpu);
        zvk_free(&view->alloc, view->compute_memory);
    }
    if (view->compute_sampler)
        virgl_destroy_object(d->gpu, view->compute_sampler, VIRGL_OBJECT_SAMPLER_STATE);
    if (view->compute_view)
        virgl_destroy_object(d->gpu, view->compute_view, VIRGL_OBJECT_SAMPLER_VIEW);
    if (view->compute_image)
        virgl_res_destroy(d->gpu, view->compute_image);
    if (view->stencil_gpu)
        virgl_res_destroy(d->gpu, view->stencil_gpu);
    pthread_mutex_unlock(&d->mutex);
    zvk_free(&view->alloc, view);
}
static uint32_t wrap(VkSamplerAddressMode mode)
{
    switch (mode) {
    case VK_SAMPLER_ADDRESS_MODE_REPEAT:
        return PIPE_TEX_WRAP_REPEAT;
    case VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT:
        return PIPE_TEX_WRAP_MIRROR_REPEAT;
    case VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER:
        return PIPE_TEX_WRAP_CLAMP_TO_BORDER;
    case VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE:
        return PIPE_TEX_WRAP_MIRROR_CLAMP_TO_EDGE;
    default:
        return PIPE_TEX_WRAP_CLAMP_TO_EDGE;
    }
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateSampler(VkDevice d, const VkSamplerCreateInfo *info,
                                               const VkAllocationCallbacks *allocator,
                                               VkSampler *result)
{
    *result = VK_NULL_HANDLE;
    if (info->anisotropyEnable ||
        fabsf(info->mipLodBias) > 16 || info->maxLod < info->minLod)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    zvk_allocator a;
    zvk_allocator_init(&a, allocator);
    VkSampler sampler = zvk_alloc(&a, sizeof *sampler);
    if (!sampler)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    sampler->alloc = a;
    sampler->device = d;
    sampler->create = *info;
    sampler->create.pNext=NULL;
    sampler->info = (struct virgl_sampler_info){
        .wrap_s = wrap(info->addressModeU),
        .wrap_t = wrap(info->addressModeV),
        .wrap_r = wrap(info->addressModeW),
        .min_img =
            info->minFilter == VK_FILTER_LINEAR ? PIPE_TEX_FILTER_LINEAR : PIPE_TEX_FILTER_NEAREST,
        .mag_img =
            info->magFilter == VK_FILTER_LINEAR ? PIPE_TEX_FILTER_LINEAR : PIPE_TEX_FILTER_NEAREST,
        .min_mip = info->mipmapMode == VK_SAMPLER_MIPMAP_MODE_LINEAR ? PIPE_TEX_MIPFILTER_LINEAR
                                                                     : PIPE_TEX_MIPFILTER_NEAREST,
        .compare_mode=info->compareEnable?PIPE_TEX_COMPARE_R_TO_TEXTURE:PIPE_TEX_COMPARE_NONE,
        .compare_func=info->compareOp,
        .seamless_cube_map=1,
        .lod_bias = info->mipLodBias,
        .min_lod = info->minLod,
        .max_lod = info->maxLod > 12 ? 12 : info->maxLod};
    if (info->borderColor == VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE ||
        info->borderColor == VK_BORDER_COLOR_INT_OPAQUE_WHITE) {
        for (int i = 0; i < 4; i++) {
            sampler->info.border[i]=(info->borderColor&1)?1u:0x3f800000u;
        }
    } else if (info->borderColor == VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK ||
               info->borderColor == VK_BORDER_COLOR_INT_OPAQUE_BLACK) {
        sampler->info.border[3]=(info->borderColor&1)?1u:0x3f800000u;
    }
    *result = sampler;
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkDestroySampler(VkDevice d, VkSampler sampler,
                                            const VkAllocationCallbacks *allocator)
{
    (void)allocator;
    if (!sampler)
        return;
    pthread_mutex_lock(&d->mutex);
    if (sampler->handle)
        virgl_destroy_object(d->gpu, sampler->handle, VIRGL_OBJECT_SAMPLER_STATE);
    pthread_mutex_unlock(&d->mutex);
    zvk_free(&sampler->alloc, sampler);
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateShaderModule(VkDevice d,
                                                    const VkShaderModuleCreateInfo *info,
                                                    const VkAllocationCallbacks *allocator,
                                                    VkShaderModule *result)
{
    *result = VK_NULL_HANDLE;
    if (info->codeSize % 4 || zvk_shader_check(info->pCode, info->codeSize / 4) != VK_SUCCESS)
        return VK_ERROR_INVALID_SHADER_NV;
    zvk_allocator a;
    zvk_allocator_init(&a, allocator);
    VkShaderModule module = zvk_alloc(&a, sizeof *module);
    if (!module)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    module->code = zvk_alloc(&a, info->codeSize);
    if (!module->code) {
        zvk_free(&a, module);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    module->alloc = a;
    module->device = d;
    module->words = info->codeSize / 4;
    memcpy(module->code, info->pCode, info->codeSize);
    *result = module;
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyShaderModule(VkDevice d, VkShaderModule module,
                                                 const VkAllocationCallbacks *allocator)
{
    (void)d;
    (void)allocator;
    if (module) {
        zvk_free(&module->alloc, module->code);
        zvk_free(&module->alloc, module);
    }
}
