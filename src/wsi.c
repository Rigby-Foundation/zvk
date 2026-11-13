/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (C) 2026 Rigby Foundation */
#include "zvk_internal.h"
#include <time.h>
#ifndef ZVK_HOST
#include <zwm.h>
#endif

struct extension {
    const char *name;
    uint32_t version, bit;
};
static const struct extension instance_extensions[] = {
    {VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
     VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_SPEC_VERSION, ZVK_EXT_PROPERTIES2},
    {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_SURFACE_SPEC_VERSION, ZVK_EXT_SURFACE},
    {VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME, VK_EXT_HEADLESS_SURFACE_SPEC_VERSION,
     ZVK_EXT_HEADLESS},
#ifndef ZVK_HOST
    {VK_SIC_ZWM_SURFACE_EXTENSION_NAME, VK_SIC_ZWM_SURFACE_SPEC_VERSION, ZVK_EXT_ZWM},
#endif
};
static const struct extension device_extensions[] = {
    {VK_EXT_HOST_QUERY_RESET_EXTENSION_NAME,
     VK_EXT_HOST_QUERY_RESET_SPEC_VERSION, ZVK_EXT_HOST_QUERY_RESET},
    {VK_EXT_PIPELINE_CREATION_CACHE_CONTROL_EXTENSION_NAME,
     VK_EXT_PIPELINE_CREATION_CACHE_CONTROL_SPEC_VERSION, ZVK_EXT_CACHE_CONTROL},
    {VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_SWAPCHAIN_SPEC_VERSION, ZVK_EXT_SWAPCHAIN}};
static VkResult enumerate_extensions(const struct extension *extensions, uint32_t n,
                                     uint32_t *count, VkExtensionProperties *properties)
{
    if (!properties) {
        *count = n;
        return VK_SUCCESS;
    }
    uint32_t take = *count < n ? *count : n;
    for (uint32_t i = 0; i < take; i++) {
        memset(properties + i, 0, sizeof *properties);
        snprintf(properties[i].extensionName, VK_MAX_EXTENSION_NAME_SIZE, "%s", extensions[i].name);
        properties[i].specVersion = extensions[i].version;
    }
    *count = take;
    return take < n ? VK_INCOMPLETE : VK_SUCCESS;
}
VkResult zvk_instance_extensions(uint32_t *count, VkExtensionProperties *properties)
{
    return enumerate_extensions(instance_extensions, ZVK_ARRAY_SIZE(instance_extensions), count,
                                properties);
}
VkResult zvk_device_extensions(uint32_t *count, VkExtensionProperties *properties)
{
    return enumerate_extensions(device_extensions, ZVK_ARRAY_SIZE(device_extensions), count,
                                properties);
}
VkResult zvk_enable_extensions(uint32_t count, const char *const *names, int device, uint32_t *bits)
{
    *bits = 0;
    const struct extension *extensions = device ? device_extensions : instance_extensions;
    uint32_t n = device ? ZVK_ARRAY_SIZE(device_extensions) : ZVK_ARRAY_SIZE(instance_extensions);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t j;
        for (j = 0; j < n; j++)
            if (!strcmp(names[i], extensions[j].name)) {
                *bits |= extensions[j].bit;
                break;
            }
        if (j == n) {
            zvk_error("extension %s is not supported", names[i]);
            return VK_ERROR_EXTENSION_NOT_PRESENT;
        }
    }
    if (!device && (*bits & (ZVK_EXT_HEADLESS | ZVK_EXT_ZWM)) && !(*bits & ZVK_EXT_SURFACE))
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    return VK_SUCCESS;
}
static VkResult surface_extent(VkSurfaceKHR surface, VkExtent2D *extent)
{
    if (!surface || surface->lost)
        return VK_ERROR_SURFACE_LOST_KHR;
#ifndef ZVK_HOST
    if (surface->connection) {
        struct zwm_m_geom geom;
        if (zwm_get_geometry(surface->connection, (int)surface->window, &geom) != 0) {
            surface->lost = 1;
            return VK_ERROR_SURFACE_LOST_KHR;
        }
        surface->extent =
            (VkExtent2D){geom.w > 0 ? (uint32_t)geom.w : 0, geom.h > 0 ? (uint32_t)geom.h : 0};
    }
#endif
    *extent = surface->extent;
    return VK_SUCCESS;
}
static void surface_unref(VkSurfaceKHR surface)
{
    if (surface && __atomic_sub_fetch(&surface->refs, 1, __ATOMIC_ACQ_REL) == 0)
        zvk_free(&surface->alloc, surface);
}
static VkResult create_surface(VkInstance instance, zwm *connection, uint32_t window,
                               const VkAllocationCallbacks *allocator, VkSurfaceKHR *result)
{
    *result = VK_NULL_HANDLE;
    zvk_allocator a;
    zvk_allocator_init(&a, allocator);
    VkSurfaceKHR surface = zvk_alloc(&a, sizeof *surface);
    if (!surface)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    surface->alloc = a;
    surface->instance = instance;
    surface->connection = connection;
    surface->window = window;
    surface->refs = 1;
    surface->extent = (VkExtent2D){UINT32_MAX, UINT32_MAX};
    VkExtent2D extent;
    VkResult r = surface_extent(surface, &extent);
    if (r != VK_SUCCESS) {
        surface_unref(surface);
        return r;
    }
    pthread_mutex_lock(&instance->surface_mutex);
    if (connection)
        for (VkSurfaceKHR s = instance->surfaces; s; s = s->next)
            if (s->connection == connection && s->window == window) {
                pthread_mutex_unlock(&instance->surface_mutex);
                surface_unref(surface);
                return VK_ERROR_NATIVE_WINDOW_IN_USE_KHR;
            }
    surface->next = instance->surfaces;
    instance->surfaces = surface;
    pthread_mutex_unlock(&instance->surface_mutex);
    *result = surface;
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL
vkCreateHeadlessSurfaceEXT(VkInstance instance, const VkHeadlessSurfaceCreateInfoEXT *info,
                           const VkAllocationCallbacks *allocator, VkSurfaceKHR *result)
{
    *result = VK_NULL_HANDLE;
    if (!(instance->extensions & ZVK_EXT_HEADLESS))
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    if (info->flags)
        return VK_ERROR_INITIALIZATION_FAILED;
    return create_surface(instance, NULL, 0, allocator, result);
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateZwmSurfaceSIC(VkInstance instance,
                                                     const VkZwmSurfaceCreateInfoSIC *info,
                                                     const VkAllocationCallbacks *allocator,
                                                     VkSurfaceKHR *result)
{
    *result = VK_NULL_HANDLE;
#ifdef ZVK_HOST
    (void)instance;
    (void)info;
    (void)allocator;
    return VK_ERROR_EXTENSION_NOT_PRESENT;
#else
    if (!(instance->extensions & ZVK_EXT_ZWM))
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    if (info->sType != VK_STRUCTURE_TYPE_ZWM_SURFACE_CREATE_INFO_SIC || info->flags ||
        !info->connection || !info->window)
        return VK_ERROR_INITIALIZATION_FAILED;
    if ((zwm_features(info->connection) & (ZWM_HELLO_GPU | ZWM_HELLO_PRESENT)) !=
        (ZWM_HELLO_GPU | ZWM_HELLO_PRESENT)) {
        zvk_error(
            "zwm GPU presentation requires a server with per-window presentation acknowledgements");
        return VK_ERROR_SURFACE_LOST_KHR;
    }
    return create_surface(instance, info->connection, info->window, allocator, result);
#endif
}
VKAPI_ATTR void VKAPI_CALL vkDestroySurfaceKHR(VkInstance instance, VkSurfaceKHR surface,
                                               const VkAllocationCallbacks *allocator)
{
    (void)allocator;
    if (!surface)
        return;
    pthread_mutex_lock(&instance->surface_mutex);
    VkSurfaceKHR *link = &instance->surfaces;
    while (*link && *link != surface)
        link = &(*link)->next;
    if (*link)
        *link = surface->next;
    pthread_mutex_unlock(&instance->surface_mutex);
    surface_unref(surface);
}
VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice physical,
                                                                    uint32_t family,
                                                                    VkSurfaceKHR surface,
                                                                    VkBool32 *supported)
{
    *supported = VK_FALSE;
    if (surface->instance != physical->instance)
        return VK_ERROR_SURFACE_LOST_KHR;
    VkExtent2D extent;
    VkResult r = surface_extent(surface, &extent);
    if (r == VK_SUCCESS && !family)
        *supported = VK_TRUE;
    return r;
}
VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
    VkPhysicalDevice physical, VkSurfaceKHR surface, VkSurfaceCapabilitiesKHR *caps)
{
    (void)physical;
    memset(caps, 0, sizeof *caps);
    VkExtent2D extent;
    VkResult r = surface_extent(surface, &extent);
    if (r != VK_SUCCESS)
        return r;
    caps->minImageCount = 2;
    caps->maxImageCount = 3;
    caps->currentExtent = extent;
    caps->minImageExtent = (VkExtent2D){1, 1};
    caps->maxImageExtent = (VkExtent2D){4096, 4096};
    caps->maxImageArrayLayers = 1;
    caps->supportedTransforms = caps->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    caps->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    caps->supportedUsageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                VK_IMAGE_USAGE_SAMPLED_BIT;
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice physical,
                                                                    VkSurfaceKHR surface,
                                                                    uint32_t *count,
                                                                    VkSurfaceFormatKHR *formats)
{
    VkExtent2D extent;
    VkResult r = surface_extent(surface, &extent);
    if (r != VK_SUCCESS)
        return r;
    const VkFormat candidates[] = {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM};
    VkSurfaceFormatKHR available[2];
    uint32_t n = 0;
    for (uint32_t i = 0; i < 2; i++) {
        VkFormatProperties p;
        vkGetPhysicalDeviceFormatProperties(physical, candidates[i], &p);
        if (p.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)
            available[n++] = (VkSurfaceFormatKHR){candidates[i], VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
    }
    if (!formats) {
        *count = n;
        return VK_SUCCESS;
    }
    uint32_t take = *count < n ? *count : n;
    memcpy(formats, available, take * sizeof *formats);
    *count = take;
    return take < n ? VK_INCOMPLETE : VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfacePresentModesKHR(VkPhysicalDevice physical,
                                                                         VkSurfaceKHR surface,
                                                                         uint32_t *count,
                                                                         VkPresentModeKHR *modes)
{
    (void)physical;
    VkExtent2D extent;
    VkResult r = surface_extent(surface, &extent);
    if (r != VK_SUCCESS)
        return r;
    if (!modes) {
        *count = 1;
        return VK_SUCCESS;
    }
    if (!*count)
        return VK_INCOMPLETE;
    modes[0] = VK_PRESENT_MODE_FIFO_KHR;
    *count = 1;
    return VK_SUCCESS;
}
/* The device mutex is held. The compositor acknowledges detach only after
 * its rendering fence, so a still-displayed image never becomes writable. */
static void detach_swapchain(VkSwapchainKHR chain)
{
    VkSurfaceKHR surface = chain->surface;
    if (surface->current != chain)
        return;
#ifndef ZVK_HOST
    if (surface->connection && !surface->lost &&
        zwm_detach_gpu(surface->connection, (int)surface->window, -1) != 0)
        surface->lost = 1;
#endif
    surface->current = NULL;
}
static void free_images(VkSwapchainKHR chain)
{
    VkDevice d = chain->device;
    for (uint32_t i = 0; i < chain->count; i++) {
        if (chain->display[i])
            virgl_res_destroy(d->gpu, chain->display[i]);
        VkImage image = chain->images[i];
        if (!image)
            continue;
        if (image->gpu)
            virgl_res_destroy(d->gpu, image->gpu);
        if (image->memory) {
            zvk_free(&image->memory->alloc, image->memory->data);
            zvk_free(&image->memory->alloc, image->memory);
        }
        zvk_free(&image->alloc, image->layouts);
        zvk_free(&image->alloc, image);
    }
}
VKAPI_ATTR void VKAPI_CALL vkDestroySwapchainKHR(VkDevice device, VkSwapchainKHR chain,
                                                 const VkAllocationCallbacks *allocator)
{
    (void)allocator;
    if (!chain)
        return;
    pthread_mutex_lock(&device->mutex);
    detach_swapchain(chain);
    if (chain->surface->latest == chain)
        chain->surface->latest = NULL;
    VkSwapchainKHR *link = &device->swapchains;
    while (*link && *link != chain)
        link = &(*link)->next;
    if (*link)
        *link = chain->next;
    free_images(chain);
    pthread_cond_broadcast(&device->condition);
    pthread_mutex_unlock(&device->mutex);
    surface_unref(chain->surface);
    zvk_free(&chain->alloc, chain);
}
void zvk_wsi_device_finish(VkDevice device)
{
    while (device->swapchains)
        vkDestroySwapchainKHR(device, device->swapchains, NULL);
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateSwapchainKHR(VkDevice device,
                                                    const VkSwapchainCreateInfoKHR *info,
                                                    const VkAllocationCallbacks *allocator,
                                                    VkSwapchainKHR *result)
{
    *result = VK_NULL_HANDLE;
    if (!(device->extensions & ZVK_EXT_SWAPCHAIN))
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    VkSurfaceKHR surface = info->surface;
    VkSurfaceCapabilitiesKHR caps;
    VkResult r = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device->physical, surface, &caps);
    if (r != VK_SUCCESS)
        return r;
    if (info->minImageCount < 2 || info->minImageCount > 3 || info->imageArrayLayers != 1 ||
        !info->imageExtent.width || !info->imageExtent.height || info->imageExtent.width > 4096 ||
        info->imageExtent.height > 4096 || info->flags ||
        info->presentMode != VK_PRESENT_MODE_FIFO_KHR ||
        info->imageSharingMode != VK_SHARING_MODE_EXCLUSIVE ||
        info->preTransform != VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR ||
        info->compositeAlpha != VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR ||
        (info->imageUsage & ~caps.supportedUsageFlags))
        return VK_ERROR_INITIALIZATION_FAILED;
    if (surface->connection && (caps.currentExtent.width != info->imageExtent.width ||
                                caps.currentExtent.height != info->imageExtent.height))
        return VK_ERROR_OUT_OF_DATE_KHR;
    uint32_t n = 2;
    VkSurfaceFormatKHR formats[2];
    r = vkGetPhysicalDeviceSurfaceFormatsKHR(device->physical, surface, &n, formats);
    if (r != VK_SUCCESS)
        return r;
    int match = 0;
    for (uint32_t i = 0; i < n; i++)
        if (formats[i].format == info->imageFormat &&
            formats[i].colorSpace == info->imageColorSpace)
            match = 1;
    if (!match)
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    if (surface->latest && !surface->latest->retired && surface->latest != info->oldSwapchain)
        return VK_ERROR_NATIVE_WINDOW_IN_USE_KHR;
    if (info->oldSwapchain && info->oldSwapchain->surface != surface)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (info->oldSwapchain)
        info->oldSwapchain->retired = 1;
    zvk_allocator a;
    zvk_allocator_init(&a, allocator);
    VkSwapchainKHR chain = zvk_alloc(&a, sizeof *chain);
    if (!chain)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    chain->alloc = a;
    chain->device = device;
    chain->surface = surface;
    chain->extent = info->imageExtent;
    chain->format = info->imageFormat;
    chain->count = info->minImageCount;
    for (uint32_t i = 0; i < chain->count; i++) {
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                .imageType = VK_IMAGE_TYPE_2D,
                                .format = chain->format,
                                .extent = {chain->extent.width, chain->extent.height, 1},
                                .mipLevels = 1,
                                .arrayLayers = 1,
                                .samples = VK_SAMPLE_COUNT_1_BIT,
                                .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = info->imageUsage,
                                .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                                .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
        r = vkCreateImage(device, &ci, allocator, chain->images + i);
        if (r != VK_SUCCESS)
            break;
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(device, chain->images[i], &req);
        VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                   .allocationSize = req.size};
        VkDeviceMemory memory;
        r = vkAllocateMemory(device, &ai, allocator, &memory);
        if (r != VK_SUCCESS)
            break;
        r = vkBindImageMemory(device, chain->images[i], memory, 0);
        if (r != VK_SUCCESS) {
            vkFreeMemory(device, memory, allocator);
            break;
        }
        chain->images[i]->swapchain = chain;
    }
    pthread_mutex_lock(&device->mutex);
    if (r != VK_SUCCESS) {
        free_images(chain);
        pthread_mutex_unlock(&device->mutex);
        zvk_free(&a, chain);
        return r;
    }
    __atomic_add_fetch(&surface->refs, 1, __ATOMIC_RELAXED);
    surface->latest = chain;
    chain->next = device->swapchains;
    device->swapchains = chain;
    pthread_mutex_unlock(&device->mutex);
    *result = chain;
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkGetSwapchainImagesKHR(VkDevice device, VkSwapchainKHR chain,
                                                       uint32_t *count, VkImage *images)
{
    (void)device;
    if (!images) {
        *count = chain->count;
        return VK_SUCCESS;
    }
    uint32_t n = *count < chain->count ? *count : chain->count;
    memcpy(images, chain->images, n * sizeof *images);
    *count = n;
    return n < chain->count ? VK_INCOMPLETE : VK_SUCCESS;
}
static VkResult chain_ready(VkSwapchainKHR chain, int acquire)
{
    VkExtent2D extent;
    VkResult r = surface_extent(chain->surface, &extent);
    if (r != VK_SUCCESS)
        return r;
    if ((acquire && chain->retired) ||
        (chain->surface->connection &&
         (extent.width != chain->extent.width || extent.height != chain->extent.height)))
        return VK_ERROR_OUT_OF_DATE_KHR;
    return chain->device->lost;
}
VKAPI_ATTR VkResult VKAPI_CALL vkAcquireNextImageKHR(VkDevice device, VkSwapchainKHR chain,
                                                     uint64_t timeout, VkSemaphore semaphore,
                                                     VkFence fence, uint32_t *index)
{
    uint64_t start = zvk_now_ns();
    pthread_mutex_lock(&device->mutex);
    VkResult r;
    for (;;) {
        r = chain_ready(chain, 1);
        if (r != VK_SUCCESS)
            break;
        for (uint32_t j = 0; j < chain->count; j++) {
            uint32_t i = (chain->cursor + j) % chain->count;
            if (chain->state[i] == 0) {
                chain->state[i] = 1;
                chain->cursor = (i + 1) % chain->count;
                *index = i;
                if (semaphore)
                    semaphore->signaled = 1;
                if (fence)
                    fence->signaled = 1;
                pthread_cond_broadcast(&device->condition);
                pthread_mutex_unlock(&device->mutex);
                return VK_SUCCESS;
            }
        }
        if (!timeout) {
            r = VK_NOT_READY;
            break;
        }
        uint64_t elapsed = zvk_now_ns() - start;
        if (elapsed >= timeout) {
            r = VK_TIMEOUT;
            break;
        }
        uint64_t wait =
            timeout == UINT64_MAX || timeout - elapsed > 10000000 ? 10000000 : timeout - elapsed;
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        uint64_t ns = ts.tv_nsec + wait;
        ts.tv_sec += (time_t)(ns / 1000000000);
        ts.tv_nsec = (long)(ns % 1000000000);
        pthread_cond_timedwait(&device->condition, &device->mutex, &ts);
    }
    pthread_mutex_unlock(&device->mutex);
    return r;
}
static VkResult present_image(VkSwapchainKHR chain, uint32_t index)
{
    VkDevice d = chain->device;
    VkSurfaceKHR surface = chain->surface;
    (void)surface;
    VkResult r = chain_ready(chain, 0);
    if (r != VK_SUCCESS)
        return r;
    if (index >= chain->count || chain->state[index] != 1 ||
        !zvk_layout_is(chain->images[index], 0, 0, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)) {
        zvk_error("presentation requires an acquired image in PRESENT_SRC_KHR layout");
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    r = zvk_image_gpu(chain->images[index]);
    if (r != VK_SUCCESS)
        return r;
#ifndef ZVK_HOST
    if (surface->connection) {
        if (!chain->display[index]) {
            chain->display[index] =
                virgl_res_create(d->gpu, PIPE_TEXTURE_2D, VIRGL_FORMAT_B8G8R8A8_UNORM,
                                 VIRGL_BIND_RENDER_TARGET | VIRGL_BIND_SAMPLER_VIEW,
                                 chain->extent.width, chain->extent.height, 1, 0, 0);
            if (!chain->display[index])
                return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        /* The zwm GPU protocol uses GL bottom-up rows; Vulkan's positive
         * viewport and transfer coordinates use a top-left origin. */
        struct virgl_blit_info blit = {
            .mask = PIPE_MASK_RGBA,
            .filter = PIPE_TEX_FILTER_NEAREST,
            .dst = {chain->display[index]->id, 0, VIRGL_FORMAT_B8G8R8A8_UNORM, 0, 0, 0,
                    (int)chain->extent.width, (int)chain->extent.height, 1},
            .src = {chain->images[index]->gpu->id, 0, chain->images[index]->format, 0,
                    (int)chain->extent.height, 0, (int)chain->extent.width,
                    -(int)chain->extent.height, 1}};
        virgl_blit(d->gpu, &blit);
        if (virgl_flush_fenced(d->gpu))
            return VK_ERROR_DEVICE_LOST;
        if (zwm_present_gpu(surface->connection, (int)surface->window, chain->display[index]->id,
                            (int)chain->extent.width, (int)chain->extent.height, -1) != 0) {
            surface->lost = 1;
            return VK_ERROR_SURFACE_LOST_KHR;
        }
        if (surface->current)
            surface->current->state[surface->current_image] = 0;
        surface->current = chain;
        surface->current_image = index;
        chain->state[index] = 2;
    } else
#endif
    {
        if (virgl_flush_fenced(d->gpu))
            return VK_ERROR_DEVICE_LOST;
        chain->state[index] = 0;
    }
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *info)
{
    VkDevice d = queue->device;
    pthread_mutex_lock(&d->mutex);
    VkResult result = d->lost;
    for (uint32_t i = 0; i < info->waitSemaphoreCount && result == VK_SUCCESS; i++)
        while (!info->pWaitSemaphores[i]->signaled && !d->lost)
            pthread_cond_wait(&d->condition, &d->mutex);
    while (d->submissions && !d->lost)
        pthread_cond_wait(&d->condition, &d->mutex);
    result = d->lost;
    if (result == VK_SUCCESS)
        for (uint32_t i = 0; i < info->waitSemaphoreCount; i++)
            info->pWaitSemaphores[i]->signaled = 0;
    VkResult wait_result = result;
    for (uint32_t i = 0; i < info->swapchainCount; i++) {
        VkResult r = wait_result != VK_SUCCESS ? wait_result
                     : d->lost ? d->lost
                               : present_image(info->pSwapchains[i], info->pImageIndices[i]);
        if (info->pResults)
            info->pResults[i] = r;
        if (r == VK_ERROR_DEVICE_LOST)
            d->lost = r;
        if (r != VK_SUCCESS && result != VK_ERROR_DEVICE_LOST)
            result = r;
    }
    if (result == VK_ERROR_DEVICE_LOST)
        d->lost = result;
    pthread_cond_broadcast(&d->condition);
    pthread_mutex_unlock(&d->mutex);
    return result;
}
