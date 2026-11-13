/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (C) 2026 Rigby Foundation */
/* Entry points come exclusively from SDL's dynamically loaded loader. */
#define SDL_VULKAN_FUNCTIONS(X)                                                                    \
    X(vkAcquireNextImageKHR)                                                                       \
    X(vkAllocateCommandBuffers)                                                                    \
    X(vkAllocateMemory)                                                                            \
    X(vkBeginCommandBuffer)                                                                        \
    X(vkBindBufferMemory)                                                                          \
    X(vkCmdBeginRenderPass)                                                                        \
    X(vkCmdBindPipeline)                                                                           \
    X(vkCmdBindVertexBuffers)                                                                      \
    X(vkCmdCopyImageToBuffer)                                                                      \
    X(vkCmdDraw)                                                                                   \
    X(vkCmdEndRenderPass)                                                                          \
    X(vkCmdPipelineBarrier)                                                                        \
    X(vkCmdPushConstants)                                                                          \
    X(vkCmdSetScissor)                                                                             \
    X(vkCmdSetViewport)                                                                            \
    X(vkCreateBuffer)                                                                              \
    X(vkCreateCommandPool)                                                                         \
    X(vkCreateDevice)                                                                              \
    X(vkCreateFence)                                                                               \
    X(vkCreateFramebuffer)                                                                         \
    X(vkCreateGraphicsPipelines)                                                                   \
    X(vkCreateImageView)                                                                           \
    X(vkCreateInstance)                                                                            \
    X(vkCreatePipelineLayout)                                                                      \
    X(vkCreateRenderPass)                                                                          \
    X(vkCreateSemaphore)                                                                           \
    X(vkCreateShaderModule)                                                                        \
    X(vkCreateSwapchainKHR)                                                                        \
    X(vkDestroyBuffer)                                                                             \
    X(vkDestroyCommandPool)                                                                        \
    X(vkDestroyDevice)                                                                             \
    X(vkDestroyFence)                                                                              \
    X(vkDestroyFramebuffer)                                                                        \
    X(vkDestroyImageView)                                                                          \
    X(vkDestroyInstance)                                                                           \
    X(vkDestroyPipeline)                                                                           \
    X(vkDestroyPipelineLayout)                                                                     \
    X(vkDestroyRenderPass)                                                                         \
    X(vkDestroySemaphore)                                                                          \
    X(vkDestroyShaderModule)                                                                       \
    X(vkDestroySurfaceKHR)                                                                         \
    X(vkDestroySwapchainKHR)                                                                       \
    X(vkDeviceWaitIdle)                                                                            \
    X(vkEndCommandBuffer)                                                                          \
    X(vkEnumeratePhysicalDevices)                                                                  \
    X(vkFreeMemory)                                                                                \
    X(vkGetBufferMemoryRequirements)                                                               \
    X(vkGetDeviceQueue)                                                                            \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR)                                                   \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR)                                                        \
    X(vkGetSwapchainImagesKHR)                                                                     \
    X(vkMapMemory)                                                                                 \
    X(vkQueuePresentKHR)                                                                           \
    X(vkQueueSubmit)                                                                               \
    X(vkResetCommandBuffer)                                                                        \
    X(vkResetFences)                                                                               \
    X(vkUnmapMemory)                                                                               \
    X(vkWaitForFences)
#define DECLARE(name) static PFN_##name name;
SDL_VULKAN_FUNCTIONS(DECLARE)
#undef DECLARE
static void sdl_vulkan_load(VkInstance instance)
{
    PFN_vkGetInstanceProcAddr gpa =
        (PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr();
    if (!instance) {
        vkCreateInstance = (PFN_vkCreateInstance)gpa(NULL, "vkCreateInstance");
        if (!vkCreateInstance) {
            fprintf(stderr, "Vulkan loader lacks vkCreateInstance\n");
            exit(2);
        }
        return;
    }
#define LOAD(name)                                                                                 \
    do {                                                                                           \
        name = (PFN_##name)gpa(instance, #name);                                                   \
        if (!name) {                                                                               \
            fprintf(stderr, "Missing Vulkan entry point: %s\n", #name);                            \
            exit(2);                                                                               \
        }                                                                                          \
    } while (0);
    SDL_VULKAN_FUNCTIONS(LOAD)
#undef LOAD
}
#undef SDL_VULKAN_FUNCTIONS
