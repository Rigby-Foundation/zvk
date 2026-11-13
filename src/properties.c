/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Extensible queries for the Vulkan 1.0 physical-device state. Unrecognized
 * pNext structures are ignored as required by the extension. */
#include "zvk_internal.h"

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures2KHR(VkPhysicalDevice physical,
                                                          VkPhysicalDeviceFeatures2 *out)
{
    vkGetPhysicalDeviceFeatures(physical, &out->features);
    for (VkBaseOutStructure *node = out->pNext; node; node = node->pNext) {
        if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_CREATION_CACHE_CONTROL_FEATURES)
            ((VkPhysicalDevicePipelineCreationCacheControlFeatures *)node)->pipelineCreationCacheControl = VK_TRUE;
        else if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES)
            ((VkPhysicalDeviceHostQueryResetFeatures *)node)->hostQueryReset = VK_TRUE;
        else if (node->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_FEATURES)
            ((VkPhysicalDeviceProtectedMemoryFeatures *)node)->protectedMemory=VK_FALSE;
        else if (node->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES)
            ((VkPhysicalDeviceShaderDrawParametersFeatures *)node)->shaderDrawParameters=VK_FALSE;
    }
}
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties2KHR(VkPhysicalDevice physical,
                                                            VkPhysicalDeviceProperties2 *out)
{
    vkGetPhysicalDeviceProperties(physical, &out->properties);
}
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFormatProperties2KHR(VkPhysicalDevice physical,
                                                                  VkFormat format,
                                                                  VkFormatProperties2 *out)
{
    vkGetPhysicalDeviceFormatProperties(physical, format, &out->formatProperties);
}
VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceImageFormatProperties2KHR(
    VkPhysicalDevice physical, const VkPhysicalDeviceImageFormatInfo2 *info,
    VkImageFormatProperties2 *out)
{
    return vkGetPhysicalDeviceImageFormatProperties(physical, info->format, info->type,
                                                     info->tiling, info->usage, info->flags,
                                                     &out->imageFormatProperties);
}
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceQueueFamilyProperties2KHR(
    VkPhysicalDevice physical, uint32_t *count, VkQueueFamilyProperties2 *out)
{
    if (!out) {
        vkGetPhysicalDeviceQueueFamilyProperties(physical, count, NULL);
        return;
    }
    uint32_t available = 1;
    VkQueueFamilyProperties property;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &available, &property);
    if (*count && available)
        out[0].queueFamilyProperties = property;
    if (*count > available)
        *count = available;
}
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceMemoryProperties2KHR(
    VkPhysicalDevice physical, VkPhysicalDeviceMemoryProperties2 *out)
{
    vkGetPhysicalDeviceMemoryProperties(physical, &out->memoryProperties);
}
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceSparseImageFormatProperties2KHR(
    VkPhysicalDevice physical, const VkPhysicalDeviceSparseImageFormatInfo2 *info,
    uint32_t *count, VkSparseImageFormatProperties2 *out)
{
    (void)physical;
    (void)info;
    (void)out;
    *count = 0;
}
