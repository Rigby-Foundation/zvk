/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (C) 2026 Rigby Foundation */
/* Initial Vulkan 1.0 limits audit. This is deliberately separate from the
 * supported-profile tests: passing those tests does not establish conformance.
 * Required limits: https://docs.vulkan.org/spec/latest/chapters/limits.html
 * Use the Vulkan Core column, without Vulkan 1.1+ or Roadmap requirements. */
#include <zvk.h>
#include <inttypes.h>
#include <stdio.h>

static unsigned passed, failed;
static void check(const char *name, double actual, double required, int maximum)
{
    int ok = maximum ? actual <= required : actual >= required;
    printf("%s    {\"name\":\"%s\",\"actual\":%.17g,\"required\":%.17g,"
           "\"comparison\":\"%s\",\"pass\":%s}",
           passed + failed ? ",\n" : "", name, actual, required, maximum ? "at_most" : "at_least",
           ok ? "true" : "false");
    if (ok)
        passed++;
    else
        failed++;
}
int main(void)
{
    VkInstance instance;
    VkInstanceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    if (vkCreateInstance(&info, NULL, &instance) != VK_SUCCESS)
        return 2;
    uint32_t count = 1;
    VkPhysicalDevice physical;
    if (vkEnumeratePhysicalDevices(instance, &count, &physical) != VK_SUCCESS || !count) {
        fprintf(stderr, "No physical device for profile audit\n");
        vkDestroyInstance(instance, NULL);
        return 2;
    }
    VkPhysicalDeviceProperties properties;
    VkPhysicalDeviceFeatures features;
    vkGetPhysicalDeviceProperties(physical, &properties);
    vkGetPhysicalDeviceFeatures(physical, &features);
    const VkPhysicalDeviceLimits *l = &properties.limits;
    printf("{\n  \"target\":\"Vulkan 1.0\",\n  \"scope\":\"initial limits and queue audit; not "
           "CTS\",\n  \"checks\":[\n");
#define MIN(field, value) check(#field, l->field, value, 0)
#define MAX(field, value) check(#field, l->field, value, 1)
    MIN(maxImageDimension1D, 4096);
    MIN(maxImageDimension2D, 4096);
    MIN(maxImageDimension3D, 256);
    MIN(maxImageDimensionCube, 4096);
    MIN(maxImageArrayLayers, 256);
    MIN(maxTexelBufferElements, 65536);
    MIN(maxUniformBufferRange, 16384);
    MIN(maxStorageBufferRange, 134217728);
    MIN(maxPushConstantsSize, 128);
    MIN(maxMemoryAllocationCount, 4096);
    MIN(maxSamplerAllocationCount, 4000);
    MIN(maxBoundDescriptorSets, 4);
    MIN(maxPerStageDescriptorSamplers, 16);
    MIN(maxPerStageDescriptorUniformBuffers, 12);
    MIN(maxPerStageDescriptorStorageBuffers, 4);
    MIN(maxPerStageDescriptorSampledImages, 16);
    MIN(maxPerStageDescriptorStorageImages, 4);
    MIN(maxPerStageDescriptorInputAttachments, 4);
    unsigned stages = 3 + 2 * !!features.tessellationShader + !!features.geometryShader;
    unsigned resources =
        l->maxPerStageDescriptorUniformBuffers + l->maxPerStageDescriptorStorageBuffers +
        l->maxPerStageDescriptorSampledImages + l->maxPerStageDescriptorStorageImages +
        l->maxPerStageDescriptorInputAttachments + l->maxColorAttachments;
    MIN(maxPerStageResources, resources < 128 ? resources : 128);
    MIN(maxDescriptorSetSamplers, stages * 16);
    MIN(maxDescriptorSetUniformBuffers, stages * 12);
    MIN(maxDescriptorSetUniformBuffersDynamic, 8);
    MIN(maxDescriptorSetStorageBuffers, stages * 4);
    MIN(maxDescriptorSetStorageBuffersDynamic, 4);
    MIN(maxDescriptorSetSampledImages, stages * 16);
    MIN(maxDescriptorSetStorageImages, stages * 4);
    MIN(maxDescriptorSetInputAttachments, 4);
    MIN(maxVertexInputAttributes, 16);
    MIN(maxVertexInputBindings, 16);
    MIN(maxVertexInputAttributeOffset, 2047);
    MIN(maxVertexInputBindingStride, 2048);
    MIN(maxVertexOutputComponents, 64);
    MIN(maxFragmentInputComponents, 64);
    MIN(maxFragmentOutputAttachments, 4);
    MIN(maxFragmentCombinedOutputResources, 4);
    MIN(maxComputeSharedMemorySize, 16384);
    MIN(maxComputeWorkGroupInvocations, 128);
    for (unsigned i = 0; i < 3; i++) {
        char name[64];
        snprintf(name, sizeof name, "maxComputeWorkGroupCount[%u]", i);
        check(name, l->maxComputeWorkGroupCount[i], 65535, 0);
        snprintf(name, sizeof name, "maxComputeWorkGroupSize[%u]", i);
        check(name, l->maxComputeWorkGroupSize[i], i == 2 ? 64 : 128, 0);
    }
    MIN(maxDrawIndexedIndexValue, 16777215);
    MIN(maxDrawIndirectCount, 1);
    MIN(maxViewports, 1);
    MIN(maxViewportDimensions[0], 4096);
    MIN(maxViewportDimensions[1], 4096);
    MAX(viewportBoundsRange[0], -8192);
    MIN(viewportBoundsRange[1], 8191);
    MIN(minMemoryMapAlignment, 64);
    MAX(minUniformBufferOffsetAlignment, 256);
    MIN(maxFramebufferWidth, 4096);
    MIN(maxFramebufferHeight, 4096);
    MIN(maxFramebufferLayers, 256);
    MIN(maxColorAttachments, 4);
    MIN(maxSampleMaskWords, 1);
    MIN(discreteQueuePriorities, 2);
    check("sampledImageStencilSampleCounts_1_and_4",
          (l->sampledImageStencilSampleCounts & (VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT)) ==
              (VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT),
          1, 0);
    VkQueueFamilyProperties family;
    count = 1;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, &family);
    check("graphics_and_compute_queue",
          count && (family.queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) ==
                       (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT),
          1, 0);
    printf("\n  ],\n  \"passed\":%u,\n  \"failed\":%u\n}\n", passed, failed);
    vkDestroyInstance(instance, NULL);
    return failed ? 1 : 0;
}
