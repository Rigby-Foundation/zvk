/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (C) 2026 Rigby Foundation */
#include <zvk.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned passed, failed;
#define CHECK(x, label)                                                                            \
    do {                                                                                           \
        if (x) {                                                                                   \
            passed++;                                                                              \
            printf("PASS %s\n", label);                                                            \
        } else {                                                                                   \
            failed++;                                                                              \
            printf("FAIL %s: %s\n", label, zvkGetLastError());                                     \
        }                                                                                          \
    } while (0)
#define VK(call)                                                                                   \
    do {                                                                                           \
        VkResult r = (call);                                                                       \
        if (r != VK_SUCCESS) {                                                                     \
            fprintf(stderr, "%s = %d: %s\n", #call, r, zvkGetLastError());                         \
            exit(2);                                                                               \
        }                                                                                          \
    } while (0)
static VkDevice device;
static VkQueue queue;
static VkCommandPool pool;
static VkCommandBuffer command;
static void prepare(VkImage image, VkSemaphore wait, VkSemaphore signal)
{
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    VK(vkBeginCommandBuffer(command, &begin));
    VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                              .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                              .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                              .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                              .image = image,
                              .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                              .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, NULL, 0, NULL, 1, &b);
    VkClearColorValue color = {{.25f, .5f, .75f, 1}};
    vkCmdClearColorImage(command, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1,
                         &b.subresourceRange);
    b.oldLayout = b.newLayout;
    b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = 0;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    VK(vkEndCommandBuffer(command));
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                           .waitSemaphoreCount = wait ? 1 : 0,
                           .pWaitSemaphores = &wait,
                           .pWaitDstStageMask = &stage,
                           .commandBufferCount = 1,
                           .pCommandBuffers = &command,
                           .signalSemaphoreCount = signal ? 1 : 0,
                           .pSignalSemaphores = &signal};
    VK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
}
struct allocations {
    unsigned calls, live, fail;
};
static void *VKAPI_CALL allocate(void *data, size_t size, size_t align,
                                 VkSystemAllocationScope scope)
{
    (void)align;
    (void)scope;
    struct allocations *a = data;
    if (a->calls++ == a->fail)
        return NULL;
    void *p = malloc(size);
    if (p)
        a->live++;
    return p;
}
static void VKAPI_CALL release(void *data, void *p)
{
    if (p) {
        ((struct allocations *)data)->live--;
        free(p);
    }
}
static void *VKAPI_CALL reallocate(void *data, void *p, size_t size, size_t align,
                                   VkSystemAllocationScope scope)
{
    if (!p)
        return allocate(data, size, align, scope);
    return realloc(p, size);
}
int main(void)
{
    uint32_t count = 0;
    VK(vkEnumerateInstanceExtensionProperties(NULL, &count, NULL));
    CHECK(count >= 2, "surface extensions enumerated");
    VkExtensionProperties e;
    count = 1;
    CHECK(vkEnumerateInstanceExtensionProperties(NULL, &count, &e) == VK_INCOMPLETE && count == 1,
          "extension enumeration truncation");
    const char *extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME,
                                VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME};
    VkInstanceCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                               .enabledExtensionCount = 2,
                               .ppEnabledExtensionNames = extensions};
    VkInstance instance;
    VK(vkCreateInstance(&ii, NULL, &instance));
    CHECK(vkGetInstanceProcAddr(instance, "vkCreateHeadlessSurfaceEXT") != NULL,
          "enabled surface dispatch");
    VkHeadlessSurfaceCreateInfoEXT hi = {.sType =
                                             VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT};
    VkSurfaceKHR surface;
    VK(vkCreateHeadlessSurfaceEXT(instance, &hi, NULL, &surface));
    count = 1;
    VkPhysicalDevice physical;
    VK(vkEnumeratePhysicalDevices(instance, &count, &physical));
    VkBool32 supported;
    VK(vkGetPhysicalDeviceSurfaceSupportKHR(physical, 0, surface, &supported));
    CHECK(supported, "graphics queue can present");
    VK(vkGetPhysicalDeviceSurfaceSupportKHR(physical, 1, surface, &supported));
    CHECK(!supported, "invalid queue family cannot present");
    VkSurfaceCapabilitiesKHR caps;
    VK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &caps));
    CHECK(caps.minImageCount == 2 && caps.maxImageCount >= 2 &&
              caps.currentExtent.width == UINT32_MAX,
          "headless surface capabilities");
    count = 0;
    VK(vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &count, NULL));
    CHECK(count > 0, "surface formats");
    count = 1;
    VkSurfaceFormatKHR format;
    VkResult r = vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &count, &format);
    CHECK((r == VK_SUCCESS || r == VK_INCOMPLETE) &&
              format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
          "surface format truncation");
    VkPresentModeKHR mode;
    count = 1;
    VK(vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, &count, &mode));
    CHECK(mode == VK_PRESENT_MODE_FIFO_KHR, "FIFO presentation");
    float priority = 1;
    VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                  .queueCount = 1,
                                  .pQueuePriorities = &priority};
    const char *swapchain_ext = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                             .queueCreateInfoCount = 1,
                             .pQueueCreateInfos = &qi,
                             .enabledExtensionCount = 1,
                             .ppEnabledExtensionNames = &swapchain_ext};
    VK(vkCreateDevice(physical, &di, NULL, &device));
    vkGetDeviceQueue(device, 0, 0, &queue);
    CHECK(vkGetDeviceProcAddr(device, "vkQueuePresentKHR") != NULL, "enabled swapchain dispatch");
    VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                  .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
    VK(vkCreateCommandPool(device, &pi, NULL, &pool));
    VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                      .commandPool = pool,
                                      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                      .commandBufferCount = 1};
    VK(vkAllocateCommandBuffers(device, &ai, &command));
    VkSwapchainCreateInfoKHR ci = {.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
                                   .surface = surface,
                                   .minImageCount = 2,
                                   .imageFormat = format.format,
                                   .imageColorSpace = format.colorSpace,
                                   .imageExtent = {32, 32},
                                   .imageArrayLayers = 1,
                                   .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                                 VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                   .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
                                   .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
                                   .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
                                   .presentMode = VK_PRESENT_MODE_FIFO_KHR,
                                   .clipped = VK_TRUE};
    VkSwapchainKHR chain;
    VK(vkCreateSwapchainKHR(device, &ci, NULL, &chain));
    VkSwapchainKHR duplicate;
    CHECK(vkCreateSwapchainKHR(device, &ci, NULL, &duplicate) == VK_ERROR_NATIVE_WINDOW_IN_USE_KHR,
          "surface has only one nonretired swapchain");
    count = 0;
    VK(vkGetSwapchainImagesKHR(device, chain, &count, NULL));
    CHECK(count == 2, "swapchain images allocated");
    VkImage images[3];
    count = 1;
    CHECK(vkGetSwapchainImagesKHR(device, chain, &count, images) == VK_INCOMPLETE && count == 1,
          "swapchain image truncation");
    count = 3;
    VK(vkGetSwapchainImagesKHR(device, chain, &count, images));
    VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkSemaphore acquired, rendered;
    VK(vkCreateSemaphore(device, &si, NULL, &acquired));
    VK(vkCreateSemaphore(device, &si, NULL, &rendered));
    VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    VK(vkCreateFence(device, &fi, NULL, &fence));
    uint32_t first, second, index = 99;
    VK(vkAcquireNextImageKHR(device, chain, 0, acquired, VK_NULL_HANDLE, &first));
    VK(vkAcquireNextImageKHR(device, chain, 0, VK_NULL_HANDLE, fence, &second));
    CHECK(first != second && vkGetFenceStatus(device, fence) == VK_SUCCESS,
          "acquisition reserves distinct images and signals a fence");
    CHECK(vkAcquireNextImageKHR(device, chain, 0, VK_NULL_HANDLE, VK_NULL_HANDLE, &index) ==
                  VK_NOT_READY &&
              index == 99,
          "zero-time acquisition reports exhaustion");
    CHECK(vkAcquireNextImageKHR(device, chain, 1000000, VK_NULL_HANDLE, VK_NULL_HANDLE, &index) ==
              VK_TIMEOUT,
          "finite acquisition timeout");
    prepare(images[first], acquired, rendered);
    VkResult status = VK_NOT_READY;
    VkPresentInfoKHR present = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                                .waitSemaphoreCount = 1,
                                .pWaitSemaphores = &rendered,
                                .swapchainCount = 1,
                                .pSwapchains = &chain,
                                .pImageIndices = &first,
                                .pResults = &status};
    VK(vkQueuePresentKHR(queue, &present));
    CHECK(status == VK_SUCCESS, "present consumes render signal and reports per-swapchain result");
    VK(vkResetFences(device, 1, &fence));
    VK(vkAcquireNextImageKHR(device, chain, 0, VK_NULL_HANDLE, fence, &index));
    CHECK(index == first, "completed headless presentation releases image");
    ci.oldSwapchain = chain;
    ci.imageExtent = (VkExtent2D){48, 64};
    VkSwapchainKHR replacement;
    VK(vkCreateSwapchainKHR(device, &ci, NULL, &replacement));
    CHECK(vkAcquireNextImageKHR(device, chain, 0, VK_NULL_HANDLE, VK_NULL_HANDLE, &index) ==
              VK_ERROR_OUT_OF_DATE_KHR,
          "replacement retires the old swapchain");
    vkDestroySwapchainKHR(device, chain, NULL);
    vkDestroySwapchainKHR(device, replacement, NULL);
    ci.oldSwapchain = VK_NULL_HANDLE;
    int rollback = 1;
    for (unsigned fail = 0; fail < 16; fail++) {
        struct allocations state = {.fail = fail};
        VkAllocationCallbacks cb = {.pUserData = &state,
                                    .pfnAllocation = allocate,
                                    .pfnReallocation = reallocate,
                                    .pfnFree = release};
        VkSwapchainKHR temp = VK_NULL_HANDLE;
        r = vkCreateSwapchainKHR(device, &ci, &cb, &temp);
        if (r == VK_SUCCESS)
            vkDestroySwapchainKHR(device, temp, &cb);
        else if (r != VK_ERROR_OUT_OF_HOST_MEMORY || temp != VK_NULL_HANDLE)
            rollback = 0;
        if (state.live)
            rollback = 0;
    }
    CHECK(rollback, "swapchain allocation failures release all images and memory");
    vkDestroyFence(device, fence, NULL);
    vkDestroySemaphore(device, acquired, NULL);
    vkDestroySemaphore(device, rendered, NULL);
    vkDestroyCommandPool(device, pool, NULL);
    vkDestroyDevice(device, NULL);
    vkDestroySurfaceKHR(instance, surface, NULL);
    vkDestroyInstance(instance, NULL);
    printf("WSI_RESULT %u passed %u failed\n", passed, failed);
    return failed ? 1 : 0;
}
