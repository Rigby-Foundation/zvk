/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (C) 2026 Rigby Foundation */
/* This executable links only the Khronos loader, never libzvk or libvirgl. */
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
static unsigned errors;
#define VK(call)                                                                                   \
    do {                                                                                           \
        VkResult result_ = (call);                                                                 \
        if (result_ != VK_SUCCESS) {                                                               \
            fprintf(stderr, "%s = %d\n", #call, result_);                                          \
            exit(2);                                                                               \
        }                                                                                          \
    } while (0)
static VKAPI_ATTR
    VkBool32 VKAPI_CALL debug_message(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                      VkDebugUtilsMessageTypeFlagsEXT type,
                                      const VkDebugUtilsMessengerCallbackDataEXT *data, void *user)
{
    (void)type;
    (void)user;
    fprintf(stderr, "VALIDATION: %s\n", data->pMessage);
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
        errors++;
    return VK_FALSE;
}
int main(void)
{
    int validation = getenv("ZVK_VALIDATE") != NULL;
    const char *extensions[] = {
        VK_KHR_SURFACE_EXTENSION_NAME, VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME,
        VK_EXT_DEBUG_UTILS_EXTENSION_NAME, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME};
    const char *layer = "VK_LAYER_KHRONOS_validation";
    VkDebugUtilsMessengerCreateInfoEXT debug = {
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT,
        .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
        .pfnUserCallback = debug_message};
    VkValidationFeatureEnableEXT sync = VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
    VkValidationFeaturesEXT features = {.sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT,
                                        .pNext = &debug,
                                        .enabledValidationFeatureCount = 1,
                                        .pEnabledValidationFeatures = &sync};
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                             .apiVersion = VK_API_VERSION_1_0,
                             .pApplicationName = "zvk loader WSI test"};
    VkInstanceCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                               .pNext = validation ? (void *)&features : (void *)&debug,
                               .pApplicationInfo = &app,
                               .enabledExtensionCount = validation ? 4 : 3,
                               .ppEnabledExtensionNames = extensions,
                               .enabledLayerCount = validation ? 1 : 0,
                               .ppEnabledLayerNames = &layer};
    VkInstance instance;
    VK(vkCreateInstance(&ii, NULL, &instance));
    PFN_vkCreateDebugUtilsMessengerEXT create_debug =
        (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance,
                                                                  "vkCreateDebugUtilsMessengerEXT");
    PFN_vkDestroyDebugUtilsMessengerEXT destroy_debug =
        (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
            instance, "vkDestroyDebugUtilsMessengerEXT");
    VkDebugUtilsMessengerEXT messenger;
    VK(create_debug(instance, &debug, NULL, &messenger));
    PFN_vkCreateHeadlessSurfaceEXT create_surface =
        (PFN_vkCreateHeadlessSurfaceEXT)vkGetInstanceProcAddr(instance,
                                                              "vkCreateHeadlessSurfaceEXT");
    VkHeadlessSurfaceCreateInfoEXT hi = {.sType =
                                             VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT};
    VkSurfaceKHR surface;
    VK(create_surface(instance, &hi, NULL, &surface));
    uint32_t count = 1;
    VkPhysicalDevice physical;
    VK(vkEnumeratePhysicalDevices(instance, &count, &physical));
    if (!count)
        return 2;
    VkBool32 presentable;
    VK(vkGetPhysicalDeviceSurfaceSupportKHR(physical, 0, surface, &presentable));
    if (!presentable)
        return 2;
    VkSurfaceFormatKHR formats[2];
    count = 2;
    VK(vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &count, formats));
    VkSurfaceCapabilitiesKHR caps;
    VK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &caps));
    float priority = 1;
    VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                  .queueCount = 1,
                                  .pQueuePriorities = &priority};
    const char *swapchain_extension = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                             .queueCreateInfoCount = 1,
                             .pQueueCreateInfos = &qi,
                             .enabledExtensionCount = 1,
                             .ppEnabledExtensionNames = &swapchain_extension};
    VkDevice device;
    VK(vkCreateDevice(physical, &di, NULL, &device));
    VkQueue queue;
    vkGetDeviceQueue(device, 0, 0, &queue);
    VkSwapchainCreateInfoKHR ci = {.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
                                   .surface = surface,
                                   .minImageCount = caps.minImageCount,
                                   .imageFormat = formats[0].format,
                                   .imageColorSpace = formats[0].colorSpace,
                                   .imageExtent = {32, 32},
                                   .imageArrayLayers = 1,
                                   .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                   .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
                                   .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
                                   .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
                                   .presentMode = VK_PRESENT_MODE_FIFO_KHR,
                                   .clipped = VK_TRUE};
    VkSwapchainKHR chain;
    VK(vkCreateSwapchainKHR(device, &ci, NULL, &chain));
    VkImage images[3];
    count = 3;
    VK(vkGetSwapchainImagesKHR(device, chain, &count, images));
    VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                  .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
    VkCommandPool pool;
    VK(vkCreateCommandPool(device, &pi, NULL, &pool));
    VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                      .commandPool = pool,
                                      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                      .commandBufferCount = 1};
    VkCommandBuffer command;
    VK(vkAllocateCommandBuffers(device, &ai, &command));
    VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkSemaphore acquired, finished;
    VK(vkCreateSemaphore(device, &si, NULL, &acquired));
    VK(vkCreateSemaphore(device, &si, NULL, &finished));
    VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    VK(vkCreateFence(device, &fi, NULL, &fence));
    for (unsigned frame = 0; frame < 6; frame++) {
        uint32_t index;
        VK(vkAcquireNextImageKHR(device, chain, UINT64_MAX, acquired, VK_NULL_HANDLE, &index));
        VK(vkResetCommandBuffer(command, 0));
        VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        VK(vkBeginCommandBuffer(command, &begin));
        VkImageMemoryBarrier barrier = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = images[index],
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
        VkClearColorValue color = {{(float)frame / 6, .25f, .5f, 1}};
        vkCmdClearColorImage(command, images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color,
                             1, &barrier.subresourceRange);
        barrier.oldLayout = barrier.newLayout;
        barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = 0;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1,
                             &barrier);
        VK(vkEndCommandBuffer(command));
        VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                               .waitSemaphoreCount = 1,
                               .pWaitSemaphores = &acquired,
                               .pWaitDstStageMask = &stage,
                               .commandBufferCount = 1,
                               .pCommandBuffers = &command,
                               .signalSemaphoreCount = 1,
                               .pSignalSemaphores = &finished};
        VK(vkResetFences(device, 1, &fence));
        VK(vkQueueSubmit(queue, 1, &submit, fence));
        VK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
        VkPresentInfoKHR present = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                                    .waitSemaphoreCount = 1,
                                    .pWaitSemaphores = &finished,
                                    .swapchainCount = 1,
                                    .pSwapchains = &chain,
                                    .pImageIndices = &index};
        VK(vkQueuePresentKHR(queue, &present));
    }
    VK(vkDeviceWaitIdle(device));
    vkDestroyFence(device, fence, NULL);
    vkDestroySemaphore(device, acquired, NULL);
    vkDestroySemaphore(device, finished, NULL);
    vkDestroyCommandPool(device, pool, NULL);
    vkDestroySwapchainKHR(device, chain, NULL);
    vkDestroyDevice(device, NULL);
    vkDestroySurfaceKHR(instance, surface, NULL);
    destroy_debug(instance, messenger, NULL);
    vkDestroyInstance(instance, NULL);
    printf("LOADER_WSI_RESULT validation=%d frames=6 errors=%u\n", validation, errors);
    return errors ? 1 : 0;
}
