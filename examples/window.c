/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (C) 2026 Rigby Foundation */
#ifdef ZVK_SDL
#define VK_NO_PROTOTYPES
#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#endif
#include <zvk_zwm.h>
#include <zwm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <abi/fb.h>
#include "shaders.h"
#ifdef ZVK_SDL
#include "window_sdl.h"
#define zvkGetLastError() SDL_GetError()
#endif
#define VK(call)                                                                                   \
    do {                                                                                           \
        VkResult vk_result_ = (call);                                                              \
        if (vk_result_ != VK_SUCCESS) {                                                            \
            fprintf(stderr, "%s = %d: %s\n", #call, vk_result_, zvkGetLastError());                \
            exit(2);                                                                               \
        }                                                                                          \
    } while (0)
struct app {
    zwm *connection;
    int window;
#ifdef ZVK_SDL
    SDL_Window *sdl_window;
#endif
    VkInstance instance;
    VkPhysicalDevice physical;
    VkSurfaceKHR surface;
    VkQueue queue;
    VkSwapchainKHR chain;
    VkExtent2D extent;
    VkFormat format;
    uint32_t count;
    VkImage images[3];
    VkImageView views[3];
    VkFramebuffer frames[3];
    VkRenderPass pass;
    VkPipelineLayout layout;
    VkPipeline pipeline;
    VkBuffer vertices, readback;
    VkDeviceMemory vertex_memory, readback_memory;
    uint8_t *pixels;
    VkCommandPool pool;
    VkCommandBuffer command;
    VkSemaphore acquired, rendered;
    VkFence fence;
};
#ifdef ZVK_SDL
static void *resume_compositor(void *argument)
{
    pid_t pid = *(pid_t *)argument;
    usleep(3000000);
    return (void *)(intptr_t)(kill(pid, SIGCONT) != 0);
}
#endif
static VkDevice device;
static VkShaderModule shader(const uint32_t *code, size_t size)
{
    VkShaderModuleCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = size, .pCode = code};
    VkShaderModule module;
    VK(vkCreateShaderModule(device, &info, NULL, &module));
    return module;
}
static VkBuffer buffer(size_t size, VkBufferUsageFlags usage, VkDeviceMemory *memory, void **map)
{
    VkBufferCreateInfo info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                               .size = size,
                               .usage = usage,
                               .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkBuffer b;
    VK(vkCreateBuffer(device, &info, NULL, &b));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, b, &req);
    VkMemoryAllocateInfo alloc = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                  .allocationSize = req.size};
    VK(vkAllocateMemory(device, &alloc, NULL, memory));
    VK(vkBindBufferMemory(device, b, *memory, 0));
    VK(vkMapMemory(device, *memory, 0, VK_WHOLE_SIZE, 0, map));
    return b;
}
static void make_pipeline(struct app *a)
{
    VkPushConstantRange push = {VK_SHADER_STAGE_VERTEX_BIT, 0, 16};
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                     .pushConstantRangeCount = 1,
                                     .pPushConstantRanges = &push};

    VK(vkCreatePipelineLayout(device, &li, NULL, &a->layout));
    VkShaderModule vs = shader(window_vert, sizeof window_vert),
                   fs = shader(triangle_frag, sizeof triangle_frag);
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vs,
         .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = fs,
         .pName = "main"}};
    VkVertexInputBindingDescription binding = {0, 20, VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attrs[2] = {{0, 0, VK_FORMAT_R32G32_SFLOAT, 0},
                                                  {1, 0, VK_FORMAT_R32G32B32_SFLOAT, 8}};
    VkPipelineVertexInputStateCreateInfo vertex = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1,
        .pVertexBindingDescriptions = &binding,
        .vertexAttributeDescriptionCount = 2,
        .pVertexAttributeDescriptions = attrs};
    VkPipelineInputAssemblyStateCreateInfo assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};

    VkPipelineViewportStateCreateInfo viewport_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,

        .scissorCount = 1,
    };
    VkPipelineRasterizationStateCreateInfo raster = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo samples = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState blend = {.colorWriteMask = 15};
    VkPipelineColorBlendStateCreateInfo blend_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &blend};
    VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2,
        .pDynamicStates = dynamic_states};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                       .stageCount = 2,
                                       .pStages = stages,
                                       .pVertexInputState = &vertex,
                                       .pInputAssemblyState = &assembly,
                                       .pViewportState = &viewport_state,
                                       .pRasterizationState = &raster,
                                       .pMultisampleState = &samples,
                                       .pColorBlendState = &blend_state,
                                       .pDynamicState = &dynamic,
                                       .layout = a->layout,
                                       .renderPass = a->pass,
                                       .basePipelineIndex = -1};

    VK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pi, NULL, &a->pipeline));
    vkDestroyShaderModule(device, vs, NULL);
    vkDestroyShaderModule(device, fs, NULL);
}
static void recreate(struct app *a)
{
    VK(vkDeviceWaitIdle(device));
    VkSurfaceCapabilitiesKHR caps;
    VK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(a->physical, a->surface, &caps));
    VkSwapchainCreateInfoKHR info = {.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
                                     .surface = a->surface,
                                     .minImageCount = 2,
                                     .imageFormat = a->format,
                                     .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
                                     .imageExtent = caps.currentExtent,
                                     .imageArrayLayers = 1,
                                     .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                                   VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                     .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
                                     .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
                                     .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
                                     .presentMode = VK_PRESENT_MODE_FIFO_KHR,
                                     .clipped = VK_TRUE,
                                     .oldSwapchain = a->chain};
    VkSwapchainKHR next;
    VK(vkCreateSwapchainKHR(device, &info, NULL, &next));
    for (uint32_t i = 0; i < a->count; i++) {
        vkDestroyFramebuffer(device, a->frames[i], NULL);
        vkDestroyImageView(device, a->views[i], NULL);
    }
    if (a->chain)
        vkDestroySwapchainKHR(device, a->chain, NULL);
    a->chain = next;
    a->extent = caps.currentExtent;
    a->count = 3;
    VK(vkGetSwapchainImagesKHR(device, a->chain, &a->count, a->images));
    for (uint32_t i = 0; i < a->count; i++) {
        VkImageViewCreateInfo view = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                      .image = a->images[i],
                                      .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                      .format = a->format,
                                      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        VK(vkCreateImageView(device, &view, NULL, a->views + i));
        VkFramebufferCreateInfo fb = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                      .renderPass = a->pass,
                                      .attachmentCount = 1,
                                      .pAttachments = a->views + i,
                                      .width = a->extent.width,
                                      .height = a->extent.height,
                                      .layers = 1};
        VK(vkCreateFramebuffer(device, &fb, NULL, a->frames + i));
    }
    printf("SWAPCHAIN %ux%u images=%u\n", a->extent.width, a->extent.height, a->count);
}
static void setup(struct app *a)
{
#ifdef ZVK_SDL
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        exit(2);
    }
    if (SDL_Vulkan_LoadLibrary("/nonexistent-vulkan-loader.so") == 0) {
        fprintf(stderr, "Missing Vulkan loader unexpectedly succeeded\n");
        exit(2);
    }
    SDL_ClearError();
    if (SDL_Vulkan_LoadLibrary(NULL) < 0) {
        fprintf(stderr, "SDL Vulkan: %s\n", SDL_GetError());
        exit(2);
    }
    printf("SDL_LOAD_FAILURE_PASS\n");
    sdl_vulkan_load(VK_NULL_HANDLE);
    a->sdl_window =
        SDL_CreateWindow("SDL2 Vulkan on sic", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                         480, 360, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
    if (!a->sdl_window) {
        fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        exit(2);
    }
    const char *extensions[2];
    unsigned extension_count = 2;
    if (!SDL_Vulkan_GetInstanceExtensions(a->sdl_window, &extension_count, extensions) ||
        extension_count != 2) {
        fprintf(stderr, "SDL Vulkan extensions: %s\n", SDL_GetError());
        exit(2);
    }
#else
    a->connection = zwm_connect();
    if (!a->connection) {
        perror("zwm_connect");
        exit(2);
    }
    a->window = zwm_create(a->connection, 480, 360, "Vulkan on sic", 0, NULL);
    if (a->window < 0) {
        perror("zwm_create");
        exit(2);
    }
    const char *extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_SIC_ZWM_SURFACE_EXTENSION_NAME};
#endif
    VkInstanceCreateInfo instance = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                     .enabledExtensionCount = 2,
                                     .ppEnabledExtensionNames = extensions};
    VK(vkCreateInstance(&instance, NULL, &a->instance));
#ifdef ZVK_SDL
    sdl_vulkan_load(a->instance);
    if (!SDL_Vulkan_CreateSurface(a->sdl_window, a->instance, &a->surface)) {
        fprintf(stderr, "SDL_Vulkan_CreateSurface: %s\n", SDL_GetError());
        exit(2);
    }
#else
    VkZwmSurfaceCreateInfoSIC surface = {.sType = VK_STRUCTURE_TYPE_ZWM_SURFACE_CREATE_INFO_SIC,
                                         .connection = a->connection,
                                         .window = (uint32_t)a->window};
    VK(vkCreateZwmSurfaceSIC(a->instance, &surface, NULL, &a->surface));
#endif
    uint32_t count = 1;
    VK(vkEnumeratePhysicalDevices(a->instance, &count, &a->physical));
    if (!count) {
        fprintf(stderr, "No virgl Vulkan device\n");
        exit(2);
    }
    float priority = 1;
    VkDeviceQueueCreateInfo queue = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                     .queueCount = 1,
                                     .pQueuePriorities = &priority};
    const char *swapchain = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    VkDeviceCreateInfo create = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                                 .queueCreateInfoCount = 1,
                                 .pQueueCreateInfos = &queue,
                                 .enabledExtensionCount = 1,
                                 .ppEnabledExtensionNames = &swapchain};
    VK(vkCreateDevice(a->physical, &create, NULL, &device));
    vkGetDeviceQueue(device, 0, 0, &a->queue);
    VkSurfaceFormatKHR formats[2];
    count = 2;
    VK(vkGetPhysicalDeviceSurfaceFormatsKHR(a->physical, a->surface, &count, formats));
    a->format = formats[0].format;
    VkAttachmentDescription attachment = {.format = a->format,
                                          .samples = VK_SAMPLE_COUNT_1_BIT,
                                          .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                                          .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                          .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                          .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                          .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                          .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR};
    VkAttachmentReference color = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    .colorAttachmentCount = 1,
                                    .pColorAttachments = &color};
    VkSubpassDependency dependency = {.srcSubpass = 0,
                                      .dstSubpass = VK_SUBPASS_EXTERNAL,
                                      .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                      .dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT,
                                      .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                                      .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT};
    VkSubpassDependency dependencies[2] = {
        {.srcSubpass = VK_SUBPASS_EXTERNAL,
         .dstSubpass = 0,
         .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
         .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
         .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT},
        dependency};
    VkRenderPassCreateInfo pass = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                   .attachmentCount = 1,
                                   .pAttachments = &attachment,
                                   .subpassCount = 1,
                                   .pSubpasses = &subpass,
                                   .dependencyCount = 2,
                                   .pDependencies = dependencies};
    VK(vkCreateRenderPass(device, &pass, NULL, &a->pass));
    make_pipeline(a);
    float vertices[] = {-.75f, -.75f, 1, 0, 0, .75f, -.75f, 0, 1, 0, 0, .75f, 0, 0, 1};
    void *map;
    a->vertices =
        buffer(sizeof vertices, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &a->vertex_memory, &map);
    memcpy(map, vertices, sizeof vertices);
    a->readback =
        buffer(8, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &a->readback_memory, (void **)&a->pixels);
    VkCommandPoolCreateInfo pool = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                    .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
    VK(vkCreateCommandPool(device, &pool, NULL, &a->pool));
    VkCommandBufferAllocateInfo command = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                           .commandPool = a->pool,
                                           .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                           .commandBufferCount = 1};
    VK(vkAllocateCommandBuffers(device, &command, &a->command));
    VkSemaphoreCreateInfo semaphore = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VK(vkCreateSemaphore(device, &semaphore, NULL, &a->acquired));
    VK(vkCreateSemaphore(device, &semaphore, NULL, &a->rendered));
    VkFenceCreateInfo fence = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VK(vkCreateFence(device, &fence, NULL, &a->fence));
    recreate(a);
}
static VkResult frame(struct app *a, unsigned number, int verify)
{
    uint32_t image;
    VkResult r =
        vkAcquireNextImageKHR(device, a->chain, UINT64_MAX, a->acquired, VK_NULL_HANDLE, &image);
    if (r != VK_SUCCESS)
        return r;
    VK(vkResetCommandBuffer(a->command, 0));
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    VK(vkBeginCommandBuffer(a->command, &begin));
    VkClearValue clear = {.color = {{.025f, .03f, .05f, 1}}};
    VkRenderPassBeginInfo pass = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                  .renderPass = a->pass,
                                  .framebuffer = a->frames[image],
                                  .renderArea = {{0, 0}, a->extent},
                                  .clearValueCount = 1,
                                  .pClearValues = &clear};
    vkCmdBeginRenderPass(a->command, &pass, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(a->command, VK_PIPELINE_BIND_POINT_GRAPHICS, a->pipeline);
    VkViewport viewport = {0, 0, (float)a->extent.width, (float)a->extent.height, 0, 1};
    VkRect2D scissor = {{0, 0}, a->extent};
    vkCmdSetViewport(a->command, 0, 1, &viewport);
    vkCmdSetScissor(a->command, 0, 1, &scissor);
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(a->command, 0, 1, &a->vertices, &offset);
    float angle = (float)number * .035f;
    float aspect = (float)a->extent.height / a->extent.width;
    float transform[4] = {cosf(angle), sinf(angle), aspect < 1 ? aspect : 1,
                          aspect > 1 ? 1 / aspect : 1};
    vkCmdPushConstants(a->command, a->layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 16, transform);
    vkCmdDraw(a->command, 3, 1, 0, 0);
    vkCmdEndRenderPass(a->command);
    if (verify) {
        VkImageMemoryBarrier barrier = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = a->images[image],
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        vkCmdPipelineBarrier(a->command, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
        VkBufferImageCopy copies[2] = {
            {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
             .imageOffset = {(int32_t)a->extent.width / 2, (int32_t)a->extent.height / 2, 0},
             .imageExtent = {1, 1, 1}},
            {.bufferOffset = 4,
             .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
             .imageOffset = {(int32_t)a->extent.width / 3, (int32_t)a->extent.height / 2, 0},
             .imageExtent = {1, 1, 1}}};
        vkCmdCopyImageToBuffer(a->command, a->images[image], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               a->readback, 2, copies);
        barrier.oldLayout = barrier.newLayout;
        barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.dstAccessMask = 0;
        vkCmdPipelineBarrier(a->command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1,
                             &barrier);
    }
    VK(vkEndCommandBuffer(a->command));
    VK(vkResetFences(device, 1, &a->fence));
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                           .waitSemaphoreCount = 1,
                           .pWaitSemaphores = &a->acquired,
                           .pWaitDstStageMask = &stage,
                           .commandBufferCount = 1,
                           .pCommandBuffers = &a->command,
                           .signalSemaphoreCount = 1,
                           .pSignalSemaphores = &a->rendered};
    VK(vkQueueSubmit(a->queue, 1, &submit, a->fence));
    VK(vkWaitForFences(device, 1, &a->fence, VK_TRUE, 10000000000ull));
    if (verify) {
        int red = a->format == VK_FORMAT_B8G8R8A8_UNORM ? 2 : 0, blue = 2 - red;
        if (a->pixels[red] < 20 || a->pixels[1] < 20 || a->pixels[blue] < 80 ||
            a->pixels[3] != 255) {
            fprintf(stderr, "Bad animated triangle readback %u %u %u %u\n", a->pixels[0],
                    a->pixels[1], a->pixels[2], a->pixels[3]);
            exit(2);
        }
    }
    VkPresentInfoKHR present = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                                .waitSemaphoreCount = 1,
                                .pWaitSemaphores = &a->rendered,
                                .swapchainCount = 1,
                                .pSwapchains = &a->chain,
                                .pImageIndices = &image};
    return vkQueuePresentKHR(a->queue, &present);
}
static void cleanup(struct app *a)
{
    VK(vkDeviceWaitIdle(device));
    vkDestroyCommandPool(device, a->pool, NULL);
    for (uint32_t i = 0; i < a->count; i++) {
        vkDestroyFramebuffer(device, a->frames[i], NULL);
        vkDestroyImageView(device, a->views[i], NULL);
    }
    vkDestroySwapchainKHR(device, a->chain, NULL);
    vkDestroyFence(device, a->fence, NULL);
    vkDestroySemaphore(device, a->acquired, NULL);
    vkDestroySemaphore(device, a->rendered, NULL);
    vkDestroyPipeline(device, a->pipeline, NULL);
    vkDestroyPipelineLayout(device, a->layout, NULL);
    vkDestroyRenderPass(device, a->pass, NULL);
    vkUnmapMemory(device, a->vertex_memory);
    vkUnmapMemory(device, a->readback_memory);
    vkDestroyBuffer(device, a->vertices, NULL);
    vkDestroyBuffer(device, a->readback, NULL);
    vkFreeMemory(device, a->vertex_memory, NULL);
    vkFreeMemory(device, a->readback_memory, NULL);
    vkDestroyDevice(device, NULL);
    vkDestroySurfaceKHR(a->instance, a->surface, NULL);
    vkDestroyInstance(a->instance, NULL);
#ifdef ZVK_SDL
    SDL_DestroyWindow(a->sdl_window);
    SDL_Vulkan_UnloadLibrary();
    SDL_Quit();
#else
    zwm_destroy(a->connection, a->window);
    zwm_disconnect(a->connection);
#endif
}
int main(int argc, char **argv)
{
    int selftest = argc > 1 && !strcmp(argv[1], "--selftest");
    unsigned frame_limit = 0;
    if (argc == 3 && !strcmp(argv[1], "--frames"))
        frame_limit = (unsigned)strtoul(argv[2], NULL, 10);
    else if (argc > 1 && !selftest) {
        fprintf(stderr, "usage: zvkwindow [--selftest | --frames count]\n");
        return 2;
    }
    struct app a = {0};
    setup(&a);
    unsigned frames = 0, resizes = 0, recreations = 0, changes = 0;
    uint32_t last = 0;
    int done = 0;
#ifdef ZVK_SDL
    pthread_t resume_thread;
    pid_t compositor = 0;
    int stalled = 0;
    struct timespec stall_start;
#endif
    while (!done) {
#ifdef ZVK_SDL
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT ||
                (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE) ||
                (event.type == SDL_KEYDOWN &&
                 (event.key.keysym.sym == SDLK_q || event.key.keysym.sym == SDLK_ESCAPE)))
                done = 1;
            if (event.type == SDL_WINDOWEVENT &&
                event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                int width, height;
                SDL_Vulkan_GetDrawableSize(a.sdl_window, &width, &height);
                resizes++;
                printf("SDL_RESIZE %dx%d drawable=%dx%d\n", event.window.data1, event.window.data2,
                       width, height);
            }
        }
#else
        zwm_event event;
        while (zwm_next_event(a.connection, &event, 0) > 0) {
            if (event.win != (uint32_t)a.window)
                continue;
            if (event.type == ZWM_S_CLOSE || (event.type == ZWM_S_KEY && event.key.down &&
                                              (event.key.sym == 27 || event.key.sym == 'q')))
                done = 1;
            if (event.type == ZWM_S_RESIZE) {
                resizes++;
                printf("RESIZE %dx%d\n", event.geom.w, event.geom.h);
            }
        }
#endif
        if (done)
            break;
        VkResult r = frame(&a, frames, selftest);
#ifdef ZVK_SDL
        if (stalled) {
            struct timespec end;
            clock_gettime(CLOCK_MONOTONIC, &end);
            int64_t elapsed = (int64_t)(end.tv_sec - stall_start.tv_sec) * 1000 +
                              (end.tv_nsec - stall_start.tv_nsec) / 1000000;
            void *resume_error = NULL;
            pthread_join(resume_thread, &resume_error);
            if (resume_error || elapsed < 2800 || r != VK_SUCCESS) {
                fprintf(stderr, "Slow compositor wait failed: %lldms, result %d\n",
                        (long long)elapsed, r);
                return 2;
            }
            printf("SDL_SLOW_FRAME_PASS %lldms\n", (long long)elapsed);
            stalled = 0;
        }
#endif
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
            recreate(&a);
            recreations++;
            continue;
        }
        VK(r);
        uint32_t sample;
        memcpy(&sample, a.pixels + 4, 4);
        if (frames && sample != last)
            changes++;
        last = sample;
        frames++;
#ifdef ZVK_SDL
        if (selftest && frames == 4 && getenv("ZVK_TEST_COMPOSITOR_PID")) {
            compositor = (pid_t)strtol(getenv("ZVK_TEST_COMPOSITOR_PID"), NULL, 10);
            if (compositor <= 1 || kill(compositor, SIGSTOP)) {
                perror("stop test compositor");
                return 2;
            }
            clock_gettime(CLOCK_MONOTONIC, &stall_start);
            puts("SDL_SLOW_FRAME_BEGIN");
            if (pthread_create(&resume_thread, NULL, resume_compositor, &compositor)) {
                kill(compositor, SIGCONT);
                return 2;
            }
            stalled = 1;
        }
#endif
        if (selftest && (frames == 8 || frames == 24 || frames == 48)) {
            int fd = open("/dev/fb0", O_RDONLY);
            struct fb_fix_screeninfo fix;
            struct fb_var_screeninfo var;
            if (fd < 0 || ioctl(fd, FBIOGET_FSCREENINFO, &fix) ||
                ioctl(fd, FBIOGET_VSCREENINFO, &var)) {
                perror("framebuffer screenshot info");
                return 2;
            }
            close(fd);
            printf("WINDOW_CAPTURE %u %llu %u %u %u\n", frames, (unsigned long long)fix.smem_start,
                   var.xres, var.yres, fix.line_length);
            fflush(stdout);
            usleep(400000);
        }
#ifdef ZVK_SDL
        if (selftest && frames == 12)
            SDL_MaximizeWindow(a.sdl_window);
        if (selftest && frames == 32)
            SDL_RestoreWindow(a.sdl_window);
        if (frame_limit && frames >= frame_limit)
            done = 1;
        if (selftest && frames == 64) {
            printf("SDL_CLOSE_REQUEST\n");
            fflush(stdout);
        }
#else
        if (selftest && (frames == 12 || frames == 32))
            zwm_maximize(a.connection, a.window);
        if ((selftest && frames == 64) || (frame_limit && frames == frame_limit))
            zwm_close(a.connection, a.window);
#endif
        if (selftest && frames > 80) {
            fprintf(stderr, "Close event did not arrive\n");
            return 2;
        }
        if (selftest)
            usleep(16000);
    }
    cleanup(&a);
    if (selftest) {
        int good = frames >= 64 && resizes >= 2 && recreations >= 2 && changes >= 10;
#ifdef ZVK_SDL
        printf("SDL_WINDOW_RESULT frames=%u resizes=%u recreations=%u animated_samples=%u %s\n",
               frames, resizes, recreations, changes, good ? "PASS" : "FAIL");
#else
        printf("WINDOW_RESULT frames=%u resizes=%u recreations=%u animated_samples=%u %s\n", frames,
               resizes, recreations, changes, good ? "PASS" : "FAIL");
#endif
        return good ? 0 : 1;
    }
    if (frame_limit)
        printf("WINDOW_RUN frames=%u PASS\n", frames);
    return 0;
}
