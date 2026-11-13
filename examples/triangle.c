/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (C) 2026 Rigby Foundation */
/* Offscreen Vulkan triangle, directly linked to libzvk. The generated shader
 * header comes from scripts/shaders.py; glslang is not needed at runtime. */
#include <zvk.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "shaders.h"
#define WIDTH 128
#define HEIGHT 128
#define VK(call)                                                                                   \
    do {                                                                                           \
        VkResult result = (call);                                                                  \
        if (result != VK_SUCCESS) {                                                                \
            fprintf(stderr, "%s: %d (%s)\n", #call, result, zvkGetLastError());                    \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
#ifdef ZVK_LOADER
static unsigned validation_errors;
static VKAPI_ATTR VkBool32 VKAPI_CALL
debug_message(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT type,
              const VkDebugUtilsMessengerCallbackDataEXT *data, void *user)
{
    (void)type;
    (void)user;
    fprintf(stderr, "VALIDATION: %s\n", data->pMessage);
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
        validation_errors++;
    return VK_FALSE;
}
#define zvkGetLastError() "Vulkan loader/driver error"
#endif
static VkDevice device;
static VkDeviceMemory allocate(VkMemoryRequirements req)
{
    VkMemoryAllocateInfo info = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                 .allocationSize = req.size,
                                 .memoryTypeIndex = 0};
    VkDeviceMemory memory;
    VK(vkAllocateMemory(device, &info, NULL, &memory));
    return memory;
}
static VkBuffer buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkDeviceMemory *memory,
                       void **map)
{
    VkBufferCreateInfo info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                               .size = size,
                               .usage = usage,
                               .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkBuffer b;
    VK(vkCreateBuffer(device, &info, NULL, &b));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, b, &req);
    *memory = allocate(req);
    VK(vkBindBufferMemory(device, b, *memory, 0));
    VK(vkMapMemory(device, *memory, 0, VK_WHOLE_SIZE, 0, map));
    return b;
}
static VkShaderModule shader(const uint32_t *code, size_t bytes)
{
    VkShaderModuleCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = bytes, .pCode = code};
    VkShaderModule module;
    VK(vkCreateShaderModule(device, &info, NULL, &module));
    return module;
}
int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "triangle.ppm";
    VkInstanceCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
#ifdef ZVK_LOADER
    const char *layer = "VK_LAYER_KHRONOS_validation";
    const char *debug_extensions[] = {VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
                                      VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME};
    int validation = getenv("ZVK_VALIDATE") != NULL;
    VkDebugUtilsMessengerCreateInfoEXT debug = {
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT,
        .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
        .pfnUserCallback = debug_message};
    VkValidationFeatureEnableEXT sync = VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
    VkValidationFeaturesEXT validation_features = {.sType =
                                                       VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT,
                                                   .pNext = &debug,
                                                   .enabledValidationFeatureCount = 1,
                                                   .pEnabledValidationFeatures = &sync};
    ii.enabledExtensionCount = validation ? 2 : 1;
    ii.ppEnabledExtensionNames = debug_extensions;
    ii.enabledLayerCount = validation ? 1 : 0;
    ii.ppEnabledLayerNames = &layer;
    ii.pNext = validation ? (void *)&validation_features : (void *)&debug;
#endif
    VkInstance instance;
    VK(vkCreateInstance(&ii, NULL, &instance));
#ifdef ZVK_LOADER
    VkDebugUtilsMessengerEXT messenger;
    PFN_vkCreateDebugUtilsMessengerEXT create_debug =
        (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance,
                                                                  "vkCreateDebugUtilsMessengerEXT");
    PFN_vkDestroyDebugUtilsMessengerEXT destroy_debug =
        (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
            instance, "vkDestroyDebugUtilsMessengerEXT");
    if (!create_debug || !destroy_debug) {
        fprintf(stderr, "Loader debug extension unavailable\n");
        return 1;
    }
    VK(create_debug(instance, &debug, NULL, &messenger));
#endif
    uint32_t count = 1;
    VkPhysicalDevice physical;
    VK(vkEnumeratePhysicalDevices(instance, &count, &physical));
    if (!count) {
        fprintf(stderr, "A virgl GPU is required.\n");
        vkDestroyInstance(instance, NULL);
        return 1;
    }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(physical, &props);
    printf("%s\n", props.deviceName);
    float priority = 1;
    VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                  .queueCount = 1,
                                  .pQueuePriorities = &priority};
    VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                             .queueCreateInfoCount = 1,
                             .pQueueCreateInfos = &qi};
    VK(vkCreateDevice(physical, &di, NULL, &device));
    VkQueue queue;
    vkGetDeviceQueue(device, 0, 0, &queue);
    const float vertices[] = {-.8f, -.8f, 1, 0, 0, .8f, -.8f, 0, 1, 0, 0, .8f, 0, 0, 1};
    VkDeviceMemory vb_memory, rb_memory;
    void *vb_map, *pixels;
    VkBuffer vb = buffer(sizeof vertices, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &vb_memory, &vb_map),
             rb = buffer(WIDTH * HEIGHT * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &rb_memory, &pixels);
    memcpy(vb_map, vertices, sizeof vertices);
    VkImageCreateInfo image_info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                    .imageType = VK_IMAGE_TYPE_2D,
                                    .format = VK_FORMAT_R8G8B8A8_UNORM,
                                    .extent = {WIDTH, HEIGHT, 1},
                                    .mipLevels = 1,
                                    .arrayLayers = 1,
                                    .samples = VK_SAMPLE_COUNT_1_BIT,
                                    .tiling = VK_IMAGE_TILING_OPTIMAL,
                                    .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                             VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                                    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VkImage image;
    VK(vkCreateImage(device, &image_info, NULL, &image));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device, image, &req);
    VkDeviceMemory image_memory = allocate(req);
    VK(vkBindImageMemory(device, image, image_memory, 0));
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                .image = image,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                .format = VK_FORMAT_R8G8B8A8_UNORM,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkImageView view;
    VK(vkCreateImageView(device, &vi, NULL, &view));
    VkAttachmentDescription attachment = {.format = VK_FORMAT_R8G8B8A8_UNORM,
                                          .samples = VK_SAMPLE_COUNT_1_BIT,
                                          .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                                          .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                          .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                          .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                          .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                          .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
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
    VkRenderPassCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                 .attachmentCount = 1,
                                 .pAttachments = &attachment,
                                 .subpassCount = 1,
                                 .pSubpasses = &subpass,
                                 .dependencyCount = 1,
                                 .pDependencies = &dependency};
    VkRenderPass pass;
    VK(vkCreateRenderPass(device, &ri, NULL, &pass));
    VkFramebufferCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                  .renderPass = pass,
                                  .attachmentCount = 1,
                                  .pAttachments = &view,
                                  .width = WIDTH,
                                  .height = HEIGHT,
                                  .layers = 1};
    VkFramebuffer fb;
    VK(vkCreateFramebuffer(device, &fi, NULL, &fb));
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    VkPipelineLayout layout;
    VK(vkCreatePipelineLayout(device, &li, NULL, &layout));
    VkShaderModule vs = shader(triangle_vert, sizeof triangle_vert),
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
    VkViewport viewport = {0, 0, WIDTH, HEIGHT, 0, 1};
    VkRect2D scissor = {{0, 0}, {WIDTH, HEIGHT}};
    VkPipelineViewportStateCreateInfo viewport_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .pViewports = &viewport,
        .scissorCount = 1,
        .pScissors = &scissor};
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
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                       .stageCount = 2,
                                       .pStages = stages,
                                       .pVertexInputState = &vertex,
                                       .pInputAssemblyState = &assembly,
                                       .pViewportState = &viewport_state,
                                       .pRasterizationState = &raster,
                                       .pMultisampleState = &samples,
                                       .pColorBlendState = &blend_state,
                                       .layout = layout,
                                       .renderPass = pass,
                                       .basePipelineIndex = -1};
    VkPipeline pipeline;
    VK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pi, NULL, &pipeline));
    VkCommandPoolCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    VkCommandPool pool;
    VK(vkCreateCommandPool(device, &ci, NULL, &pool));
    VkCommandBufferAllocateInfo ca = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                      .commandPool = pool,
                                      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                      .commandBufferCount = 1};
    VkCommandBuffer command;
    VK(vkAllocateCommandBuffers(device, &ca, &command));
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    VK(vkBeginCommandBuffer(command, &bi));
    VkClearValue clear = {.color = {{0, 0, 0, 1}}};
    VkRenderPassBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                   .renderPass = pass,
                                   .framebuffer = fb,
                                   .renderArea = {{0, 0}, {WIDTH, HEIGHT}},
                                   .clearValueCount = 1,
                                   .pClearValues = &clear};
    vkCmdBeginRenderPass(command, &begin, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(command, 0, 1, &vb, &offset);
    vkCmdDraw(command, 3, 1, 0, 0);
    vkCmdEndRenderPass(command);
    VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                              .imageExtent = {WIDTH, HEIGHT, 1}};
    vkCmdCopyImageToBuffer(command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb, 1, &copy);
    VK(vkEndCommandBuffer(command));
    VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    VK(vkCreateFence(device, &fence_info, NULL, &fence));
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                           .commandBufferCount = 1,
                           .pCommandBuffers = &command};
    VK(vkQueueSubmit(queue, 1, &submit, fence));
    VK(vkWaitForFences(device, 1, &fence, VK_TRUE, 10000000000ull));
    uint8_t *center = (uint8_t *)pixels + (HEIGHT / 2 * WIDTH + WIDTH / 2) * 4;
    if (center[0] < 20 || center[1] < 20 || center[2] < 80 || center[3] != 255) {
        fprintf(stderr, "Triangle readback failed\n");
        return 1;
    }
    FILE *out = fopen(path, "wb");
    if (!out) {
        perror(path);
        return 1;
    }
    fprintf(out, "P6\n%d %d\n255\n", WIDTH, HEIGHT);
    int failed = 0;
    for (unsigned i = 0; i < WIDTH * HEIGHT; i++)
        if (fwrite((uint8_t *)pixels + i * 4, 1, 3, out) != 3)
            failed = 1;
    if (fclose(out))
        failed = 1;
    if (failed) {
        fprintf(stderr, "Could not write %s\n", path);
        return 1;
    }
    printf("Rendered SPIR-V triangle: %s\n", path);
    vkDestroyFence(device, fence, NULL);
    vkDestroyCommandPool(device, pool, NULL);
    vkDestroyPipeline(device, pipeline, NULL);
    vkDestroyShaderModule(device, vs, NULL);
    vkDestroyShaderModule(device, fs, NULL);
    vkDestroyPipelineLayout(device, layout, NULL);
    vkDestroyFramebuffer(device, fb, NULL);
    vkDestroyRenderPass(device, pass, NULL);
    vkDestroyImageView(device, view, NULL);
    vkDestroyImage(device, image, NULL);
    vkFreeMemory(device, image_memory, NULL);
    vkUnmapMemory(device, vb_memory);
    vkUnmapMemory(device, rb_memory);
    vkDestroyBuffer(device, vb, NULL);
    vkDestroyBuffer(device, rb, NULL);
    vkFreeMemory(device, vb_memory, NULL);
    vkFreeMemory(device, rb_memory, NULL);
    vkDestroyDevice(device, NULL);
#ifdef ZVK_LOADER
    destroy_debug(instance, messenger, NULL);
#endif
    vkDestroyInstance(instance, NULL);
#ifdef ZVK_LOADER
    printf("LOADER_RESULT validation=%d errors=%u\n", validation, validation_errors);
    return validation_errors ? 1 : 0;
#else
    return 0;
#endif
}
