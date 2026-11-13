/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (C) 2026 Rigby Foundation */
#include "zvk_internal.h"
#include <stdarg.h>
#include <time.h>
#include <errno.h>
#include <math.h>

static _Thread_local char diagnostic[512];
const char *zvkGetLastError(void)
{
    return diagnostic;
}
void zvk_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(diagnostic, sizeof diagnostic, fmt, ap);
    va_end(ap);
}
void zvk_allocator_init(zvk_allocator *out, const VkAllocationCallbacks *in)
{
    memset(out, 0, sizeof *out);
    if (in) {
        out->cb = *in;
        out->custom = 1;
    }
}
void *zvk_alloc(const zvk_allocator *a, size_t size)
{
    return zvk_alloc_aligned(a, size, 16);
}
void *zvk_alloc_aligned(const zvk_allocator *a, size_t size, size_t alignment)
{
    if (!size)
        size = 1;
    void *p = NULL;
    if (a && a->custom)
        p = a->cb.pfnAllocation(a->cb.pUserData, size, alignment,
                                VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
    else if (posix_memalign(&p, alignment, size))
        p = NULL;
    if (p)
        memset(p, 0, size);
    return p;
}
void zvk_free(const zvk_allocator *a, void *p)
{
    if (!p)
        return;
    if (a && a->custom)
        a->cb.pfnFree(a->cb.pUserData, p);
    else
        free(p);
}
void *zvk_grow(const zvk_allocator *a, void *old, size_t oldsize, size_t size)
{
    void *p = zvk_alloc(a, size);
    if (p) {
        if (old)
            memcpy(p, old, oldsize < size ? oldsize : size);
        zvk_free(a, old);
    }
    return p;
}
int zvk_range(VkDeviceSize size, VkDeviceSize offset, VkDeviceSize count)
{
    return offset <= size && count <= size - offset;
}
uint64_t zvk_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000 + (uint64_t)ts.tv_nsec;
}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceVersion(uint32_t *version)
{
    *version = VK_API_VERSION_1_0;
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceExtensionProperties(
    const char *layer, uint32_t *count, VkExtensionProperties *properties)
{
    if (layer) {
        *count = 0;
        return VK_ERROR_LAYER_NOT_PRESENT;
    }
    return zvk_instance_extensions(count, properties);
}
VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceLayerProperties(uint32_t *count,
                                                                  VkLayerProperties *properties)
{
    (void)properties;
    *count = 0;
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(const VkInstanceCreateInfo *info,
                                                const VkAllocationCallbacks *allocator,
                                                VkInstance *result)
{
    *result = VK_NULL_HANDLE;
    if (!info || info->sType != VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO)
        return VK_ERROR_INITIALIZATION_FAILED;
    uint32_t extensions = 0;
    VkResult ext_result = zvk_enable_extensions(info->enabledExtensionCount,
                                                info->ppEnabledExtensionNames, 0, &extensions);
    if (ext_result != VK_SUCCESS)
        return ext_result;
    if (info->enabledLayerCount)
        return VK_ERROR_LAYER_NOT_PRESENT;
    uint32_t version = info->pApplicationInfo ? info->pApplicationInfo->apiVersion : 0;
    if (version && (VK_API_VERSION_VARIANT(version) || VK_API_VERSION_MAJOR(version) != 1 ||
                    VK_API_VERSION_MINOR(version)))
        return VK_ERROR_INCOMPATIBLE_DRIVER;
    zvk_allocator a;
    zvk_allocator_init(&a, allocator);
    VkInstance instance = zvk_alloc(&a, sizeof *instance);
    if (!instance)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    instance->alloc = a;
    instance->extensions = extensions;
    pthread_mutex_init(&instance->surface_mutex, NULL);
    set_loader_magic_value(instance);
    set_loader_magic_value(&instance->physical);
    instance->physical.instance = instance;
    virgl *probe = virgl_open();
    if (probe) {
        const struct virgl_caps_v2 *caps = virgl_caps(probe);
        if (caps && caps->v1.glsl_level >= 330) {
            instance->physical.caps = *caps;
            instance->available = 1;
        }
        virgl_close(probe);
    }
    *result = instance;
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyInstance(VkInstance instance,
                                             const VkAllocationCallbacks *allocator)
{
    (void)allocator;
    if (instance) {
        pthread_mutex_destroy(&instance->surface_mutex);
        zvk_free(&instance->alloc, instance);
    }
}
VKAPI_ATTR VkResult VKAPI_CALL vkEnumeratePhysicalDevices(VkInstance instance, uint32_t *count,
                                                          VkPhysicalDevice *devices)
{
    uint32_t available = instance->available ? 1 : 0;
    if (!devices) {
        *count = available;
        return VK_SUCCESS;
    }
    uint32_t n = *count < available ? *count : available;
    if (n)
        devices[0] = &instance->physical;
    *count = n;
    return n < available ? VK_INCOMPLETE : VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures(VkPhysicalDevice physical,
                                                       VkPhysicalDeviceFeatures *features)
{
    (void)physical;
    memset(features, 0, sizeof *features);
    features->robustBufferAccess = VK_TRUE;
    features->shaderStorageImageExtendedFormats =
        !!(physical->caps.capability_bits_v2 & VIRGL_CAP_V2_ZVK_METAL_IMAGES);
    features->textureCompressionBC = zvk_bc_supported(physical);
    features->fullDrawIndexUint32 = VK_TRUE;
    features->multiDrawIndirect = VK_TRUE;
    features->drawIndirectFirstInstance = VK_TRUE;
    features->depthBiasClamp = physical->caps.v1.bset.polygon_offset_clamp;
    features->independentBlend =
        physical->caps.v1.bset.indep_blend_enable && physical->caps.v1.bset.indep_blend_func;
    features->occlusionQueryPrecise = physical->caps.v1.bset.occlusion_query;
}
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties(VkPhysicalDevice physical,
                                                         VkPhysicalDeviceProperties *p)
{
    memset(p, 0, sizeof *p);
    p->apiVersion = VK_API_VERSION_1_0;
    p->driverVersion = VK_MAKE_VERSION(0, 1, 0);
    p->deviceType = VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU;
    snprintf(p->deviceName, sizeof p->deviceName, "zvk experimental (%.64s)",
             physical->caps.renderer);
    memcpy(p->pipelineCacheUUID, "zvk-virgl-v1", 12);
    VkPhysicalDeviceLimits *l = &p->limits;
    l->maxImageDimension1D = l->maxImageDimension2D = 4096;
    l->maxImageDimension3D = 256;
    l->maxImageDimensionCube = 4096;
    l->maxImageArrayLayers = 256;
    l->maxTexelBufferElements = 0;
    l->maxUniformBufferRange = 16384;
    if (physical->caps.capability_bits_v2 & VIRGL_CAP_V2_ZVK_METAL) {
        l->maxStorageBufferRange = 128u * 1024u * 1024u;
        l->maxPerStageDescriptorStorageBuffers = 8;
        l->maxDescriptorSetStorageBuffers = 32;
        l->maxDescriptorSetStorageBuffersDynamic = 4;
        l->maxComputeSharedMemorySize = 16384;
        l->maxComputeWorkGroupInvocations = 128;
        for (unsigned i = 0; i < 3; i++) {
            l->maxComputeWorkGroupCount[i] = 65535;
            l->maxComputeWorkGroupSize[i] = i == 2 ? 64 : 128;
        }
    }
    if (physical->caps.capability_bits_v2 & VIRGL_CAP_V2_ZVK_METAL_IMAGES) {
        l->maxTexelBufferElements = 65536;
        l->maxPerStageDescriptorStorageImages = 4;
        l->maxDescriptorSetStorageImages = 24;
        l->storageImageSampleCounts = VK_SAMPLE_COUNT_1_BIT;
    }
    l->maxPushConstantsSize = ZVK_PUSH;
    l->maxMemoryAllocationCount = 4096;
    l->maxSamplerAllocationCount = 4096;
    l->bufferImageGranularity = 16;
    l->maxBoundDescriptorSets = ZVK_SETS;
    l->maxPerStageDescriptorSamplers = l->maxPerStageDescriptorSampledImages = ZVK_SAMPLERS;
    l->maxPerStageDescriptorUniformBuffers = ZVK_UBOS;
    if (physical->caps.v1.max_uniform_blocks &&
        l->maxPerStageDescriptorUniformBuffers >= physical->caps.v1.max_uniform_blocks)
        l->maxPerStageDescriptorUniformBuffers = physical->caps.v1.max_uniform_blocks - 1;
    l->maxPerStageDescriptorInputAttachments = ZVK_ATTACHMENTS;
    l->maxPerStageResources = physical->caps.capability_bits_v2 & VIRGL_CAP_V2_ZVK_METAL_IMAGES
                                  ? ZVK_RESOURCES
                                  : ZVK_UBOS + ZVK_SAMPLERS;
    l->maxDescriptorSetSamplers =
        (physical->caps.capability_bits_v2 & VIRGL_CAP_V2_ZVK_METAL_IMAGES ? 3 : 2) * ZVK_SAMPLERS;
    l->maxDescriptorSetSampledImages =
        (physical->caps.capability_bits_v2 & VIRGL_CAP_V2_ZVK_METAL_IMAGES ? 3 : 2) * ZVK_SAMPLERS;
    l->maxDescriptorSetUniformBuffers = l->maxDescriptorSetUniformBuffersDynamic =
        (physical->caps.capability_bits_v2 & VIRGL_CAP_V2_ZVK_METAL ? 3 : 2) * ZVK_UBOS;
    l->maxDescriptorSetInputAttachments = ZVK_ATTACHMENTS;
    l->maxVertexInputAttributes = l->maxVertexInputBindings = ZVK_ATTRS;
    l->maxVertexInputAttributeOffset = 2047;
    l->maxVertexInputBindingStride = 2048;
    l->maxVertexOutputComponents = l->maxFragmentInputComponents = 64;
    l->maxFragmentOutputAttachments = l->maxFragmentCombinedOutputResources = ZVK_COLORS;
    l->maxDrawIndexedIndexValue = UINT32_MAX;
    l->maxDrawIndirectCount = ZVK_INDIRECT_DRAWS;
    l->maxSamplerLodBias = 16;
    l->maxSamplerAnisotropy = 1;
    l->maxViewports = 1;
    l->maxViewportDimensions[0] = l->maxViewportDimensions[1] = 4096;
    l->viewportBoundsRange[0] = -8192;
    l->viewportBoundsRange[1] = 8191;
    l->viewportSubPixelBits = 4;
    l->minMemoryMapAlignment = 64;
    l->minTexelBufferOffsetAlignment = 16;
    l->minUniformBufferOffsetAlignment = physical->caps.uniform_buffer_offset_alignment;
    if (l->minUniformBufferOffsetAlignment < 16)
        l->minUniformBufferOffsetAlignment = 16;
    l->minStorageBufferOffsetAlignment = 16;
    l->minTexelOffset = -8;
    l->maxTexelOffset = 7;
    l->maxFramebufferWidth = l->maxFramebufferHeight = 4096;
    l->maxFramebufferLayers = 256;
    l->framebufferColorSampleCounts = zvk_samples(physical, VK_FORMAT_R8G8B8A8_UNORM) &
                                      zvk_samples(physical, VK_FORMAT_B8G8R8A8_UNORM);
    l->framebufferDepthSampleCounts = zvk_samples(physical, VK_FORMAT_D32_SFLOAT) &
                                      zvk_samples(physical, VK_FORMAT_D24_UNORM_S8_UINT);
    l->framebufferStencilSampleCounts = zvk_samples(physical, VK_FORMAT_D24_UNORM_S8_UINT);
    l->framebufferNoAttachmentsSampleCounts = l->framebufferColorSampleCounts;
    l->sampledImageColorSampleCounts = l->framebufferColorSampleCounts;
    l->sampledImageIntegerSampleCounts = VK_SAMPLE_COUNT_1_BIT;
    l->sampledImageDepthSampleCounts =
        l->framebufferDepthSampleCounts & zvk_samples(physical, VK_FORMAT_D16_UNORM);
    l->sampledImageStencilSampleCounts =
        l->framebufferStencilSampleCounts & l->framebufferColorSampleCounts;
    l->maxColorAttachments = ZVK_COLORS;
    l->maxSampleMaskWords = 1;
    l->discreteQueuePriorities = 2;
    l->pointSizeRange[0] = l->pointSizeRange[1] = 1;
    l->lineWidthRange[0] = l->lineWidthRange[1] = 1;
    l->optimalBufferCopyOffsetAlignment = l->optimalBufferCopyRowPitchAlignment = 4;
    l->nonCoherentAtomSize = 1;
    /* Apple's M4 GL compatibility layer reports four subpixel bits, but
     * native coverage transitions at 1/512-pixel half-steps on both axes:
     * its rasterizer actually snaps to an eight-bit fractional grid. */
    l->subPixelPrecisionBits = !strcmp(physical->caps.renderer,"Apple M4") ? 8 : 4;
    /* GPU sampler interpolation uses float32 arithmetic. Reporting four bits
     * incorrectly describes a coarse weight grid, especially at NPOT repeat
     * seams; both fractional texture and mip weights support eight bits. */
    l->subTexelPrecisionBits = l->mipmapPrecisionBits = 8;
}
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceQueueFamilyProperties(
    VkPhysicalDevice physical, uint32_t *count, VkQueueFamilyProperties *properties)
{
    if (!properties) {
        *count = 1;
        return;
    }
    if (*count) {
        properties[0] = (VkQueueFamilyProperties){
            .queueFlags =
                VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_TRANSFER_BIT |
                (physical->caps.capability_bits_v2 & VIRGL_CAP_V2_ZVK_METAL ? VK_QUEUE_COMPUTE_BIT
                                                                            : 0),
            .queueCount = 1,
            .minImageTransferGranularity = {1, 1, 1}};
        *count = 1;
    }
}
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceMemoryProperties(
    VkPhysicalDevice physical, VkPhysicalDeviceMemoryProperties *properties)
{
    (void)physical;
    memset(properties, 0, sizeof *properties);
    properties->memoryTypeCount = properties->memoryHeapCount = 1;
    properties->memoryTypes[0].propertyFlags =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
        VK_MEMORY_PROPERTY_HOST_CACHED_BIT | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    properties->memoryHeaps[0].size = ZVK_MAX_ALLOC;
    properties->memoryHeaps[0].flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;
}
VKAPI_ATTR VkResult VKAPI_CALL
vkEnumerateDeviceExtensionProperties(VkPhysicalDevice physical, const char *layer, uint32_t *count,
                                     VkExtensionProperties *properties)
{
    (void)physical;
    if (layer) {
        *count = 0;
        return VK_ERROR_LAYER_NOT_PRESENT;
    }
    return zvk_device_extensions(count, properties);
}
VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceLayerProperties(VkPhysicalDevice physical,
                                                                uint32_t *count,
                                                                VkLayerProperties *properties)
{
    (void)physical;
    (void)properties;
    *count = 0;
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateDevice(VkPhysicalDevice physical,
                                              const VkDeviceCreateInfo *info,
                                              const VkAllocationCallbacks *allocator,
                                              VkDevice *result)
{
    *result = VK_NULL_HANDLE;
    uint32_t extensions = 0;
    VkResult ext_result = zvk_enable_extensions(info->enabledExtensionCount,
                                                info->ppEnabledExtensionNames, 1, &extensions);
    if (ext_result != VK_SUCCESS)
        return ext_result;
    if ((extensions & ZVK_EXT_SWAPCHAIN) && !(physical->instance->extensions & ZVK_EXT_SURFACE))
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    if (info->enabledLayerCount)
        return VK_ERROR_LAYER_NOT_PRESENT;
    if (info->queueCreateInfoCount != 1 || info->pQueueCreateInfos[0].queueFamilyIndex ||
        info->pQueueCreateInfos[0].queueCount != 1 || info->pQueueCreateInfos[0].flags)
        return VK_ERROR_INITIALIZATION_FAILED;
    VkPhysicalDeviceFeatures supported;
    vkGetPhysicalDeviceFeatures(physical, &supported);
    const VkPhysicalDeviceFeatures *features = info->pEnabledFeatures;
    int cache_control=0, host_query_reset=0;
    for (const VkBaseInStructure *node = info->pNext; node; node = node->pNext)
        if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) {
            if (features)
                return VK_ERROR_INITIALIZATION_FAILED;
            features = &((const VkPhysicalDeviceFeatures2 *)node)->features;
        } else if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_CREATION_CACHE_CONTROL_FEATURES &&
                   ((const VkPhysicalDevicePipelineCreationCacheControlFeatures *)node)->pipelineCreationCacheControl) {
            if (!(extensions&ZVK_EXT_CACHE_CONTROL))return VK_ERROR_FEATURE_NOT_PRESENT;
            cache_control=1;
        } else if (node->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES &&
                   ((const VkPhysicalDeviceHostQueryResetFeatures *)node)->hostQueryReset) {
            if (!(extensions&ZVK_EXT_HOST_QUERY_RESET))return VK_ERROR_FEATURE_NOT_PRESENT;
            host_query_reset=1;
        } else if (node->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_FEATURES &&
                   ((const VkPhysicalDeviceProtectedMemoryFeatures *)node)->protectedMemory) {
            return VK_ERROR_FEATURE_NOT_PRESENT;
        } else if (node->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES &&
                   ((const VkPhysicalDeviceShaderDrawParametersFeatures *)node)->shaderDrawParameters) {
            return VK_ERROR_FEATURE_NOT_PRESENT;
        }
    if (features) {
        const VkBool32 *want = (const VkBool32 *)features,
                       *have = (const VkBool32 *)&supported;
        for (size_t i = 0; i < sizeof supported / sizeof(VkBool32); i++)
            if (want[i] && !have[i])
                return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    const float *priorities = info->pQueueCreateInfos[0].pQueuePriorities;
    if (!priorities || !(priorities[0] >= 0.0f && priorities[0] <= 1.0f))
        return VK_ERROR_INITIALIZATION_FAILED;
    zvk_allocator a;
    zvk_allocator_init(&a, allocator);
    VkDevice device = zvk_alloc(&a, sizeof *device);
    if (!device)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    device->alloc = a;
    device->extensions = extensions;
    device->cache_control=cache_control;
    device->host_query_reset=host_query_reset;
    if (features)
        device->enabled_features = *features;
    device->physical = physical;
    set_loader_magic_value(device);
    set_loader_magic_value(&device->queue);
    device->queue.device = device;
    device->queue.priority = priorities[0] >= .5f;
    pthread_mutex_init(&device->mutex, NULL);
    pthread_mutex_init(&device->memory_mutex, NULL);
    pthread_cond_init(&device->condition, NULL);
    device->gpu = virgl_open();
    if (!device->gpu) {
        pthread_cond_destroy(&device->condition);
        pthread_mutex_destroy(&device->mutex);
        pthread_mutex_destroy(&device->memory_mutex);
        zvk_free(&a, device);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    VkResult queue_result = zvk_queue_start(device);
    if (queue_result != VK_SUCCESS) {
        virgl_close(device->gpu);
        pthread_cond_destroy(&device->condition);
        pthread_mutex_destroy(&device->mutex);
        pthread_mutex_destroy(&device->memory_mutex);
        zvk_free(&a, device);
        return queue_result;
    }
    *result = device;
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyDevice(VkDevice device, const VkAllocationCallbacks *allocator)
{
    (void)allocator;
    if (!device)
        return;
    zvk_queue_finish(device);
    zvk_wsi_device_finish(device);
    virgl_close(device->gpu);
    pthread_cond_destroy(&device->condition);
    pthread_mutex_destroy(&device->mutex);
    pthread_mutex_destroy(&device->memory_mutex);
    zvk_free(&device->alloc, device);
}
VKAPI_ATTR void VKAPI_CALL vkGetDeviceQueue(VkDevice device, uint32_t family, uint32_t index,
                                            VkQueue *queue)
{
    *queue = family || index ? VK_NULL_HANDLE : &device->queue;
}
VKAPI_ATTR VkResult VKAPI_CALL vkQueueWaitIdle(VkQueue queue)
{
    VkDevice d = queue->device;
    pthread_mutex_lock(&d->mutex);
    while (d->submissions && !d->lost)
        pthread_cond_wait(&d->condition, &d->mutex);
    VkResult result = d->lost;
    if (result == VK_SUCCESS && virgl_flush_fenced(d->gpu))
        result = d->lost = VK_ERROR_DEVICE_LOST;
    pthread_mutex_unlock(&d->mutex);
    return result;
}
VKAPI_ATTR VkResult VKAPI_CALL vkDeviceWaitIdle(VkDevice device)
{
    return vkQueueWaitIdle(&device->queue);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateFence(VkDevice d, const VkFenceCreateInfo *info,
                                             const VkAllocationCallbacks *allocator,
                                             VkFence *result)
{
    zvk_allocator a;
    zvk_allocator_init(&a, allocator);
    *result = zvk_alloc(&a, sizeof **result);
    if (!*result)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    (*result)->alloc = a;
    (*result)->device = d;
    (*result)->signaled = !!(info->flags & VK_FENCE_CREATE_SIGNALED_BIT);
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyFence(VkDevice d, VkFence fence,
                                          const VkAllocationCallbacks *allocator)
{
    (void)d;
    (void)allocator;
    if (fence)
        zvk_free(&fence->alloc, fence);
}
VKAPI_ATTR VkResult VKAPI_CALL vkGetFenceStatus(VkDevice d, VkFence fence)
{
    pthread_mutex_lock(&d->mutex);
    VkResult r = d->lost ? d->lost : fence->signaled ? VK_SUCCESS : VK_NOT_READY;
    pthread_mutex_unlock(&d->mutex);
    return r;
}
VKAPI_ATTR VkResult VKAPI_CALL vkResetFences(VkDevice d, uint32_t count, const VkFence *fences)
{
    pthread_mutex_lock(&d->mutex);
    for (uint32_t i = 0; i < count; i++)
        fences[i]->signaled = 0;
    pthread_mutex_unlock(&d->mutex);
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkWaitForFences(VkDevice d, uint32_t count, const VkFence *fences,
                                               VkBool32 all, uint64_t timeout)
{
    if (!count)
        return VK_SUCCESS;
    uint64_t start = zvk_now_ns();
    pthread_mutex_lock(&d->mutex);
    for (;;) {
        uint32_t signaled = 0;
        for (uint32_t i = 0; i < count; i++)
            signaled += !!fences[i]->signaled;
        if (d->lost || (all ? signaled == count : signaled != 0)) {
            VkResult r = d->lost;
            pthread_mutex_unlock(&d->mutex);
            return r;
        }
        uint64_t elapsed = zvk_now_ns() - start;
        if (elapsed >= timeout) {
            pthread_mutex_unlock(&d->mutex);
            return VK_TIMEOUT;
        }
        struct timespec until;
        clock_gettime(CLOCK_REALTIME, &until);
        uint64_t slice =
            timeout == UINT64_MAX || timeout - elapsed > 100000000 ? 100000000 : timeout - elapsed;
        uint64_t ns = (uint64_t)until.tv_nsec + slice;
        until.tv_sec += (time_t)(ns / 1000000000);
        until.tv_nsec = (long)(ns % 1000000000);
        pthread_cond_timedwait(&d->condition, &d->mutex, &until);
    }
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateSemaphore(VkDevice d, const VkSemaphoreCreateInfo *info,
                                                 const VkAllocationCallbacks *allocator,
                                                 VkSemaphore *result)
{
    (void)info;
    zvk_allocator a;
    zvk_allocator_init(&a, allocator);
    *result = zvk_alloc(&a, sizeof **result);
    if (!*result)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    (*result)->alloc = a;
    (*result)->device = d;
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkDestroySemaphore(VkDevice d, VkSemaphore s,
                                              const VkAllocationCallbacks *allocator)
{
    (void)d;
    (void)allocator;
    if (s)
        zvk_free(&s->alloc, s);
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateEvent(VkDevice d, const VkEventCreateInfo *info,
                                             const VkAllocationCallbacks *allocator,
                                             VkEvent *result)
{
    (void)info;
    zvk_allocator a;
    zvk_allocator_init(&a, allocator);
    *result = zvk_alloc(&a, sizeof **result);
    if (!*result)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    (*result)->alloc = a;
    (*result)->device = d;
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyEvent(VkDevice d, VkEvent e,
                                          const VkAllocationCallbacks *allocator)
{
    (void)d;
    (void)allocator;
    if (e)
        zvk_free(&e->alloc, e);
}
VKAPI_ATTR VkResult VKAPI_CALL vkGetEventStatus(VkDevice d, VkEvent e)
{
    pthread_mutex_lock(&d->mutex);
    VkResult r = e->signaled ? VK_EVENT_SET : VK_EVENT_RESET;
    pthread_mutex_unlock(&d->mutex);
    return r;
}
VKAPI_ATTR VkResult VKAPI_CALL vkSetEvent(VkDevice d, VkEvent e)
{
    pthread_mutex_lock(&d->mutex);
    e->signaled = 1;
    pthread_cond_broadcast(&d->condition);
    pthread_mutex_unlock(&d->mutex);
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkResetEvent(VkDevice d, VkEvent e)
{
    pthread_mutex_lock(&d->mutex);
    e->signaled = 0;
    pthread_mutex_unlock(&d->mutex);
    return VK_SUCCESS;
}
