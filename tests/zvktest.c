/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (C) 2026 Rigby Foundation */
#include <zvk.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include <unistd.h>
#include <spirv/unified1/spirv.h>
#include "shaders.h"

static unsigned passed, failed;
#ifdef ZVK_LOADER
static unsigned validation_errors;
#define zvkGetLastError() "Vulkan loader/driver error"
static VkDebugUtilsMessengerEXT debug_messenger;
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
#endif
#define EXPECT(test, label)                                                                        \
    do {                                                                                           \
        if (test) {                                                                                \
            passed++;                                                                              \
            printf("PASS %s\n", label);                                                            \
        } else {                                                                                   \
            failed++;                                                                              \
            printf("FAIL %s (%s:%d) %s\n", label, __FILE__, __LINE__, zvkGetLastError());          \
        }                                                                                          \
    } while (0)
#define VK(call)                                                                                   \
    do {                                                                                           \
        VkResult r_ = (call);                                                                      \
        if (r_ != VK_SUCCESS) {                                                                    \
            fprintf(stderr, "%s -> %d: %s\n", #call, r_, zvkGetLastError());                       \
            exit(2);                                                                               \
        }                                                                                          \
    } while (0)
static VkInstance instance;
static VkPhysicalDevice physical;
static VkDevice device;
static VkQueue queue;
static VkCommandPool pool;
static VkCommandBuffer command;
static VkFence fence;
static VkPhysicalDeviceProperties properties;
struct buffer {
    VkBuffer handle;
    VkDeviceMemory memory;
    void *map;
    VkDeviceSize size;
};
struct image {
    VkImage handle;
    VkDeviceMemory memory;
    VkImageView view;
};
static struct buffer buffer(VkDeviceSize size, VkBufferUsageFlags usage)
{
    struct buffer b = {.size = size};
    VkBufferCreateInfo info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                               .size = size,
                               .usage = usage,
                               .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VK(vkCreateBuffer(device, &info, NULL, &b.handle));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, b.handle, &req);
    VkMemoryAllocateInfo alloc = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                  .allocationSize = req.size,
                                  .memoryTypeIndex = 0};
    VK(vkAllocateMemory(device, &alloc, NULL, &b.memory));
    VK(vkBindBufferMemory(device, b.handle, b.memory, 0));
    VK(vkMapMemory(device, b.memory, 0, VK_WHOLE_SIZE, 0, &b.map));
    return b;
}
static void free_buffer(struct buffer b)
{
    vkUnmapMemory(device, b.memory);
    vkDestroyBuffer(device, b.handle, NULL);
    vkFreeMemory(device, b.memory, NULL);
}
static VkImageCreateFlags image_create_flags;
static struct image image_config(uint32_t width, uint32_t height, VkFormat format,
                                 VkImageUsageFlags usage, uint32_t levels, uint32_t layers,
                                 int cube, VkSampleCountFlagBits samples)
{
    struct image im = {0};
    VkImageCreateInfo info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                              .imageType = VK_IMAGE_TYPE_2D,
                              .format = format,
                              .extent = {width, height, 1},
                              .mipLevels = levels,
                              .arrayLayers = layers,
                              .flags = image_create_flags | (cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0),
                              .samples = samples,
                              .tiling = VK_IMAGE_TILING_OPTIMAL,
                              .usage = usage,
                              .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                              .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VK(vkCreateImage(device, &info, NULL, &im.handle));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device, im.handle, &req);
    VkMemoryAllocateInfo alloc = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                  .allocationSize = req.size,
                                  .memoryTypeIndex = 0};
    VK(vkAllocateMemory(device, &alloc, NULL, &im.memory));
    VK(vkBindImageMemory(device, im.handle, im.memory, 0));
    VkImageViewCreateInfo view = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = im.handle,
        .viewType = cube         ? VK_IMAGE_VIEW_TYPE_CUBE
                    : layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY
                                 : VK_IMAGE_VIEW_TYPE_2D,
        .format = format,
        .subresourceRange = {(format == VK_FORMAT_D16_UNORM || format == VK_FORMAT_D32_SFLOAT ||
                              format == VK_FORMAT_D24_UNORM_S8_UINT)
                                 ? VK_IMAGE_ASPECT_DEPTH_BIT
                                 : VK_IMAGE_ASPECT_COLOR_BIT,
                             0, levels, 0, layers}};
    if (usage & (VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_STORAGE_BIT|
                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|
                 VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT))
        VK(vkCreateImageView(device, &view, NULL, &im.view));
    return im;
}
static struct image image_levels(uint32_t width, uint32_t height, VkFormat format,
                                 VkImageUsageFlags usage, uint32_t levels, uint32_t layers,
                                 int cube)
{
    return image_config(width, height, format, usage, levels, layers, cube, VK_SAMPLE_COUNT_1_BIT);
}
static VkSampleCountFlagBits pipeline_samples = VK_SAMPLE_COUNT_1_BIT;
static uint32_t pipeline_subpass;
static const uint32_t *pipeline_sample_mask;
static int pipeline_alpha_coverage, pipeline_color_count = -1;
static const VkPipelineRasterizationStateCreateInfo *pipeline_raster;
static int pipeline_dynamic_raster;
static VkPipelineCache pipeline_cache;
static const VkPipelineVertexInputStateCreateInfo *pipeline_vertex;
static struct image image(uint32_t width, uint32_t height, VkFormat format, VkImageUsageFlags usage)
{
    return image_levels(width, height, format, usage, 1, 1, 0);
}
static void free_image(struct image im)
{
    vkDestroyImageView(device, im.view, NULL);
    vkDestroyImage(device, im.handle, NULL);
    vkFreeMemory(device, im.memory, NULL);
}
static void begin(void)
{
    VK(vkResetCommandBuffer(command, 0));
    VkCommandBufferBeginInfo info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    VK(vkBeginCommandBuffer(command, &info));
}
static void submit(void)
{
    VK(vkEndCommandBuffer(command));
    VK(vkResetFences(device, 1, &fence));
    VkSubmitInfo info = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                         .commandBufferCount = 1,
                         .pCommandBuffers = &command};
    VK(vkQueueSubmit(queue, 1, &info, fence));
    VK(vkWaitForFences(device, 1, &fence, VK_TRUE, 10000000000ull));
}
static void barrier_aspect(VkImage image, VkImageLayout old, VkImageLayout next,
                           VkImageAspectFlags aspect)
{
    VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                              .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                              .dstAccessMask =
                                  VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                              .oldLayout = old,
                              .newLayout = next,
                              .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                              .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                              .image = image,
                              .subresourceRange = {aspect, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}
static void barrier(VkImage image, VkImageLayout old, VkImageLayout next)
{
    barrier_aspect(image, old, next, VK_IMAGE_ASPECT_COLOR_BIT);
}
static void indirect_barrier(VkBuffer buffer)
{
    VkBufferMemoryBarrier b={.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_INDIRECT_COMMAND_READ_BIT,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .buffer=buffer,.size=VK_WHOLE_SIZE};
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
                         0,0,NULL,1,&b,0,NULL);
}
static VkShaderModule module(const uint32_t *code, size_t size)
{
    VkShaderModule m;
    VkShaderModuleCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = size, .pCode = code};
    VK(vkCreateShaderModule(device, &info, NULL, &m));
    return m;
}
static VkPipeline pipeline_shaders(const uint32_t *vertex_code, size_t vertex_size,
                                   const VkSpecializationInfo *specialization, VkRenderPass pass,
                                   VkPipelineLayout layout, const uint32_t *frag, size_t size,
                                   VkCullModeFlags cull, int depth, int blend)
{
    VkShaderModule vs = module(vertex_code, vertex_size), fs = module(frag, size);
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vs,
         .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = fs,
         .pName = "main",
         .pSpecializationInfo = specialization}};
    VkVertexInputBindingDescription binding = {0, 20, VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attrs[2] = {{0, 0, VK_FORMAT_R32G32_SFLOAT, 0},
                                                  {1, 0, VK_FORMAT_R32G32B32_SFLOAT, 8}};
    if (vertex_code == integer_vert) {
        attrs[0].format = VK_FORMAT_R32G32_SINT;
        attrs[1].format = VK_FORMAT_R32G32B32_UINT;
    }
    VkPipelineVertexInputStateCreateInfo vertex = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1,
        .pVertexBindingDescriptions = &binding,
        .vertexAttributeDescriptionCount = 2,
        .pVertexAttributeDescriptions = attrs};
    if (vertex_code == builtin_vert || vertex_code == ms_write_vert ||
        vertex_code == stencil_write_vert) {
        vertex.vertexBindingDescriptionCount = 0;
        vertex.vertexAttributeDescriptionCount = 0;
    }
    if (vertex_code == instanced_vert) {
        binding.stride = 12;
        binding.inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;
        attrs[0] = (VkVertexInputAttributeDescription){0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};
        vertex.vertexAttributeDescriptionCount = 1;
    }
    if (pipeline_vertex)
        vertex = *pipeline_vertex;
    VkPipelineInputAssemblyStateCreateInfo assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo viewport = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo raster = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = cull,
        .frontFace = VK_FRONT_FACE_CLOCKWISE,
        .lineWidth = 1.0};
    if (pipeline_raster)
        raster = *pipeline_raster;
    VkPipelineMultisampleStateCreateInfo samples = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = pipeline_samples,
        .pSampleMask = pipeline_sample_mask,
        .alphaToCoverageEnable = pipeline_alpha_coverage};
    VkPipelineDepthStencilStateCreateInfo ds = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = depth == 1,
        .depthWriteEnable = depth == 1,
        .depthCompareOp = VK_COMPARE_OP_LESS};
    if (depth >= 2) {
        ds.stencilTestEnable = 1;
        ds.front =
            (VkStencilOpState){.failOp = VK_STENCIL_OP_KEEP,
                               .passOp = depth == 2 ? VK_STENCIL_OP_REPLACE : VK_STENCIL_OP_KEEP,
                               .depthFailOp = VK_STENCIL_OP_KEEP,
                               .compareOp = depth == 2 ? VK_COMPARE_OP_ALWAYS : VK_COMPARE_OP_EQUAL,
                               .compareMask = 255,
                               .writeMask = 255,
                               .reference = 5};
        ds.back = ds.front;
    }
    VkPipelineColorBlendAttachmentState rt = {.blendEnable = blend,
                                              .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
                                              .dstColorBlendFactor =
                                                  VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
                                              .colorBlendOp = VK_BLEND_OP_ADD,
                                              .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
                                              .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
                                              .alphaBlendOp = VK_BLEND_OP_ADD,
                                              .colorWriteMask = 15};
    VkPipelineColorBlendAttachmentState rts[4] = {rt, rt, rt, rt};
    VkPipelineColorBlendStateCreateInfo bs = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = pipeline_color_count >= 0 ? (uint32_t)pipeline_color_count
                           : frag == mrt_frag        ? 2
                                                     : 1,
        .pAttachments = rts};
    VkDynamicState dynamics[7] = {VK_DYNAMIC_STATE_VIEWPORT,
                                  VK_DYNAMIC_STATE_SCISSOR,
                                  VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
                                  VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
                                  VK_DYNAMIC_STATE_STENCIL_REFERENCE,
                                  VK_DYNAMIC_STATE_LINE_WIDTH,
                                  VK_DYNAMIC_STATE_DEPTH_BIAS};
    VkPipelineDynamicStateCreateInfo dynamic = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = pipeline_dynamic_raster ? 7
                             : depth >= 2            ? 5
                                                     : 2,
        .pDynamicStates = dynamics};
    VkGraphicsPipelineCreateInfo info = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                         .stageCount = raster.rasterizerDiscardEnable ? 1 : 2,
                                         .pStages = stages,
                                         .pVertexInputState = &vertex,
                                         .pInputAssemblyState = &assembly,
                                         .pViewportState = &viewport,
                                         .pRasterizationState = &raster,
                                         .pMultisampleState = &samples,
                                         .pDepthStencilState = &ds,
                                         .pColorBlendState = &bs,
                                         .pDynamicState = &dynamic,
                                         .layout = layout,
                                         .renderPass = pass,
                                         .subpass = pipeline_subpass,
                                         .basePipelineIndex = -1};
    VkPipeline p;
    VK(vkCreateGraphicsPipelines(device, pipeline_cache, 1, &info, NULL, &p));
    vkDestroyShaderModule(device, vs, NULL);
    vkDestroyShaderModule(device, fs, NULL);
    return p;
}
static VkPipeline pipeline(VkRenderPass pass, VkPipelineLayout layout, const uint32_t *frag,
                           size_t size, VkCullModeFlags cull, int depth, int blend)
{
    return pipeline_shaders(triangle_vert, sizeof triangle_vert, NULL, pass, layout, frag, size,
                            cull, depth, blend);
}
static void begin_render(VkRenderPass pass, VkFramebuffer fb, VkPipeline p, struct buffer vertices,
                         uint32_t clear_count)
{
    VkClearValue clear[2] = {{{{0, 0, 0, 1}}}, {.depthStencil = {1.0, 0}}};
    VkRenderPassBeginInfo info = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                  .renderPass = pass,
                                  .framebuffer = fb,
                                  .renderArea = {{0, 0}, {32, 32}},
                                  .clearValueCount = clear_count,
                                  .pClearValues = clear};
    vkCmdBeginRenderPass(command, &info, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    VkViewport vp = {0, 0, 32, 32, 0, 1};
    VkRect2D sc = {{0, 0}, {32, 32}};
    vkCmdSetViewport(command, 0, 1, &vp);
    vkCmdSetScissor(command, 0, 1, &sc);
    VkDeviceSize off = 0;
    if (vertices.handle)
        vkCmdBindVertexBuffers(command, 0, 1, &vertices.handle, &off);
}
static void readback(VkImage image, struct buffer b)
{
    barrier(image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy region = {.bufferOffset = 16,
                                .bufferRowLength = 40,
                                .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                .imageExtent = {32, 32, 1}};
    vkCmdCopyImageToBuffer(command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b.handle, 1,
                           &region);
}
static int pixel(struct buffer b, unsigned x, unsigned y, int r, int g, int blue, int tolerance)
{
    uint8_t *p = (uint8_t *)b.map + 16 + (y * 40 + x) * 4;
    int good = abs(p[0] - r) <= tolerance && abs(p[1] - g) <= tolerance &&
               abs(p[2] - blue) <= tolerance && p[3] == 255;
    if (!good)
        printf("pixel %u,%u got %u %u %u %u, expected %d %d %d\n", x, y, p[0], p[1], p[2], p[3], r,
               g, blue);
    return good;
}
static void discovery(void)
{
    uint32_t version = 0;
    VK(vkEnumerateInstanceVersion(&version));
#ifdef ZVK_LOADER
    EXPECT(version >= VK_API_VERSION_1_0, "loader supports Vulkan 1.0");
#else
    EXPECT(version == VK_API_VERSION_1_0, "Vulkan API version");
#endif
    EXPECT(vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance") != NULL &&
               vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkQueueSubmit") == NULL,
           "global dispatch scope");
    const char *scoped_names[]={"vkGetImageSparseMemoryRequirements","vkGetPhysicalDeviceImageFormatProperties",
        "vkGetPhysicalDeviceMemoryProperties","vkGetPhysicalDeviceQueueFamilyProperties",
        "vkGetPhysicalDeviceSparseImageFormatProperties","vkGetPhysicalDeviceSurfaceCapabilitiesKHR"};
    int scoped=1;
    for(unsigned i=0;i<sizeof scoped_names/sizeof *scoped_names;i++)
        scoped&=vkGetInstanceProcAddr(VK_NULL_HANDLE,scoped_names[i])==NULL;
    EXPECT(scoped,"wrapped core declarations stay outside global dispatch");
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                             .pApplicationName = "zvktest",
                             .apiVersion = VK_API_VERSION_1_0};
    VkInstanceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                 .pApplicationInfo = &app};
    const char *extensions[] = {VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
#ifdef ZVK_LOADER
        VK_EXT_DEBUG_UTILS_EXTENSION_NAME, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME,
#endif
    };
    info.enabledExtensionCount=sizeof extensions/sizeof *extensions;
    info.ppEnabledExtensionNames=extensions;
#ifdef ZVK_LOADER
    const char *layer = "VK_LAYER_KHRONOS_validation";
    VkDebugUtilsMessengerCreateInfoEXT debug = {
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT,
        .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT,
        .pfnUserCallback = debug_message};
    VkValidationFeatureEnableEXT sync = VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
    VkValidationFeaturesEXT validation = {.sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT,
                                          .pNext = &debug,
                                          .enabledValidationFeatureCount = 1,
                                          .pEnabledValidationFeatures = &sync};
    info.pNext = &validation;
    info.enabledLayerCount = 1;
    info.ppEnabledLayerNames = &layer;
#endif
    VK(vkCreateInstance(&info, NULL, &instance));
#ifdef ZVK_LOADER
    PFN_vkCreateDebugUtilsMessengerEXT create_debug =
        (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance,
                                                                  "vkCreateDebugUtilsMessengerEXT");
    VK(create_debug(instance, &debug, NULL, &debug_messenger));
#endif
    uint32_t count = 0;
    VK(vkEnumeratePhysicalDevices(instance, &count, NULL));
    if (!count) {
        fprintf(stderr, "No virgl Vulkan graphics device.\n");
        exit(2);
    }
    EXPECT(count == 1, "one physical graphics device");
    count = 0;
    EXPECT(vkEnumeratePhysicalDevices(instance, &count, &physical) == VK_INCOMPLETE,
           "enumeration truncation");
    count = 1;
    VK(vkEnumeratePhysicalDevices(instance, &count, &physical));
    vkGetPhysicalDeviceProperties(physical, &properties);
    printf("DEVICE %s\n", properties.deviceName);
    VkPhysicalDeviceFeatures features;
    vkGetPhysicalDeviceFeatures(physical, &features);
    EXPECT(!features.geometryShader && !features.tessellationShader && !features.shaderFloat64,
           "unsupported features stay disabled");
    uint32_t families = 1;
    VkQueueFamilyProperties q;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &families, &q);
    EXPECT((q.queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
               !!(q.queueFlags & VK_QUEUE_COMPUTE_BIT) ==
                   !!properties.limits.maxComputeWorkGroupInvocations,
           "graphics and transfer queue profile");
    float priority = 1;
    VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                  .queueCount = 1,
                                  .pQueuePriorities = &priority};
    VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                             .queueCreateInfoCount = 1,
                             .pQueueCreateInfos = &qi};
    VkDevice plain_device;VK(vkCreateDevice(physical,&di,NULL,&plain_device));
    EXPECT(vkGetDeviceProcAddr(plain_device,"vkResetQueryPoolEXT")==NULL,
           "host query reset device dispatch requires its extension");
    vkDestroyDevice(plain_device,NULL);
    VkPhysicalDeviceHostQueryResetFeatures host_reset={
        .sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES};
    VkPhysicalDeviceFeatures2 feature_query={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext=&host_reset};
    PFN_vkGetPhysicalDeviceFeatures2KHR get_features2=(PFN_vkGetPhysicalDeviceFeatures2KHR)
        vkGetInstanceProcAddr(instance,"vkGetPhysicalDeviceFeatures2KHR");
    get_features2(physical,&feature_query);
    EXPECT(host_reset.hostQueryReset,"host query reset feature is reported through the extensible query");
    const char *device_extension=VK_EXT_HOST_QUERY_RESET_EXTENSION_NAME;
    di.enabledExtensionCount=1;di.ppEnabledExtensionNames=&device_extension;di.pNext=&host_reset;
    VkPhysicalDeviceFeatures enabled = {.multiDrawIndirect = VK_TRUE,
                                        .drawIndirectFirstInstance = VK_TRUE};
    enabled.robustBufferAccess = features.robustBufferAccess;
    enabled.textureCompressionBC = features.textureCompressionBC;
    enabled.shaderStorageImageExtendedFormats = features.shaderStorageImageExtendedFormats;
    di.pEnabledFeatures = &enabled;
    VK(vkCreateDevice(physical, &di, NULL, &device));
    vkGetDeviceQueue(device, 0, 0, &queue);
    EXPECT(vkGetDeviceProcAddr(device, "vkCmdDraw") != NULL &&
#ifndef ZVK_LOADER
               vkGetDeviceProcAddr(device, "vkCreateInstance") == NULL &&
#endif
               vkGetDeviceProcAddr(device, "vkThisDoesNotExist") == NULL,
           "device dispatch scope");
    EXPECT(vkGetDeviceProcAddr(device,"vkGetImageSparseMemoryRequirements") &&
        vkGetInstanceProcAddr(instance,"vkGetPhysicalDeviceImageFormatProperties") &&
#ifndef ZVK_LOADER
        !vkGetInstanceProcAddr(instance,"vkGetPhysicalDeviceSurfaceCapabilitiesKHR") &&
#endif
        !vkGetDeviceProcAddr(device,"vkGetPhysicalDeviceMemoryProperties"),
        "wrapped declarations retain device and instance extension scopes");
    VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                  .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
    VK(vkCreateCommandPool(device, &pi, NULL, &pool));
    VkCommandBufferAllocateInfo ca = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                      .commandPool = pool,
                                      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                      .commandBufferCount = 1};
    VK(vkAllocateCommandBuffers(device, &ca, &command));
    VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VK(vkCreateFence(device, &fi, NULL, &fence));
    EXPECT(vkGetFenceStatus(device, fence) == VK_NOT_READY &&
               vkWaitForFences(device, 1, &fence, VK_TRUE, 0) == VK_TIMEOUT,
           "unsignaled fence status and timeout");
}
static void transfers(void)
{
    struct buffer a = buffer(64,
                             VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT),
                  b = buffer(64, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    EXPECT(properties.limits.minMemoryMapAlignment >= 64 &&
               (uintptr_t)a.map % properties.limits.minMemoryMapAlignment == 0,
           "mapped backing meets the mandatory 64-byte alignment");
    memset(a.map, 0, 64);
    memset(b.map, 0, 64);
    uint32_t data[3] = {0x11223344, 0x55667788, 0xaabbccdd};
    begin();
    vkCmdFillBuffer(command, a.handle, 0, 64, 0x12345678);
    vkCmdUpdateBuffer(command, a.handle, 8, sizeof data, data);
    memset(data, 0, sizeof data);
    VkBufferCopy region = {4, 12, 24};
    vkCmdCopyBuffer(command, a.handle, b.handle, 1, &region);
    region.size = 0;
    EXPECT(*(uint32_t *)a.map == 0 && *(uint32_t *)b.map == 0, "recording defers execution");
    submit();
    uint32_t *out = b.map;
    EXPECT(out[3] == 0x12345678 && out[4] == 0x11223344 && out[5] == 0x55667788 &&
               out[6] == 0xaabbccdd && out[8] == 0x12345678,
           "buffer fill, copied update data and transfer regions");
    EXPECT(vkGetFenceStatus(device, fence) == VK_SUCCESS, "submission signals fence");
    memset(b.map, 0, 64);
    VkSubmitInfo info = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                         .commandBufferCount = 1,
                         .pCommandBuffers = &command};
    VK(vkQueueSubmit(queue, 1, &info, VK_NULL_HANDLE));
    VK(vkQueueWaitIdle(queue));
    EXPECT(out[4] == 0x11223344, "command buffer resubmission");
    void *ptr = NULL;
    EXPECT(vkMapMemory(device, a.memory, UINT64_MAX, 32, 0, &ptr) != VK_SUCCESS,
           "memory offset overflow rejected");
    begin();
    vkCmdDispatch(command, 65536, 1, 1);
    EXPECT(vkEndCommandBuffer(command) == VK_ERROR_FEATURE_NOT_PRESENT,
           "unsupported compute command fails explicitly");
    free_buffer(a);
    free_buffer(b);
}
static void graphics(void)
{
    const float verts[] = {-.8f, -.8f, 1, 0, 0, .8f, -.8f, 0, 1, 0, 0, .8f, 0, 0, 1};
    struct buffer vb = buffer(sizeof verts, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    memcpy(vb.map, verts, sizeof verts);
    struct buffer rb = buffer(16 + 40 * 32 * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    memset(rb.map, 0xa5, (size_t)rb.size);
    struct image target =
        image(32, 32, VK_FORMAT_R8G8B8A8_UNORM,
              VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                  VK_IMAGE_USAGE_TRANSFER_DST_BIT);
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
    VkRenderPassCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                 .attachmentCount = 1,
                                 .pAttachments = &attachment,
                                 .subpassCount = 1,
                                 .pSubpasses = &subpass};
    VkRenderPass pass;
    VK(vkCreateRenderPass(device, &ri, NULL, &pass));
    VkFramebufferCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                  .renderPass = pass,
                                  .attachmentCount = 1,
                                  .pAttachments = &target.view,
                                  .width = 32,
                                  .height = 32,
                                  .layers = 1};
    VkFramebuffer fb;
    VK(vkCreateFramebuffer(device, &fi, NULL, &fb));
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    VkPipelineLayout layout;
    VK(vkCreatePipelineLayout(device, &li, NULL, &layout));
    VkPipeline tri = pipeline(pass, layout, triangle_frag, sizeof triangle_frag, 0, 0, 0);
    begin();
    begin_render(pass, fb, tri, vb, 1);
    vkCmdDraw(command, 3, 1, 0, 0);
    vkCmdEndRenderPass(command);
    readback(target.handle, rb);
    EXPECT(((uint8_t *)rb.map)[16] == 0xa5, "draw and readback remain deferred");
    submit();
    EXPECT(pixel(rb, 0, 0, 0, 0, 0, 0), "render-pass clear");
    EXPECT(pixel(rb, 16, 16, 56, 66, 133, 5), "SPIR-V interpolated triangle");
    EXPECT(((uint8_t *)rb.map)[0] == 0xa5 && ((uint8_t *)rb.map)[16 + 32 * 4] == 0xa5,
           "readback offsets and row padding preserved");
    uint16_t indices[] = {99, 0, 1, 2};
    struct buffer ib = buffer(sizeof indices, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    memcpy(ib.map, indices, sizeof indices);
    begin();
    begin_render(pass, fb, tri, vb, 1);
    vkCmdBindIndexBuffer(command, ib.handle, 0, VK_INDEX_TYPE_UINT16);
    vkCmdDrawIndexed(command, 3, 1, 1, 0, 0);
    vkCmdEndRenderPass(command);
    readback(target.handle, rb);
    submit();
    EXPECT(pixel(rb, 16, 16, 56, 66, 133, 5), "indexed drawing with firstIndex");
    VkVertexInputBindingDescription sparse_binding = {0, 20, VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription sparse_attributes[] = {
        {4, 0, VK_FORMAT_R32G32_SFLOAT, 0}, {15, 0, VK_FORMAT_R32G32B32_SFLOAT, 8}};
    VkPipelineVertexInputStateCreateInfo sparse_vertex = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &sparse_binding,
        .vertexAttributeDescriptionCount = 2, .pVertexAttributeDescriptions = sparse_attributes};
    pipeline_vertex = &sparse_vertex;
    VkPipeline sparse = pipeline_shaders(sparse_attributes_vert, sizeof sparse_attributes_vert, NULL,
                                      pass, layout, triangle_frag, sizeof triangle_frag, 0, 0, 0);
    pipeline_vertex = NULL;
    begin();
    begin_render(pass, fb, sparse, vb, 1);
    vkCmdBindIndexBuffer(command, ib.handle, 0, VK_INDEX_TYPE_UINT16);
    vkCmdDrawIndexed(command, 3, 1, 1, 0, 0);
    vkCmdEndRenderPass(command);
    readback(target.handle, rb);
    submit();
    EXPECT(pixel(rb, 16, 16, 56, 66, 133, 5),
           "sparse vertex locations 4 and 15 map to dense backend inputs without location zero");
    vkDestroyPipeline(device, sparse, NULL);
    struct buffer constant_color=buffer(32,VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    const VkFormat constant_formats[]={VK_FORMAT_R32G32B32_SFLOAT,VK_FORMAT_R16G16B16A16_SFLOAT,
                                        VK_FORMAT_R8G8B8A8_UNORM,VK_FORMAT_R16G16B16A16_SNORM};
    const float constant_float[]={.25f,.5f,.75f};
    const uint16_t constant_half[]={0x3400,0x3800,0x3a00,0x3c00};
    const uint8_t constant_unorm[]={64,128,191,255};
    const int16_t constant_snorm[]={8192,16384,24575,32767};
    const void *constant_data[]={constant_float,constant_half,constant_unorm,constant_snorm};
    const size_t constant_sizes[]={sizeof constant_float,sizeof constant_half,sizeof constant_unorm,sizeof constant_snorm};
    for (unsigned i=0;i<4;i++) {
        memset(constant_color.map,0xa5,32);
        memcpy((uint8_t *)constant_color.map+8,constant_data[i],constant_sizes[i]);
        VkVertexInputBindingDescription bindings[]={{0,20,VK_VERTEX_INPUT_RATE_VERTEX},
                                                     {1,0,VK_VERTEX_INPUT_RATE_VERTEX}};
        VkVertexInputAttributeDescription attrs[]={{4,0,VK_FORMAT_R32G32_SFLOAT,0},
                                                     {15,1,constant_formats[i],8}};
        sparse_vertex.vertexBindingDescriptionCount=2;sparse_vertex.pVertexBindingDescriptions=bindings;
        sparse_vertex.pVertexAttributeDescriptions=attrs;
        pipeline_vertex=&sparse_vertex;
        VkPipeline cp=pipeline_shaders(sparse_attributes_vert,sizeof sparse_attributes_vert,NULL,
                                       pass,layout,triangle_frag,sizeof triangle_frag,0,0,0);
        pipeline_vertex=NULL;
        begin();begin_render(pass,fb,cp,vb,1);
        VkBuffer buffers[]={vb.handle,constant_color.handle};VkDeviceSize offsets[]={0,0};
        vkCmdBindVertexBuffers(command,0,2,buffers,offsets);
        vkCmdDraw(command,3,1,0,0);vkCmdEndRenderPass(command);
        readback(target.handle,rb);submit();
        EXPECT(pixel(rb,16,16,64,128,191,2),
               "constant vertex fetch converts float/half/normalized data at a nonzero attribute offset");
        vkDestroyPipeline(device,cp,NULL);
    }
    free_buffer(constant_color);
    VkPushConstantRange range = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
    li.pushConstantRangeCount = 1;
    li.pPushConstantRanges = &range;
    VkPipelineLayout push_layout;
    VK(vkCreatePipelineLayout(device, &li, NULL, &push_layout));
    VkPipeline push = pipeline(pass, push_layout, push_frag, sizeof push_frag, 0, 0, 0);
    float values[4] = {.25f, .5f, .75f, 1};
    begin();
    begin_render(pass, fb, push, vb, 1);
    vkCmdPushConstants(command, push_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, values);
    memset(values, 0, sizeof values);
    vkCmdDraw(command, 3, 1, 0, 0);
    vkCmdEndRenderPass(command);
    readback(target.handle, rb);
    submit();
    EXPECT(pixel(rb, 16, 16, 64, 128, 191, 1), "push constants are copied during recording");
    VkPipeline intrinsics = pipeline(pass, push_layout, intrinsics_frag, sizeof intrinsics_frag, 0, 0, 0);
    float intrinsic_values[4] = {.5f, .5f, 2.0f, 2.5f};
    begin();
    begin_render(pass, fb, intrinsics, vb, 1);
    vkCmdPushConstants(command, push_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, intrinsic_values);
    vkCmdDraw(command, 3, 1, 0, 0);
    vkCmdEndRenderPass(command);
    readback(target.handle, rb);
    submit();
    uint8_t *intrinsic_pixel = (uint8_t *)rb.map + 16 + (16 * 40 + 16) * 4;
    EXPECT(abs(intrinsic_pixel[0] - 43) <= 1 && abs(intrinsic_pixel[1] - 64) <= 1 &&
               abs(intrinsic_pixel[2] - 128) <= 1 && abs(intrinsic_pixel[3] - 128) <= 1,
           "native GPU asin, pow, matrix inverse/determinant and modf structure results");
    vkDestroyPipeline(device, intrinsics, NULL);
    VkPipeline flow = pipeline(pass, push_layout, flow_frag, sizeof flow_frag, 0, 0, 0);
    float flow_values[4] = {.5f, .5f, 1, 1};
    begin();
    begin_render(pass, fb, flow, vb, 1);
    vkCmdPushConstants(command, push_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, flow_values);
    vkCmdDraw(command, 3, 1, 0, 0);
    vkCmdEndRenderPass(command);
    readback(target.handle, rb);
    submit();
    EXPECT(pixel(rb, 16, 16, 64, 128, 128, 1), "SPIR-V loops, dynamic indexing and function calls");
    VkPipeline structured=pipeline(pass,push_layout,structured_frag,sizeof structured_frag,0,0,0);
    begin();begin_render(pass,fb,structured,vb,1);
    vkCmdDraw(command,3,1,0,0);vkCmdEndRenderPass(command);
    readback(target.handle,rb);submit();
    int reconverged=1;
    for (unsigned x=14;x<18;x++)
        reconverged &= pixel(rb,x,16,64,128,191,1);
    EXPECT(reconverged,"structured loops, switch fallthrough, early returns and private arrays reconverge before derivatives");
    vkDestroyPipeline(device,structured,NULL);
    VkPipeline selection=pipeline(pass,push_layout,selection_exit_frag_spvasm,sizeof selection_exit_frag_spvasm,0,0,0);
    begin();begin_render(pass,fb,selection,vb,1);
    vkCmdDraw(command,3,1,0,0);vkCmdEndRenderPass(command);
    readback(target.handle,rb);submit();
    int escaped=1;
    for (unsigned x=14;x<18;x++)escaped &= pixel(rb,x,16,64,128,191,1);
    EXPECT(escaped,"outer selection exits skip inner stores and preserve edge phi values before reconvergence");
    vkDestroyPipeline(device,selection,NULL);
    VkPipeline zero_mask=pipeline(pass,push_layout,sample_mask_zero_frag,sizeof sample_mask_zero_frag,0,0,0);
    begin();begin_render(pass,fb,zero_mask,vb,1);
    vkCmdDraw(command,3,1,0,0);vkCmdEndRenderPass(command);
    readback(target.handle,rb);submit();
    EXPECT(pixel(rb,16,16,0,0,0,0),"shader sample-mask output suppresses single-sample color writes");
    vkDestroyPipeline(device,zero_mask,NULL);
    VkDescriptorSetLayoutBinding binding = {.binding = 0,
                                            .descriptorType =
                                                VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
                                            .descriptorCount = 1,
                                            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT};
    VkDescriptorSetLayoutCreateInfo si = {.sType =
                                              VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                          .bindingCount = 1,
                                          .pBindings = &binding};
    VkDescriptorSetLayout set_layout;
    VK(vkCreateDescriptorSetLayout(device, &si, NULL, &set_layout));
    li.pushConstantRangeCount = 0;
    li.pPushConstantRanges = NULL;
    li.setLayoutCount = 1;
    li.pSetLayouts = &set_layout;
    VkPipelineLayout uniform_layout;
    VK(vkCreatePipelineLayout(device, &li, NULL, &uniform_layout));
    VkPipeline uni = pipeline(pass, uniform_layout, uniform_frag, sizeof uniform_frag, 0, 0, 0);
    VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1};
    VkDescriptorPoolCreateInfo dpi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                      .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
                                      .maxSets = 1,
                                      .poolSizeCount = 1,
                                      .pPoolSizes = &ps};
    VkDescriptorPool dp;
    VK(vkCreateDescriptorPool(device, &dpi, NULL, &dp));
    VkDescriptorSetAllocateInfo dai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                       .descriptorPool = dp,
                                       .descriptorSetCount = 1,
                                       .pSetLayouts = &set_layout};
    VkDescriptorSet ds;
    VK(vkAllocateDescriptorSets(device, &dai, &ds));
    uint32_t dynamic = (uint32_t)properties.limits.minUniformBufferOffsetAlignment;
    if (!dynamic)
        dynamic = 16;
    struct buffer ub = buffer(dynamic + 128, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    float uniform_values[4] = {1, .25f, .5f, 1};
    memcpy((uint8_t *)ub.map + dynamic + 32, uniform_values, 16);
    VkDescriptorBufferInfo dbi = {ub.handle, 0, 48};
    VkWriteDescriptorSet write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                  .dstSet = ds,
                                  .dstBinding = 0,
                                  .descriptorCount = 1,
                                  .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
                                  .pBufferInfo = &dbi};
    vkUpdateDescriptorSets(device, 1, &write, 0, NULL);
    begin();
    begin_render(pass, fb, uni, vb, 1);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, uniform_layout, 0, 1, &ds, 1,
                            &dynamic);
    vkCmdDraw(command, 3, 1, 0, 0);
    vkCmdEndRenderPass(command);
    readback(target.handle, rb);
    submit();
    EXPECT(pixel(rb, 16, 16, 255, 64, 128, 1), "dynamic UBO offset and SPIR-V member Offset");
    struct buffer bits=buffer(dynamic+1024,VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    uint32_t *words=(uint32_t *)((uint8_t *)bits.map+dynamic);
    uint32_t seed=0x12345678u;
    for (unsigned i=0;i<256;i++)words[i]=(seed=seed*1664525u+1013904223u);
    const uint32_t specials[]={0x7f800001u,0x7fc00000u,1u,0x80000001u,
        0x00800000u,0xff800000u,0xffffffffu,0x80000000u};
    memcpy(words,specials,sizeof specials);
    VkDescriptorBufferInfo bits_info={bits.handle,0,1024};
    write.pBufferInfo=&bits_info;vkUpdateDescriptorSets(device,1,&write,0,NULL);
    VkPipeline bits_pipeline=pipeline(pass,uniform_layout,uniform_bits_frag,sizeof uniform_bits_frag,0,0,0);
    for (unsigned iteration=0;iteration<2;iteration++) {
        begin();begin_render(pass,fb,bits_pipeline,vb,1);
        vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,uniform_layout,0,1,&ds,1,&dynamic);
        vkCmdDraw(command,3,1,0,0);vkCmdEndRenderPass(command);
        readback(target.handle,rb);submit();
        int exact=1;
        for (unsigned x=14;x<18;x++) {
            uint32_t value=(16*32+x)^0x9e3779b9u;
            for (unsigned i=0;i<4;i++)value=words[4*(value%64)+(value&3)];
            exact &= pixel(rb,x,16,value&255,(value>>8)&255,(value>>16)&255,0);
        }
        EXPECT(exact,"dynamic uniform indexing preserves arbitrary integer bits and observes host rewrites");
        for (unsigned i=0;i<256;i++)words[i]^=0xdeadbeefu;
    }
    vkDestroyPipeline(device,bits_pipeline,NULL);free_buffer(bits);
    write.pBufferInfo=&dbi;
#ifndef ZVK_LOADER
    /* Vulkan 1.0's pool capacity precondition makes this an implementation
     * error-path test; exclude it when checking valid inputs with validation. */
    VkDescriptorSet extra = VK_NULL_HANDLE;
    EXPECT(vkAllocateDescriptorSets(device, &dai, &extra) == VK_ERROR_OUT_OF_POOL_MEMORY,
           "descriptor pool capacity enforced");
#endif
    VkPipeline matrix = pipeline(pass, uniform_layout, matrix_frag, sizeof matrix_frag, 0, 0, 0);
    float matrix_data[20] = {1, 0, 0, 0, 0, .5f, 0, 0, 0, 0, .25f, 0, 0, 0, 0, 0, .25f, .5f, 1, 1};
    memcpy((uint8_t *)ub.map + dynamic, matrix_data, sizeof matrix_data);
    dbi.range = 80;
    vkUpdateDescriptorSets(device, 1, &write, 0, NULL);
    begin();
    begin_render(pass, fb, matrix, vb, 1);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, uniform_layout, 0, 1, &ds, 1,
                            &dynamic);
    vkCmdDraw(command, 3, 1, 0, 0);
    vkCmdEndRenderPass(command);
    readback(target.handle, rb);
    submit();
    EXPECT(pixel(rb, 16, 16, 64, 64, 64, 1), "row-major matrix stride and std140 array stride");
    vkDestroyPipeline(device, matrix, NULL);
    vkDestroyPipeline(device, uni, NULL);
    vkDestroyPipelineLayout(device, uniform_layout, NULL);
    vkDestroyDescriptorPool(device, dp, NULL);
    vkDestroyDescriptorSetLayout(device, set_layout, NULL);
    free_buffer(ub);
    VkPipeline culled =
        pipeline(pass, push_layout, push_frag, sizeof push_frag, VK_CULL_MODE_BACK_BIT, 0, 0);
    float red[4] = {1, 0, 0, 1};
    begin();
    begin_render(pass, fb, culled, vb, 1);
    vkCmdPushConstants(command, push_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, red);
    vkCmdDraw(command, 3, 1, 0, 0);
    vkCmdEndRenderPass(command);
    readback(target.handle, rb);
    submit();
    EXPECT(pixel(rb, 16, 16, 255, 0, 0, 0), "Vulkan clockwise front face and back-face culling");
    vkDestroyPipeline(device, culled, NULL);
    begin();
    begin_render(pass, fb, push, vb, 1);
    VkRect2D left = {{0, 0}, {16, 32}};
    vkCmdSetScissor(command, 0, 1, &left);
    vkCmdPushConstants(command, push_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, red);
    vkCmdDraw(command, 3, 1, 0, 0);
    vkCmdEndRenderPass(command);
    readback(target.handle, rb);
    submit();
    EXPECT(pixel(rb, 15, 15, 255, 0, 0, 0) && pixel(rb, 16, 15, 0, 0, 0, 0),
           "dynamic scissor boundary");
    begin();
    begin_render(pass, fb, push, vb, 1);
    VkViewport negative = {-16, 0, 32, 32, 0, 1};
    vkCmdSetViewport(command, 0, 1, &negative);
    vkCmdPushConstants(command, push_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, red);
    vkCmdDraw(command, 3, 1, 0, 0);
    vkCmdEndRenderPass(command);
    readback(target.handle, rb);
    submit();
    EXPECT(pixel(rb, 0, 16, 255, 0, 0, 0) && pixel(rb, 16, 16, 0, 0, 0, 0),
           "negative viewport origin clips correctly to the framebuffer");
    VkPipeline blended = pipeline(pass, push_layout, push_frag, sizeof push_frag, 0, 0, 1);
    float blue[4] = {0, 0, 1, 1}, half_red[4] = {1, 0, 0, .5f};
    begin();
    begin_render(pass, fb, blended, vb, 1);
    vkCmdPushConstants(command, push_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, blue);
    vkCmdDraw(command, 3, 1, 0, 0);
    vkCmdPushConstants(command, push_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, half_red);
    vkCmdDraw(command, 3, 1, 0, 0);
    vkCmdEndRenderPass(command);
    readback(target.handle, rb);
    submit();
    uint8_t *blend_pixel = (uint8_t *)rb.map + 16 + (16 * 40 + 16) * 4;
    EXPECT(abs(blend_pixel[0] - 128) <= 1 && blend_pixel[1] == 0 &&
               abs(blend_pixel[2] - 127) <= 1 && abs(blend_pixel[3] - 128) <= 1,
           "source-alpha blending");
    vkDestroyPipeline(device, blended, NULL);
    /* A combined sampler is independent of the OpenGL frontend. */
    struct image texture = image(2, 2, VK_FORMAT_R8G8B8A8_UNORM,
                                 VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    struct buffer upload = buffer(16, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    uint32_t texels[4] = {0xffcc6633u, 0xff00ff00u, 0xffff0000u, 0xffffffffu};
    memcpy(upload.map, texels, 16);
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    VK(vkCreateDescriptorSetLayout(device, &si, NULL, &set_layout));
    li.pSetLayouts = &set_layout;
    VkPipelineLayout texture_layout;
    VK(vkCreatePipelineLayout(device, &li, NULL, &texture_layout));
    VkPipeline textured =
        pipeline(pass, texture_layout, texture_frag, sizeof texture_frag, 0, 0, 0);
    ps.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    VK(vkCreateDescriptorPool(device, &dpi, NULL, &dp));
    dai.descriptorPool = dp;
    dai.pSetLayouts = &set_layout;
    VK(vkAllocateDescriptorSets(device, &dai, &ds));
    VkSamplerCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                               .magFilter = VK_FILTER_NEAREST,
                               .minFilter = VK_FILTER_NEAREST,
                               .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                               .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                               .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                               .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                               .maxLod = 0};
    VkSampler sampler;
    VK(vkCreateSampler(device, &sci, NULL, &sampler));
    VkDescriptorImageInfo image_info = {sampler, texture.view,
                                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    write.dstSet = ds;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pBufferInfo = NULL;
    write.pImageInfo = &image_info;
    vkUpdateDescriptorSets(device, 1, &write, 0, NULL);
    begin();
    barrier(texture.handle, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy texture_region = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                        .imageExtent = {2, 2, 1}};
    vkCmdCopyBufferToImage(command, upload.handle, texture.handle,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &texture_region);
    barrier(texture.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    begin_render(pass, fb, textured, vb, 1);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, texture_layout, 0, 1, &ds, 0,
                            NULL);
    vkCmdDraw(command, 3, 1, 0, 0);
    vkCmdEndRenderPass(command);
    readback(target.handle, rb);
    submit();
    EXPECT(pixel(rb, 16, 16, 51, 102, 204, 1),
           "combined image sampler and buffer-to-image transfer");
    /* Partial upload after rendering must retain pixels outside the copy. */
    begin();
    barrier(target.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy one_pixel = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                   .imageOffset = {0, 0, 0},
                                   .imageExtent = {1, 1, 1}};
    vkCmdCopyBufferToImage(command, upload.handle, target.handle,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &one_pixel);
    barrier(target.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    readback(target.handle, rb);
    submit();
    EXPECT(pixel(rb, 0, 0, 51, 102, 204, 0) && pixel(rb, 16, 16, 51, 102, 204, 1),
           "partial image upload preserves rendered pixels");
    struct image copy = image(32, 32, VK_FORMAT_R8G8B8A8_UNORM,
                              VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    begin();
    barrier(copy.handle, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearColorValue green = {{0, 1, 0, 1}};
    VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(command, copy.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &green, 1,
                         &all);
    barrier(copy.handle,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy image_region = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                .srcOffset = {8, 8, 0},
                                .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                .dstOffset = {8, 8, 0},
                                .extent = {16, 16, 1}};
    vkCmdCopyImage(command, target.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, copy.handle,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &image_region);
    barrier(copy.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    readback(copy.handle, rb);
    submit();
    EXPECT(pixel(rb, 0, 0, 0, 255, 0, 0) && pixel(rb, 16, 16, 51, 102, 204, 1),
           "image clear and image-to-image subrectangle copy");
    free_image(copy);
    vkDestroyPipeline(device, textured, NULL);
    vkDestroyPipelineLayout(device, texture_layout, NULL);
    vkDestroyDescriptorPool(device, dp, NULL);
    vkDestroyDescriptorSetLayout(device, set_layout, NULL);
    vkDestroySampler(device, sampler, NULL);
    free_image(texture);
    free_buffer(upload);
    /* Two fragments at equal depth: LESS must retain the first draw. */
    struct image depth_image =
        image(32, 32, VK_FORMAT_D32_SFLOAT,
              VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    VkAttachmentDescription attachments[2] = {
        attachment,
        {.format = VK_FORMAT_D32_SFLOAT,
         .samples = VK_SAMPLE_COUNT_1_BIT,
         .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
         .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
         .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
         .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
         .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
         .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL}};
    VkAttachmentReference depth_ref = {1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    subpass.pDepthStencilAttachment = &depth_ref;
    ri.attachmentCount = 2;
    ri.pAttachments = attachments;
    VkRenderPass depth_pass;
    VK(vkCreateRenderPass(device, &ri, NULL, &depth_pass));
    VkImageView depth_views[2] = {target.view, depth_image.view};
    fi.renderPass = depth_pass;
    fi.attachmentCount = 2;
    fi.pAttachments = depth_views;
    VkFramebuffer depth_fb;
    VK(vkCreateFramebuffer(device, &fi, NULL, &depth_fb));
    VkPipeline depth_pipeline =
        pipeline(depth_pass, push_layout, push_frag, sizeof push_frag, 0, 1, 0);
    begin();
    begin_render(depth_pass, depth_fb, depth_pipeline, vb, 2);
    vkCmdPushConstants(command, push_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, red);
    vkCmdDraw(command, 3, 1, 0, 0);
    vkCmdPushConstants(command, push_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, blue);
    vkCmdDraw(command, 3, 1, 0, 0);
    vkCmdEndRenderPass(command);
    readback(target.handle, rb);
    VkBufferImageCopy depth_copy = {.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1},
                                    .imageExtent = {32, 32, 1}};
    struct buffer depth_rb = buffer(4096, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    barrier_aspect(depth_image.handle,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_ASPECT_DEPTH_BIT);
    vkCmdCopyImageToBuffer(command, depth_image.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           depth_rb.handle, 1, &depth_copy);
    submit();
    EXPECT(pixel(rb, 16, 16, 255, 0, 0, 0), "depth write and LESS comparison");
    EXPECT(fabsf(((float *)depth_rb.map)[16 * 32 + 16] - .5f) < .00001f &&
               ((float *)depth_rb.map)[0] == 1.0f,
           "Vulkan zero-to-one depth conversion and depth readback");
    const uint32_t *depth_shaders[2]={early_keep_depth_frag,output_initializer_frag_spvasm};
    const size_t depth_sizes[2]={sizeof early_keep_depth_frag,sizeof output_initializer_frag_spvasm};
    for(unsigned test=0;test<2;test++) {
        VkPipeline special=pipeline(depth_pass,push_layout,depth_shaders[test],depth_sizes[test],0,1,0);
        begin();begin_render(depth_pass,depth_fb,special,vb,2);vkCmdDraw(command,3,1,0,0);
        vkCmdEndRenderPass(command);readback(target.handle,rb);
        barrier_aspect(depth_image.handle,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_ASPECT_DEPTH_BIT);
        vkCmdCopyImageToBuffer(command,depth_image.handle,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,depth_rb.handle,1,&depth_copy);submit();
        const float *values=depth_rb.map;
        EXPECT(test ? pixel(rb,16,16,64,128,191,1) && values[16*32+16]==.25f &&
                      pixel(rb,12,16,0,0,0,0) && values[16*32+12]==1 :
                      pixel(rb,16,16,0,255,0,0) && fabsf(values[16*32+16]-.5f)<.00001f,
               test ? "color/depth output initializers survive while discarded invocations leave depth unchanged" :
                      "early fragment tests retain rasterized depth and ignore the shader's FragDepth write");
        vkDestroyPipeline(device,special,NULL);
    }
    VkPipelineRasterizationStateCreateInfo biased_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .frontFace = VK_FRONT_FACE_CLOCKWISE,
        .lineWidth = 1,
        .depthBiasEnable = VK_TRUE,
        .depthBiasConstantFactor = -1024};
    pipeline_raster = &biased_state;
    VkPipeline biased = pipeline(depth_pass, push_layout, push_frag, sizeof push_frag, 0, 1, 0);
    pipeline_dynamic_raster = 1;
    VkPipeline dynamic_biased =
        pipeline(depth_pass, push_layout, push_frag, sizeof push_frag, 0, 1, 0);
    pipeline_dynamic_raster = 0;
    biased_state.rasterizerDiscardEnable = VK_TRUE;
    VkPipeline discarded = pipeline(depth_pass, push_layout, push_frag, sizeof push_frag, 0, 1, 0);
    pipeline_raster = NULL;
    begin();
    begin_render(depth_pass, depth_fb, depth_pipeline, vb, 2);
    vkCmdPushConstants(command, push_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, red);
    vkCmdDraw(command, 3, 1, 0, 0);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, biased);
    vkCmdPushConstants(command, push_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, blue);
    vkCmdDraw(command, 3, 1, 0, 0);
    vkCmdEndRenderPass(command);
    readback(target.handle, rb);
    submit();
    EXPECT(pixel(rb, 16, 16, 0, 0, 255, 0), "static negative depth bias changes LESS comparison");
    begin();
    begin_render(depth_pass, depth_fb, dynamic_biased, vb, 2);
    vkCmdSetLineWidth(command, 1);
    vkCmdSetDepthBias(command, 1024, 0, 0);
    vkCmdPushConstants(command, push_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, red);
    vkCmdDraw(command, 3, 1, 0, 0);
    vkCmdSetDepthBias(command, -1024, 0, 0);
    vkCmdPushConstants(command, push_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, blue);
    vkCmdDraw(command, 3, 1, 0, 0);
    vkCmdEndRenderPass(command);
    readback(target.handle, rb);
    submit();
    EXPECT(pixel(rb, 16, 16, 0, 0, 255, 0),
           "dynamic depth bias and unit line width affect recorded draws");
    begin();
    begin_render(depth_pass, depth_fb, discarded, vb, 2);
    vkCmdDraw(command, 3, 1, 0, 0);
    vkCmdEndRenderPass(command);
    readback(target.handle, rb);
    submit();
    EXPECT(pixel(rb, 16, 16, 0, 0, 0, 0), "rasterizer discard works without a fragment stage");
#ifndef ZVK_LOADER
    begin();
    vkCmdSetLineWidth(command, 2);
    EXPECT(vkEndCommandBuffer(command) == VK_ERROR_FEATURE_NOT_PRESENT,
           "wide line width remains rejected without feature support");
#endif
    vkDestroyPipeline(device, biased, NULL);
    vkDestroyPipeline(device, dynamic_biased, NULL);
    vkDestroyPipeline(device, discarded, NULL);
    free_buffer(depth_rb);
    vkDestroyPipeline(device, depth_pipeline, NULL);
    vkDestroyFramebuffer(device, depth_fb, NULL);
    vkDestroyRenderPass(device, depth_pass, NULL);
    free_image(depth_image);
    VkPipeline phi = pipeline(pass, layout, phi_frag_spvasm, sizeof phi_frag_spvasm, 0, 0, 0);
    begin();
    begin_render(pass, fb, phi, vb, 1);
    vkCmdDraw(command, 3, 1, 0, 0);
    vkCmdEndRenderPass(command);
    readback(target.handle, rb);
    submit();
    EXPECT(pixel(rb, 16, 16, 191, 64, 128, 1), "parallel phi copies and switch control flow");
    vkDestroyPipeline(device, phi, NULL);
    VkPipeline builtin = pipeline_shaders(builtin_vert, sizeof builtin_vert, NULL, pass, layout,
                                          triangle_frag, sizeof triangle_frag, 0, 0, 0);
    begin();
    begin_render(pass, fb, builtin, vb, 1);
    vkCmdDraw(command, 3, 2, 0, 0);
    vkCmdEndRenderPass(command);
    readback(target.handle, rb);
    submit();
    EXPECT(pixel(rb, 16, 16, 0, 255, 0, 0), "vertex and instance builtins without vertex buffers");
    begin();
    begin_render(pass, fb, builtin, vb, 1);
    vkCmdDraw(command, 3, 1, 0, 1);
    vkCmdEndRenderPass(command);
    readback(target.handle, rb);
    submit();
    EXPECT(pixel(rb, 16, 16, 0, 255, 0, 0), "nonzero firstInstance reaches InstanceIndex");
    begin();
    begin_render(pass, fb, builtin, vb, 1);
    vkCmdBindIndexBuffer(command, ib.handle, 0, VK_INDEX_TYPE_UINT16);
    vkCmdDrawIndexed(command, 3, 1, 1, 0, 1);
    vkCmdEndRenderPass(command);
    readback(target.handle, rb);
    submit();
    EXPECT(pixel(rb, 16, 16, 0, 255, 0, 0), "indexed draws preserve nonzero firstInstance");
    struct buffer multi =
        buffer(96, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    VkDrawIndirectCommand draws[2] = {{3, 1, 0, 0}, {3, 1, 0, 1}};
    begin();
    vkCmdUpdateBuffer(command, multi.handle, 8, sizeof draws[0], draws);
    vkCmdUpdateBuffer(command, multi.handle, 40, sizeof draws[1], draws + 1);
    indirect_barrier(multi.handle);
    begin_render(pass, fb, builtin, vb, 1);
    vkCmdDrawIndirect(command, multi.handle, 8, 2, 32);
    vkCmdEndRenderPass(command);
    readback(target.handle, rb);
    submit();
    EXPECT(pixel(rb, 16, 16, 0, 255, 0, 0),
           "multiple indirect draws honor stride, updates and firstInstance");
    VkDrawIndexedIndirectCommand indexed_draws[2] = {{3, 1, 1, 0, 0}, {3, 1, 1, 0, 1}};
    begin();
    vkCmdUpdateBuffer(command, multi.handle, 8, sizeof indexed_draws[0], indexed_draws);
    vkCmdUpdateBuffer(command, multi.handle, 40, sizeof indexed_draws[1], indexed_draws + 1);
    indirect_barrier(multi.handle);
    begin_render(pass, fb, builtin, vb, 1);
    vkCmdBindIndexBuffer(command, ib.handle, 0, VK_INDEX_TYPE_UINT16);
    vkCmdDrawIndexedIndirect(command, multi.handle, 8, 2, 32);
    vkCmdEndRenderPass(command);
    readback(target.handle, rb);
    submit();
    EXPECT(pixel(rb, 16, 16, 0, 255, 0, 0),
           "multiple indexed indirect draws preserve base instance");
#ifndef ZVK_LOADER
    begin();
    begin_render(pass, fb, builtin, vb, 1);
    vkCmdDrawIndirect(command, multi.handle, 72, 2, 32);
    vkCmdEndRenderPass(command);
    EXPECT(vkEndCommandBuffer(command) == VK_ERROR_FEATURE_NOT_PRESENT,
           "indirect array range includes its final command");
#endif
    free_buffer(multi);
    vkDestroyPipeline(device, builtin, NULL);
    VkPipeline instanced = pipeline_shaders(instanced_vert, sizeof instanced_vert, NULL, pass,
                                            layout, triangle_frag, sizeof triangle_frag, 0, 0, 0);
    struct buffer instance_data = buffer(40, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    float colors[10] = {-1, 1, 0, 0, 0, 1, 0, 0, 0, 1};
    memcpy(instance_data.map, colors, sizeof colors);
    begin();
    begin_render(pass, fb, instanced, vb, 1);
    VkDeviceSize instance_offset = 4;
    vkCmdBindVertexBuffers(command, 0, 1, &instance_data.handle, &instance_offset);
    vkCmdDraw(command, 3, 1, 0, 1);
    vkCmdEndRenderPass(command);
    readback(target.handle, rb);
    submit();
    EXPECT(pixel(rb, 16, 16, 0, 255, 0, 0), "firstInstance rebases instance-rate vertex buffers");
    free_buffer(instance_data);
    vkDestroyPipeline(device, instanced, NULL);

    VkPipeline integer = pipeline_shaders(integer_vert, sizeof integer_vert, NULL, pass, layout,
                                          triangle_frag, sizeof triangle_frag, 0, 0, 0);
    int32_t integer_verts[15] = {-8, -8, 32, 64, 128, 8, -8, 32, 64, 128, 0, 8, 32, 64, 128};
    memcpy(vb.map, integer_verts, sizeof integer_verts);
    begin();
    begin_render(pass, fb, integer, vb, 1);
    vkCmdDraw(command, 3, 1, 0, 0);
    vkCmdEndRenderPass(command);
    readback(target.handle, rb);
    submit();
    EXPECT(pixel(rb, 16, 16, 32, 64, 128, 1), "signed and unsigned integer vertex attributes");
    vkDestroyPipeline(device, integer, NULL);
    memcpy(vb.map, verts, sizeof verts);
    VkPipeline nan_pipeline = pipeline(pass, push_layout, nan_frag, sizeof nan_frag, 0, 0, 0);
    float zeros[4] = {0};
    begin();
    begin_render(pass, fb, nan_pipeline, vb, 1);
    vkCmdPushConstants(command, push_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, zeros);
    vkCmdDraw(command, 3, 1, 0, 0);
    vkCmdEndRenderPass(command);
    readback(target.handle, rb);
    submit();
    EXPECT(pixel(rb, 16, 16, 255, 255, 0, 0), "unordered NaN comparison, isnan and scalar select");
    vkDestroyPipeline(device, nan_pipeline, NULL);
    VkPipeline insert = pipeline(pass, push_layout, dynamic_insert_frag_spvasm,
                                  sizeof dynamic_insert_frag_spvasm, 0, 0, 0);
    for (uint32_t lane=0;lane<4;lane++) {
        begin();
        begin_render(pass,fb,insert,vb,1);
        vkCmdPushConstants(command,push_layout,VK_SHADER_STAGE_FRAGMENT_BIT,0,4,&lane);
        vkCmdDraw(command,3,1,0,0);
        vkCmdEndRenderPass(command);
        readback(target.handle,rb);
        submit();
        const uint8_t *value=(uint8_t *)rb.map+16+(16*40+16)*4;
        EXPECT(lane==3 ? value[0]==0 && value[1]==0 && value[2]==0 && value[3]==128 :
               pixel(rb,16,16,lane==0?128:0,lane==1?128:0,lane==2?128:0,1),
               "dynamic vector insertion, debug function parameters and half quantization execute on GPU");
    }
    vkDestroyPipeline(device,insert,NULL);
    VkPipeline idorder =
        pipeline(pass, layout, idorder_frag_spvasm, sizeof idorder_frag_spvasm, 0, 0, 0);
    begin();
    begin_render(pass, fb, idorder, vb, 1);
    vkCmdDraw(command, 3, 1, 0, 0);
    vkCmdEndRenderPass(command);
    readback(target.handle, rb);
    submit();
    EXPECT(pixel(rb, 16, 16, 128, 128, 191, 1),
           "nonmonotonic SPIR-V IDs and scalar insertion preserving other lanes");
    vkDestroyPipeline(device, idorder, NULL);
    struct {
        float red;
        uint32_t selected;
    } specialized = {.75f, 1};
    VkSpecializationMapEntry entries[2] = {{0, 0, 4}, {1, 4, 4}};
    VkSpecializationInfo spec = {.mapEntryCount = 2,
                                 .pMapEntries = entries,
                                 .dataSize = sizeof specialized,
                                 .pData = &specialized};
    VkPipeline specialized_pipeline =
        pipeline_shaders(triangle_vert, sizeof triangle_vert, &spec, pass, layout, specialize_frag,
                         sizeof specialize_frag, 0, 0, 0);
    begin();
    begin_render(pass, fb, specialized_pipeline, vb, 1);
    vkCmdDraw(command, 3, 1, 0, 0);
    vkCmdEndRenderPass(command);
    readback(target.handle, rb);
    submit();
    EXPECT(pixel(rb, 16, 16, 191, 128, 255, 1), "float and Boolean specialization constants");
    vkDestroyPipeline(device, specialized_pipeline, NULL);
    /* Indirect arguments are read at execution, after preceding updates. */
    struct buffer indirect =
        buffer(64, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    VkDrawIndirectCommand arguments = {3, 1, 0, 0};
    begin();
    vkCmdUpdateBuffer(command, indirect.handle, 16, sizeof arguments, &arguments);
    indirect_barrier(indirect.handle);
    begin_render(pass, fb, tri, vb, 1);
    vkCmdDrawIndirect(command, indirect.handle, 16, 1, sizeof arguments);
    vkCmdEndRenderPass(command);
    readback(target.handle, rb);
    submit();
    EXPECT(pixel(rb, 16, 16, 56, 66, 133, 5),
           "indirect draw reads buffer updated during command execution");
    VkDrawIndexedIndirectCommand indexed_args = {3, 1, 1, 0, 0};
    begin();
    vkCmdUpdateBuffer(command, indirect.handle, 8, sizeof indexed_args, &indexed_args);
    indirect_barrier(indirect.handle);
    begin_render(pass, fb, tri, vb, 1);
    vkCmdBindIndexBuffer(command, ib.handle, 0, VK_INDEX_TYPE_UINT16);
    vkCmdDrawIndexedIndirect(command, indirect.handle, 8, 1, sizeof indexed_args);
    vkCmdEndRenderPass(command);
    readback(target.handle, rb);
    submit();
    EXPECT(pixel(rb, 16, 16, 56, 66, 133, 5), "indexed indirect draw and firstIndex");
    free_buffer(indirect);
    /* Color blits can scale and reverse a source axis on the GPU. */
    struct image blitted = image(32, 32, VK_FORMAT_R8G8B8A8_UNORM,
                                 VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    begin();
    barrier(blitted.handle, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageBlit blit = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                        .srcOffsets = {{0, 32, 0}, {32, 0, 1}},
                        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                        .dstOffsets = {{0, 0, 0}, {32, 32, 1}}};
    vkCmdBlitImage(command, target.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, blitted.handle,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
    barrier(blitted.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    readback(blitted.handle, rb);
    submit();
    EXPECT(pixel(rb, 16, 15, 56, 66, 133, 5), "GPU image blit with vertical reversal");
    free_image(blitted);
    struct image stencil_image =
        image(32, 32, VK_FORMAT_D24_UNORM_S8_UINT,
              VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    VkAttachmentDescription stencil_attachments[2] = {
        attachment,
        {.format = VK_FORMAT_D24_UNORM_S8_UINT,
         .samples = VK_SAMPLE_COUNT_1_BIT,
         .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
         .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
         .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
         .stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE,
         .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
         .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL}};
    VkAttachmentReference stencil_ref = {1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    subpass.pDepthStencilAttachment = &stencil_ref;
    ri.attachmentCount = 2;
    ri.pAttachments = stencil_attachments;
    VkRenderPass stencil_pass;
    VK(vkCreateRenderPass(device, &ri, NULL, &stencil_pass));
    VkImageView stencil_views[2] = {target.view, stencil_image.view};
    fi.renderPass = stencil_pass;
    fi.attachmentCount = 2;
    fi.pAttachments = stencil_views;
    VkFramebuffer stencil_fb;
    VK(vkCreateFramebuffer(device, &fi, NULL, &stencil_fb));
    VkPipeline replace = pipeline(stencil_pass, push_layout, push_frag, sizeof push_frag, 0, 2, 0),
               compare = pipeline(stencil_pass, push_layout, push_frag, sizeof push_frag, 0, 3, 0);
    VkQueryPoolCreateInfo query_info = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                        .queryType = VK_QUERY_TYPE_OCCLUSION,
                                        .queryCount = 2};
    VkQueryPool queries;
    VK(vkCreateQueryPool(device, &query_info, NULL, &queries));
    struct buffer query_buffer = buffer(32, VK_BUFFER_USAGE_TRANSFER_DST_BIT),
                  stencil_readback = buffer(5120, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    begin();
    vkCmdResetQueryPool(command, queries, 0, 2);
    begin_render(stencil_pass, stencil_fb, replace, vb, 2);
    vkCmdSetStencilCompareMask(command, VK_STENCIL_FACE_FRONT_AND_BACK, 255);
    vkCmdSetStencilWriteMask(command, VK_STENCIL_FACE_FRONT_AND_BACK, 255);
    vkCmdSetStencilReference(command, VK_STENCIL_FACE_FRONT_AND_BACK, 5);
    vkCmdPushConstants(command, push_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, red);
    vkCmdBeginQuery(command, queries, 0, 0);
    vkCmdDraw(command, 3, 1, 0, 0);
    vkCmdEndQuery(command, queries, 0);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, compare);
    vkCmdSetStencilReference(command, VK_STENCIL_FACE_FRONT_AND_BACK, 6);
    vkCmdPushConstants(command, push_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, blue);
    vkCmdBeginQuery(command, queries, 1, 0);
    vkCmdDraw(command, 3, 1, 0, 0);
    vkCmdEndQuery(command, queries, 1);
    VkRect2D no_scissor = {{0, 0}, {0, 0}};
    vkCmdSetScissor(command, 0, 1, &no_scissor);
    vkCmdSetStencilWriteMask(command, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
    VkClearAttachment attachment_clear = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                          .colorAttachment = 0,
                                          .clearValue = {.color = {{0, 0, 1, 1}}}};
    VkClearRect clear_rect = {.rect = {{0, 0}, {8, 8}}, .layerCount = 1};
    vkCmdClearAttachments(command, 1, &attachment_clear, 1, &clear_rect);
    attachment_clear.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
    attachment_clear.clearValue.depthStencil =
        (VkClearDepthStencilValue){.depth = .25f, .stencil = 9};
    clear_rect.rect = (VkRect2D){{8, 8}, {16, 16}};
    vkCmdClearAttachments(command, 1, &attachment_clear, 1, &clear_rect);
    VkRect2D full_scissor = {{0, 0}, {32, 32}};
    vkCmdSetScissor(command, 0, 1, &full_scissor);
    vkCmdSetStencilReference(command, VK_STENCIL_FACE_FRONT_AND_BACK, 9);
    vkCmdDraw(command, 3, 1, 0, 0);
    vkCmdEndRenderPass(command);
    readback(target.handle, rb);
    VkBufferImageCopy stencil_copies[2] = {
        {.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1}, .imageExtent = {32, 32, 1}},
        {.bufferOffset = 4096,
         .imageSubresource = {VK_IMAGE_ASPECT_STENCIL_BIT, 0, 0, 1},
         .imageExtent = {32, 32, 1}}};
    barrier_aspect(stencil_image.handle,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT);
    vkCmdCopyImageToBuffer(command, stencil_image.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           stencil_readback.handle, 2, stencil_copies);
    vkCmdCopyQueryPoolResults(command, queries, 0, 2, query_buffer.handle, 0, 16,
                              VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT |
                                  VK_QUERY_RESULT_WAIT_BIT);
    submit();
    EXPECT(pixel(rb, 16, 16, 0, 0, 255, 0),
           "dynamic stencil reference, write/compare masks and equality test");
    EXPECT(pixel(rb, 0, 0, 0, 0, 255, 0), "attachment color clear ignores pipeline scissor");
    EXPECT((((uint32_t *)stencil_readback.map)[16 * 32 + 16] & 0xffffffu) == 0xffffffu &&
               ((uint8_t *)stencil_readback.map)[4096 + 16 * 32 + 16] == 9 &&
               ((uint8_t *)stencil_readback.map)[4096] == 0,
           "stencil attachment clear ignores write mask and preserves depth");
    uint64_t query_values[4] = {0};
    VK(vkGetQueryPoolResults(device, queries, 0, 2, sizeof query_values, query_values, 16,
                             VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT |
                                 VK_QUERY_RESULT_WAIT_BIT));
    EXPECT(query_values[0] > 100 && query_values[1] == 1 && query_values[2] == 0 &&
               query_values[3] == 1,
           "GPU occlusion queries distinguish visible and stencil-rejected fragments");
    EXPECT(!memcmp(query_values, query_buffer.map, 32),
           "query results copied into a buffer with availability and stride");
    uint64_t saved_query=query_values[0];
    PFN_vkResetQueryPoolEXT host_reset=(PFN_vkResetQueryPoolEXT)
        vkGetDeviceProcAddr(device,"vkResetQueryPoolEXT");
    EXPECT(host_reset!=NULL,"enabled host query reset has a device entry point");
    if (host_reset) {
        host_reset(device,queries,1,1);
        query_values[2]=123;
        EXPECT(vkGetQueryPoolResults(device,queries,0,2,sizeof query_values,query_values,16,
                   VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WITH_AVAILABILITY_BIT)==VK_NOT_READY &&
               query_values[0]==saved_query && query_values[1]==1 &&
               query_values[2]==123 && query_values[3]==0,
               "host query reset invalidates only the requested query and preserves unavailable output");
    }
    begin();
    vkCmdResetQueryPool(command, queries, 0, 2);
    submit();
    query_values[0] = query_values[2] = 123;
    EXPECT(vkGetQueryPoolResults(device, queries, 0, 2, sizeof query_values, query_values, 16,
                                 VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) ==
                   VK_NOT_READY &&
               query_values[0] == 123 && query_values[1] == 0 && query_values[3] == 0,
           "query reset clears availability without overwriting unavailable results");
    vkDestroyQueryPool(device, queries, NULL);
    free_buffer(query_buffer);
    free_buffer(stencil_readback);
    vkDestroyPipeline(device, replace, NULL);
    vkDestroyPipeline(device, compare, NULL);
    vkDestroyFramebuffer(device, stencil_fb, NULL);
    vkDestroyRenderPass(device, stencil_pass, NULL);
    free_image(stencil_image);
    vkDestroyPipeline(device, flow, NULL);
    vkDestroyPipeline(device, push, NULL);
    vkDestroyPipelineLayout(device, push_layout, NULL);
    free_buffer(ib);
    vkDestroyPipeline(device, tri, NULL);
    vkDestroyPipelineLayout(device, layout, NULL);
    vkDestroyFramebuffer(device, fb, NULL);
    vkDestroyRenderPass(device, pass, NULL);
    free_image(target);
    free_buffer(vb);
    free_buffer(rb);
}
struct allocations {
    unsigned calls, live, fail;
};
static void mip_transfers(void)
{
    struct image tex =
        image_levels(4, 4, VK_FORMAT_R8G8B8A8_UNORM,
                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 3, 3, 0);
    struct buffer bytes =
        buffer(512, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    memset(bytes.map, 0xcd, 512);
    for (unsigned l = 0; l < 3; l++)
        for (unsigned y = 0; y < 2; y++)
            for (unsigned x = 0; x < 2; x++) {
                uint32_t pixel = 0xff1e1400u | (10 + l * 30);
                memcpy((uint8_t *)bytes.map + 16 + l * 48 + y * 16 + x * 4, &pixel, 4);
            }
    VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                              .image = tex.handle,
                              .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                              .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0,
                                                   VK_REMAINING_MIP_LEVELS, 0,
                                                   VK_REMAINING_ARRAY_LAYERS}};
    begin();
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    VkClearColorValue clear = {{0, 0, 0, 1}};
    vkCmdClearColorImage(command, tex.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1,
                         &b.subresourceRange);
    VkBufferImageCopy region = {.bufferOffset = 16,
                                .bufferRowLength = 4,
                                .bufferImageHeight = 3,
                                .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 1, 0, 3},
                                .imageExtent = {2, 2, 1}};
    vkCmdCopyBufferToImage(command, bytes.handle, tex.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           1, &region);
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, 2, 1};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    region.bufferOffset = 256;
    region.imageSubresource.baseArrayLayer = 2;
    region.imageSubresource.layerCount = 1;
    vkCmdCopyImageToBuffer(command, tex.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, bytes.handle,
                           1, &region);
    submit();
    uint8_t *out = (uint8_t *)bytes.map + 256;
    EXPECT(out[0] == 70 && out[1] == 20 && out[2] == 30 && out[16] == 70 && out[8] == 0xcd,
           "mip-level array upload/readback preserves padded rows and layers");
    begin();
    VkImageBlit blit = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 1, 2, 1},
                        .srcOffsets = {{0, 0, 0}, {2, 2, 1}},
                        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 2, 0, 1},
                        .dstOffsets = {{0, 0, 0}, {1, 1, 1}}};
    vkCmdBlitImage(command, tex.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, tex.handle,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
    b.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 2, 1, 0, 1};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    VkImageCopy copy = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 2, 0, 1},
                        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, 1},
                        .extent = {1, 1, 1}};
    vkCmdCopyImage(command, tex.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, tex.handle,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    b.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, 1, 1};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    region = (VkBufferImageCopy){.bufferOffset = 320,
                                 .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, 1},
                                 .imageExtent = {1, 1, 1}};
    vkCmdCopyImageToBuffer(command, tex.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, bytes.handle,
                           1, &region);
    submit();
    out = (uint8_t *)bytes.map + 320;
    EXPECT(out[0] == 70 && out[1] == 20 && out[2] == 30,
           "mip blit and cross-level image copy respect independent subresource layouts");
    free_buffer(bytes);
    free_image(tex);
}
static void multisampling(void)
{
    VkImageFormatProperties props;
    VK(vkGetPhysicalDeviceImageFormatProperties(
        physical, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 0, &props));
    EXPECT(props.sampleCounts & VK_SAMPLE_COUNT_4_BIT,
           "four-sample color attachments advertised by the backend");
    if (!(props.sampleCounts & VK_SAMPLE_COUNT_4_BIT))
        return;
    struct image ms =
        image_config(32, 32, VK_FORMAT_R8G8B8A8_UNORM,
                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 1, 1, 0,
                     VK_SAMPLE_COUNT_4_BIT);
    struct image target =
        image(32, 32, VK_FORMAT_R8G8B8A8_UNORM,
              VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    struct image explicit_target =
        image(32, 32, VK_FORMAT_R8G8B8A8_UNORM,
              VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    VkAttachmentDescription attachments[2] = {
        {.format = VK_FORMAT_R8G8B8A8_UNORM,
         .samples = VK_SAMPLE_COUNT_4_BIT,
         .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
         .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
         .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
         .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL},
        {.format = VK_FORMAT_R8G8B8A8_UNORM,
         .samples = VK_SAMPLE_COUNT_1_BIT,
         .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
         .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
         .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
         .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL}};
    VkAttachmentReference color = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
                          resolve = {1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                .colorAttachmentCount = 1,
                                .pColorAttachments = &color,
                                .pResolveAttachments = &resolve};
    VkRenderPassCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                 .attachmentCount = 2,
                                 .pAttachments = attachments,
                                 .subpassCount = 1,
                                 .pSubpasses = &sub};
    VkRenderPass pass;
    VK(vkCreateRenderPass(device, &ri, NULL, &pass));
    VkImageView views[2] = {ms.view, target.view};
    VkFramebufferCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                  .renderPass = pass,
                                  .attachmentCount = 2,
                                  .pAttachments = views,
                                  .width = 32,
                                  .height = 32,
                                  .layers = 1};
    VkFramebuffer fb;
    VK(vkCreateFramebuffer(device, &fi, NULL, &fb));
    VkPushConstantRange push = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                     .pushConstantRangeCount = 1,
                                     .pPushConstantRanges = &push};
    VkPipelineLayout layout;
    VK(vkCreatePipelineLayout(device, &li, NULL, &layout));
    pipeline_samples = VK_SAMPLE_COUNT_4_BIT;
    VkPipeline p = pipeline(pass, layout, push_frag, sizeof push_frag, 0, 0, 0);
    pipeline_samples = VK_SAMPLE_COUNT_1_BIT;
    const float verts[] = {-.8f, -.8f, 1, 0, 0, .8f, -.8f, 0, 1, 0, 0, .8f, 0, 0, 1};
    struct buffer vb = buffer(sizeof verts, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    memcpy(vb.map, verts, sizeof verts);
    struct buffer rb = buffer(16 + 40 * 32 * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                .queryType = VK_QUERY_TYPE_OCCLUSION,
                                .queryCount = 1};
    VkQueryPool query;
    VK(vkCreateQueryPool(device, &qi, NULL, &query));
    begin();
    vkCmdResetQueryPool(command, query, 0, 1);
    begin_render(pass, fb, p, vb, 1);
    float value[4] = {.25, .5, .75, 1};
    vkCmdPushConstants(command, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, value);
    vkCmdDraw(command, 3, 1, 0, 0);
    vkCmdBeginQuery(command, query, 0, 0);
    VkClearAttachment clear = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                               .clearValue.color = {{1, 0, 0, 1}}};
    VkClearRect rect = {.rect = {{0, 0}, {4, 4}}, .layerCount = 1};
    vkCmdClearAttachments(command, 1, &clear, 1, &rect);
    vkCmdEndQuery(command, query, 0);
    vkCmdEndRenderPass(command);
    readback(target.handle, rb);
    submit();
    EXPECT(pixel(rb, 16, 16, 64, 128, 191, 1),
           "render-pass multisample resolve averages color samples");
    EXPECT(pixel(rb, 0, 0, 255, 0, 0, 0),
           "partial multisample attachment clear preserves sample contents");
    uint64_t result = 99;
    VK(vkGetQueryPoolResults(device, query, 0, 1, sizeof result, &result, sizeof result,
                             VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
    EXPECT(result == 0, "multisample clears do not contribute occlusion samples");
    unsigned edges = 0;
    for (int y = 4; y < 32; y++)
        for (int x = 4; x < 32; x++) {
            uint8_t *pixel = (uint8_t *)rb.map + 16 + (y * 40 + x) * 4;
            if (pixel[0] > 0 && pixel[0] < 63)
                edges++;
        }
    EXPECT(edges > 0, "resolved triangle edges contain fractional sample coverage");
    begin();
    barrier(explicit_target.handle, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageResolve region = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                             .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                             .extent = {32, 32, 1}};
    vkCmdResolveImage(command, ms.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      explicit_target.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    barrier(explicit_target.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    readback(explicit_target.handle, rb);
    submit();
    EXPECT(pixel(rb, 16, 16, 64, 128, 191, 1) && pixel(rb, 0, 0, 255, 0, 0, 0),
           "explicit multisample resolve matches render-pass resolve");
    vkDestroyQueryPool(device, query, NULL);
    vkDestroyPipeline(device, p, NULL);
    vkDestroyPipelineLayout(device, layout, NULL);
    vkDestroyFramebuffer(device, fb, NULL);
    vkDestroyRenderPass(device, pass, NULL);
    free_buffer(vb);
    free_buffer(rb);
    free_image(ms);
    free_image(target);
    free_image(explicit_target);
}
static void texture_extensions(void)
{
    const float verts[] = {-.8f, -.8f, 1, 0, 0, .8f, -.8f, 0, 1, 0, 0, .8f, 0, 0, 1};
    struct buffer vb = buffer(sizeof verts, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    memcpy(vb.map, verts, sizeof verts);
    struct buffer rb = buffer(16 + 40 * 32 * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    struct image target =
        image(32, 32, VK_FORMAT_R8G8B8A8_UNORM,
              VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    VkAttachmentDescription attachment = {.format = VK_FORMAT_R8G8B8A8_UNORM,
                                          .samples = VK_SAMPLE_COUNT_1_BIT,
                                          .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                                          .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                          .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                          .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
    VkAttachmentReference color = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                .colorAttachmentCount = 1,
                                .pColorAttachments = &color};
    VkRenderPassCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                 .attachmentCount = 1,
                                 .pAttachments = &attachment,
                                 .subpassCount = 1,
                                 .pSubpasses = &sub};
    VkRenderPass pass;
    VK(vkCreateRenderPass(device, &ri, NULL, &pass));
    VkFramebufferCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                  .renderPass = pass,
                                  .attachmentCount = 1,
                                  .pAttachments = &target.view,
                                  .width = 32,
                                  .height = 32,
                                  .layers = 1};
    VkFramebuffer fb;
    VK(vkCreateFramebuffer(device, &fi, NULL, &fb));
    for (int kind = 0; kind < 5; kind++) {
        uint32_t layers = kind == 1 ? 6 : 3;
        struct image tex =
            image_levels(4, 4, VK_FORMAT_R8G8B8A8_UNORM,
                         VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                             VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                         3, layers, kind == 1);
        VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 3, 0, layers};
        VkImageMemoryBarrier transition = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                           .image = tex.handle,
                                           .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                           .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                           .subresourceRange = all};
        begin();
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1,
                             &transition);
        for (uint32_t m = 0; m < 3; m++)
            for (uint32_t l = 0; l < layers; l++) {
                VkClearColorValue value = {{(float)m / 2, (float)l / 5, 0.25, 1}};
                VkImageSubresourceRange one = {VK_IMAGE_ASPECT_COLOR_BIT, m, 1, l, 1};
                vkCmdClearColorImage(command, tex.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                     &value, 1, &one);
            }
        transition.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        transition.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1,
                             &transition);
        submit();
        VkImageView alternate = VK_NULL_HANDLE;
        if (kind == 2) {
            VkImageViewCreateInfo vi = {
                .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .image = tex.handle,
                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                .format = VK_FORMAT_R8G8B8A8_UNORM,
                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, 2, 1}};
            VK(vkCreateImageView(device, &vi, NULL, &alternate));
        }
        VkDescriptorType type = kind >= 3 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                          : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        VkDescriptorSetLayoutBinding binding = {.binding = 0,
                                                .descriptorType = type,
                                                .descriptorCount = kind == 4   ? 12
                                                                   : kind >= 2 ? 2
                                                                               : 1,
                                                .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT};
        VkDescriptorSetLayoutCreateInfo si = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = 1,
            .pBindings = &binding};
        VkDescriptorSetLayout set_layout;
        VK(vkCreateDescriptorSetLayout(device, &si, NULL, &set_layout));
        VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                         .setLayoutCount = 1,
                                         .pSetLayouts = &set_layout};
        VkPipelineLayout layout;
        VK(vkCreatePipelineLayout(device, &li, NULL, &layout));
        const uint32_t *code = kind == 0   ? array_frag
                               : kind == 1 ? cube_frag
                               : kind == 2 ? descriptors_frag
                               : kind == 3 ? ubos_frag
                                           : ubos12_frag;
        size_t size = kind == 0   ? sizeof array_frag
                      : kind == 1 ? sizeof cube_frag
                      : kind == 2 ? sizeof descriptors_frag
                      : kind == 3 ? sizeof ubos_frag
                                  : sizeof ubos12_frag;
        VkPipeline p = pipeline(pass, layout, code, size, 0, 0, 0);
        VkDescriptorPoolSize ps = {type, binding.descriptorCount};
        VkDescriptorPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                         .maxSets = 1,
                                         .poolSizeCount = 1,
                                         .pPoolSizes = &ps};
        VkDescriptorPool dp;
        VK(vkCreateDescriptorPool(device, &pi, NULL, &dp));
        VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                          .descriptorPool = dp,
                                          .descriptorSetCount = 1,
                                          .pSetLayouts = &set_layout};
        VkDescriptorSet set;
        VK(vkAllocateDescriptorSets(device, &ai, &set));
        VkSamplerCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .maxLod = 2};
        VkSampler sampler;
        VK(vkCreateSampler(device, &sci, NULL, &sampler));
        VkDescriptorImageInfo image_info = {sampler, alternate ? alternate : tex.view,
                                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        struct buffer ub = buffer(16, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
        float uniform[4] = {0.5, 0.4, 0.25, 1};
        memcpy(ub.map, uniform, sizeof uniform);
        VkDescriptorBufferInfo buffer_info = {ub.handle, 0, 16};
        VkWriteDescriptorSet write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                      .dstSet = set,
                                      .dstBinding = 0,
                                      .dstArrayElement = kind >= 2 ? 1 : 0,
                                      .descriptorCount = 1,
                                      .descriptorType = type,
                                      .pImageInfo = &image_info,
                                      .pBufferInfo = &buffer_info};
        VkDescriptorBufferInfo uniform_array[12];
        if (kind == 4) {
            for (unsigned i = 0; i < 12; i++)
                uniform_array[i] = buffer_info;
            write.dstArrayElement = 0;
            write.descriptorCount = 12;
            write.pBufferInfo = uniform_array;
        }
        vkUpdateDescriptorSets(device, 1, &write, 0, NULL);
        begin();
        begin_render(pass, fb, p, vb, 1);
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0,
                                NULL);
        vkCmdDraw(command, 3, 1, 0, 0);
        vkCmdEndRenderPass(command);
        readback(target.handle, rb);
        submit();
        EXPECT(pixel(rb, 16, 16, kind == 1 ? 255 : 128, kind == 1 ? 0 : 102, 64, 2),
               kind == 0   ? "2D array sampling selects layer and explicit mip"
               : kind == 1 ? "cube sampling selects face and explicit mip"
               : kind == 2 ? "sampler descriptor array accesses only the statically used element"
               : kind == 3 ? "UBO descriptor array accesses only the statically used element"
                           : "twelve uniform buffers are accessible in one shader stage");
        vkDestroyPipeline(device, p, NULL);
        vkDestroyPipelineLayout(device, layout, NULL);
        vkDestroyDescriptorPool(device, dp, NULL);
        vkDestroyDescriptorSetLayout(device, set_layout, NULL);
        vkDestroySampler(device, sampler, NULL);
        if (alternate)
            vkDestroyImageView(device, alternate, NULL);
        free_image(tex);
        free_buffer(ub);
    }
    vkDestroyFramebuffer(device, fb, NULL);
    vkDestroyRenderPass(device, pass, NULL);
    free_image(target);
    free_buffer(rb);
    free_buffer(vb);
}
static void color_targets(void)
{
    const float verts[] = {-.8f, -.8f, 1, 0, 0, .8f, -.8f, 0, 1, 0, 0, .8f, 0, 0, 1};
    struct buffer vb = buffer(sizeof verts, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    memcpy(vb.map, verts, sizeof verts);
    struct buffer rb[2] = {buffer(16 + 40 * 32 * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT),
                           buffer(16 + 40 * 32 * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT)};
    struct image targets[2] = {
        image(32, 32, VK_FORMAT_R8G8B8A8_SRGB,
              VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT),
        image(32, 32, VK_FORMAT_R8G8B8A8_UNORM,
              VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT)};
    VkAttachmentDescription attachments[2] = {
        {.format = VK_FORMAT_R8G8B8A8_SRGB,
         .samples = VK_SAMPLE_COUNT_1_BIT,
         .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
         .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
         .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
         .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL},
        {.format = VK_FORMAT_R8G8B8A8_UNORM,
         .samples = VK_SAMPLE_COUNT_1_BIT,
         .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
         .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
         .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
         .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL}};
    VkAttachmentReference colors[2] = {{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
                                       {1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}};
    VkSubpassDescription sub = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                .colorAttachmentCount = 2,
                                .pColorAttachments = colors};
    VkRenderPassCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                 .attachmentCount = 2,
                                 .pAttachments = attachments,
                                 .subpassCount = 1,
                                 .pSubpasses = &sub};
    VkRenderPass pass;
    VK(vkCreateRenderPass(device, &ri, NULL, &pass));
    VkImageView views[2] = {targets[0].view, targets[1].view};
    VkFramebufferCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                  .renderPass = pass,
                                  .attachmentCount = 2,
                                  .pAttachments = views,
                                  .width = 32,
                                  .height = 32,
                                  .layers = 1};
    VkFramebuffer fb;
    VK(vkCreateFramebuffer(device, &fi, NULL, &fb));
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    VkPipelineLayout layout;
    VK(vkCreatePipelineLayout(device, &li, NULL, &layout));
    VkPipeline p = pipeline(pass, layout, mrt_frag, sizeof mrt_frag, 0, 0, 0);
    begin();
    begin_render(pass, fb, p, vb, 2);
    VkClearAttachment clear = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                               .colorAttachment = 1,
                               .clearValue.color = {{0.25, 0.5, 0.75, 1}}};
    VkClearRect rect = {.rect = {{0, 0}, {32, 32}}, .layerCount = 1};
    vkCmdClearAttachments(command, 1, &clear, 1, &rect);
    vkCmdDraw(command, 3, 1, 0, 0);
    vkCmdEndRenderPass(command);
    for (int i = 0; i < 2; i++)
        readback(targets[i].handle, rb[i]);
    submit();
    EXPECT(pixel(rb[0], 16, 16, 137, 188, 225, 2), "sRGB framebuffer encodes linear shader RGB");
    EXPECT(pixel(rb[1], 16, 16, 191, 64, 128, 2),
           "multiple render targets preserve separate fragment outputs");
    EXPECT(pixel(rb[1], 0, 0, 64, 128, 191, 1), "clear attachment selects the second color slot");
    vkDestroyPipeline(device,p,NULL);
    vkDestroyFramebuffer(device,fb,NULL);
    vkDestroyRenderPass(device,pass,NULL);
    VkAttachmentReference sparse_colors[4]={{VK_ATTACHMENT_UNUSED,VK_IMAGE_LAYOUT_UNDEFINED},
        {VK_ATTACHMENT_UNUSED,VK_IMAGE_LAYOUT_UNDEFINED},{VK_ATTACHMENT_UNUSED,VK_IMAGE_LAYOUT_UNDEFINED},
        {1,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}};
    sub.colorAttachmentCount=4;sub.pColorAttachments=sparse_colors;
    VK(vkCreateRenderPass(device,&ri,NULL,&pass));fi.renderPass=pass;
    VK(vkCreateFramebuffer(device,&fi,NULL,&fb));
    pipeline_color_count=4;
    p=pipeline(pass,layout,sparse_color_frag,sizeof sparse_color_frag,0,0,0);
    pipeline_color_count=-1;
    begin();begin_render(pass,fb,p,vb,2);vkCmdDraw(command,3,1,0,0);vkCmdEndRenderPass(command);
    for(unsigned i=0;i<2;i++)readback(targets[i].handle,rb[i]);
    submit();
    EXPECT(pixel(rb[1],16,16,64,128,191,1) && pixel(rb[0],16,16,137,188,225,2),
           "a single fragment output at location three reaches its sparse color slot and preserves unused attachments");
    vkDestroyPipeline(device, p, NULL);
    vkDestroyPipelineLayout(device, layout, NULL);
    vkDestroyFramebuffer(device, fb, NULL);
    vkDestroyRenderPass(device, pass, NULL);
    for (int i = 0; i < 2; i++) {
        free_image(targets[i]);
        free_buffer(rb[i]);
    }
    free_buffer(vb);
}
static void *VKAPI_CALL alloc_cb(void *user, size_t size, size_t alignment,
                                 VkSystemAllocationScope scope)
{
    (void)scope;
    struct allocations *a = user;
    if (a->calls++ == a->fail)
        return NULL;
    void *p = NULL;
    if (posix_memalign(&p, alignment, size))
        p = NULL;
    if (p)
        a->live++;
    return p;
}
static void VKAPI_CALL free_cb(void *user, void *p)
{
    if (p) {
        struct allocations *a = user;
        a->live--;
        free(p);
    }
}
static void *VKAPI_CALL realloc_cb(void *user, void *old, size_t size, size_t alignment,
                                   VkSystemAllocationScope scope)
{
    (void)alignment;
    (void)scope;
    if (!old)
        return alloc_cb(user, size, alignment, scope);
    return realloc(old, size);
}
static void packed_depth_transfers(void)
{
    struct image im = image(2, 2, VK_FORMAT_D24_UNORM_S8_UINT,
                            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    struct buffer out = buffer(32, VK_BUFFER_USAGE_TRANSFER_DST_BIT),
                  input = buffer(16, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    VkImageSubresourceRange aspects = {VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0,
                                       1, 0, 1};
    VkClearDepthStencilValue clear = {.25f, 77};
    VkBufferImageCopy copies[2] = {
        {.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1}, .imageExtent = {2, 2, 1}},
        {.bufferOffset = 16,
         .imageSubresource = {VK_IMAGE_ASPECT_STENCIL_BIT, 0, 0, 1},
         .imageExtent = {2, 2, 1}}};
    begin();
    barrier_aspect(im.handle, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   aspects.aspectMask);
    vkCmdClearDepthStencilImage(command, im.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1,
                                &aspects);
    barrier_aspect(im.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, aspects.aspectMask);
    vkCmdCopyImageToBuffer(command, im.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, out.handle, 2,
                           copies);
    submit();
    EXPECT((((uint32_t *)out.map)[0] & 0xffffffu) == 0x400000u && ((uint8_t *)out.map)[16] == 77 &&
               ((uint8_t *)out.map)[19] == 77,
           "packed depth and byte-sized stencil readback");
    memset(input.map, 99, 16);
    VkBufferImageCopy stencil = copies[1];
    stencil.bufferOffset = 0;
    begin();
    barrier_aspect(im.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, aspects.aspectMask);
    vkCmdCopyBufferToImage(command, input.handle, im.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           1, &stencil);
    barrier_aspect(im.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, aspects.aspectMask);
    vkCmdCopyImageToBuffer(command, im.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, out.handle, 2,
                           copies);
    submit();
    EXPECT((((uint32_t *)out.map)[0] & 0xffffffu) == 0x400000u && ((uint8_t *)out.map)[16] == 99,
           "stencil-only upload preserves depth");
    for (int i = 0; i < 4; i++)
        ((uint32_t *)input.map)[i] = 0xbfffffu;
    begin();
    barrier_aspect(im.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, aspects.aspectMask);
    vkCmdCopyBufferToImage(command, input.handle, im.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           1, copies);
    barrier_aspect(im.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, aspects.aspectMask);
    vkCmdCopyImageToBuffer(command, im.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, out.handle, 2,
                           copies);
    submit();
    EXPECT((((uint32_t *)out.map)[0] & 0xffffffu) == 0xbfffffu && ((uint8_t *)out.map)[16] == 99,
           "depth-only upload preserves stencil");
    struct image dst = image(2, 2, VK_FORMAT_D24_UNORM_S8_UINT,
                             VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    clear.depth = .5f;
    clear.stencil = 1;
    VkImageCopy copy = {.srcSubresource = {VK_IMAGE_ASPECT_STENCIL_BIT, 0, 0, 1},
                        .dstSubresource = {VK_IMAGE_ASPECT_STENCIL_BIT, 0, 0, 1},
                        .extent = {2, 2, 1}};
    begin();
    barrier_aspect(dst.handle, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   aspects.aspectMask);
    vkCmdClearDepthStencilImage(command, dst.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear,
                                1, &aspects);
    vkCmdCopyImage(command, im.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.handle,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    barrier_aspect(dst.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, aspects.aspectMask);
    vkCmdCopyImageToBuffer(command, dst.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, out.handle, 2,
                           copies);
    submit();
    EXPECT((((uint32_t *)out.map)[0] & 0xffffffu) == 0x800000u && ((uint8_t *)out.map)[16] == 99,
           "stencil-only image copy preserves destination depth");
    free_image(dst);
    free_image(im);
    free_buffer(out);
    free_buffer(input);
}
static void allocation_tests(void)
{
    VkPipelineCacheCreateInfo cache_info={.sType=VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    VkPipelineCache cache;
    VK(vkCreatePipelineCache(device,&cache_info,NULL,&cache));
    unsigned char cache_data[sizeof(VkPipelineCacheHeaderVersionOne)+8], untouched[sizeof cache_data];
    memset(untouched,0xa7,sizeof untouched);
    int short_cache_ok=1;
    for (size_t n=0;n<sizeof(VkPipelineCacheHeaderVersionOne);n++) {
        memcpy(cache_data,untouched,sizeof cache_data);
        size_t bytes=n;
        short_cache_ok &= vkGetPipelineCacheData(device,cache,&bytes,cache_data+4)==VK_INCOMPLETE &&
            bytes==0 && !memcmp(cache_data,untouched,sizeof cache_data);
    }
    EXPECT(short_cache_ok,"every undersized cache buffer stays untouched and reports zero bytes");
    size_t cache_bytes=sizeof(VkPipelineCacheHeaderVersionOne);
    VK(vkGetPipelineCacheData(device,cache,&cache_bytes,cache_data+4));
    EXPECT(cache_bytes==sizeof(VkPipelineCacheHeaderVersionOne) &&
           !memcmp(cache_data,untouched,4) && !memcmp(cache_data+4+cache_bytes,untouched,4),
           "a complete cache header respects both buffer guards");
    vkDestroyPipelineCache(device,cache,NULL);
    VkBufferCreateInfo uniform_info={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=17,
        .usage=VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT};
    VkBuffer uniform;
    VK(vkCreateBuffer(device,&uniform_info,NULL,&uniform));
    VkMemoryRequirements uniform_req;
    vkGetBufferMemoryRequirements(device,uniform,&uniform_req);
    EXPECT(uniform_req.alignment>=properties.limits.minUniformBufferOffsetAlignment &&
           uniform_req.size>=17 && uniform_req.size%uniform_req.alignment==0,
           "uniform buffer memory requirements respect advertised alignment and rounded size");
    VkMemoryAllocateInfo uniform_alloc={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize=uniform_req.size+uniform_req.alignment};
    VkDeviceMemory uniform_memory;
    VK(vkAllocateMemory(device,&uniform_alloc,NULL,&uniform_memory));
    EXPECT(vkBindBufferMemory(device,uniform,uniform_memory,uniform_req.alignment/2)==VK_ERROR_OUT_OF_DEVICE_MEMORY,
           "uniform buffer binding rejects an offset below its required alignment");
    VK(vkBindBufferMemory(device,uniform,uniform_memory,uniform_req.alignment));
    vkDestroyBuffer(device,uniform,NULL);vkFreeMemory(device,uniform_memory,NULL);
    struct allocations a = {.fail = 1};
    VkAllocationCallbacks cb = {.pUserData = &a,
                                .pfnAllocation = alloc_cb,
                                .pfnReallocation = realloc_cb,
                                .pfnFree = free_cb};
    VkMemoryAllocateInfo mi = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                               .allocationSize = 64};
    VkDeviceMemory memory = VK_NULL_HANDLE;
    EXPECT(vkAllocateMemory(device, &mi, &cb, &memory) == VK_ERROR_OUT_OF_HOST_MEMORY &&
               memory == VK_NULL_HANDLE && a.live == 0,
           "memory allocation failure rolls back object");
    a.calls = 0;
    a.fail = UINT32_MAX;
    VK(vkAllocateMemory(device, &mi, &cb, &memory));
    void *mapped;
    VK(vkMapMemory(device, memory, 3, 16, 0, &mapped));
    EXPECT(((uintptr_t)mapped - 3) % 64 == 0,
           "allocation callbacks preserve mapped base alignment at nonzero offsets");
    vkUnmapMemory(device, memory);
    vkFreeMemory(device, memory, &cb);
    VkDeviceMemory *allocations = calloc(properties.limits.maxMemoryAllocationCount, sizeof *allocations);
    if (!allocations)
        exit(2);
    mi.allocationSize = 1;
    for (uint32_t i = 0; i < properties.limits.maxMemoryAllocationCount; i++)
        VK(vkAllocateMemory(device, &mi, NULL, allocations + i));
    memory = VK_NULL_HANDLE;
    EXPECT(vkAllocateMemory(device, &mi, NULL, &memory) == VK_ERROR_OUT_OF_DEVICE_MEMORY && !memory,
           "allocation count limit returns device OOM without a partial allocation");
    for (uint32_t i = 0; i < properties.limits.maxMemoryAllocationCount; i++)
        vkFreeMemory(device, allocations[i], NULL);
    free(allocations);
    VK(vkAllocateMemory(device, &mi, NULL, &memory));
    EXPECT(memory != VK_NULL_HANDLE, "freeing memory restores the allocation count budget");
    vkFreeMemory(device, memory, NULL);
    VkPhysicalDeviceMemoryProperties heaps;
    vkGetPhysicalDeviceMemoryProperties(physical, &heaps);
    mi.allocationSize = heaps.memoryHeaps[0].size / 2;
    VkDeviceMemory halves[2];
    VK(vkAllocateMemory(device, &mi, NULL, halves));
    VK(vkAllocateMemory(device, &mi, NULL, halves + 1));
    mi.allocationSize = 1;
    EXPECT(vkAllocateMemory(device, &mi, NULL, &memory) == VK_ERROR_OUT_OF_DEVICE_MEMORY && !memory,
           "allocations collectively respect the reported memory heap budget");
    vkFreeMemory(device, halves[0], NULL);
    vkFreeMemory(device, halves[1], NULL);
    VK(vkAllocateMemory(device, &mi, NULL, &memory));
    EXPECT(memory != VK_NULL_HANDLE, "freeing memory restores the heap byte budget");
    vkFreeMemory(device, memory, NULL);
    VkSubpassDescription subpasses[3] = {{.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS},
                                         {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS},
                                         {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS}};
    VkRenderPassCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                 .subpassCount = 3,
                                 .pSubpasses = subpasses};
    for (unsigned i = 0; i < 2; i++) {
        a.calls = 0;
        a.fail = i;
        VkRenderPass pass = VK_NULL_HANDLE;
        EXPECT(vkCreateRenderPass(device, &ri, &cb, &pass) == VK_ERROR_OUT_OF_HOST_MEMORY &&
                   pass == VK_NULL_HANDLE && a.live == 0,
               "render-pass allocation failure rolls back its subpass storage");
    }
    a.fail = UINT32_MAX;
    VkRenderPass empty;
    VK(vkCreateRenderPass(device, &ri, &cb, &empty));
    vkDestroyRenderPass(device, empty, &cb);
    EXPECT(a.live == 0, "attachment-free render pass releases all allocator-owned storage");
    VkDescriptorSetLayoutBinding binding = {.binding = 0,
                                            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                            .descriptorCount = 1,
                                            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT};
    VkDescriptorSetLayoutCreateInfo li = {.sType =
                                              VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                          .bindingCount = 1,
                                          .pBindings = &binding};
    VkDescriptorSetLayout layouts[2];
    VK(vkCreateDescriptorSetLayout(device, &li, &cb, &layouts[0]));
    layouts[1] = layouts[0];
    VkDescriptorPoolSize size = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2};
    VkDescriptorPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                     .maxSets = 2,
                                     .poolSizeCount = 1,
                                     .pPoolSizes = &size};
    VkDescriptorPool dp;
    VK(vkCreateDescriptorPool(device, &pi, &cb, &dp));
    unsigned baseline = a.live;
    a.fail = a.calls + 1;
    VkDescriptorSetAllocateInfo di = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                      .descriptorPool = dp,
                                      .descriptorSetCount = 2,
                                      .pSetLayouts = layouts};
    VkDescriptorSet sets[2];
    EXPECT(vkAllocateDescriptorSets(device, &di, sets) == VK_ERROR_OUT_OF_HOST_MEMORY &&
               sets[0] == VK_NULL_HANDLE && sets[1] == VK_NULL_HANDLE && a.live == baseline,
           "descriptor batch failure rolls back allocations and capacity");
    a.fail = UINT32_MAX;
    VK(vkAllocateDescriptorSets(device, &di, sets));
    vkDestroyDescriptorSetLayout(device, layouts[0], &cb);
    vkDestroyDescriptorPool(device, dp, &cb);
    EXPECT(a.live == 0, "layout storage survives until descriptor pool destruction");
    VkCommandPool cp;
    VkCommandPoolCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                  .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
    VK(vkCreateCommandPool(device, &ci, &cb, &cp));
    VkCommandBuffer c;
    VkCommandBufferAllocateInfo ca = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                      .commandPool = cp,
                                      .commandBufferCount = 1,
                                      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY};
    VK(vkAllocateCommandBuffers(device, &ca, &c));
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    VK(vkBeginCommandBuffer(c, &bi));
    struct buffer b = buffer(16, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    a.fail = a.calls;
    vkCmdFillBuffer(c, b.handle, 0, 16, 1);
    EXPECT(vkEndCommandBuffer(c) == VK_ERROR_OUT_OF_HOST_MEMORY,
           "recording allocation failure reaches vkEndCommandBuffer");
    vkDestroyCommandPool(device, cp, &cb);
    free_buffer(b);
    EXPECT(a.live == 0, "command allocation callbacks balance");
}
struct event_thread {
    VkDevice device;
    VkEvent event;
};
static void *signal_event(void *data)
{
    struct event_thread *p = data;
    usleep(20000);
    vkSetEvent(p->device, p->event);
    return NULL;
}
static void synchronization_tests(void)
{
    VkSemaphore semaphore;
    VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VK(vkCreateSemaphore(device, &si, NULL, &semaphore));
    VkSubmitInfo signal = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                           .signalSemaphoreCount = 1,
                           .pSignalSemaphores = &semaphore};
    VK(vkQueueSubmit(queue, 1, &signal, VK_NULL_HANDLE));
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo wait = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                         .waitSemaphoreCount = 1,
                         .pWaitSemaphores = &semaphore,
                         .pWaitDstStageMask = &stage};
    VK(vkQueueSubmit(queue, 1, &wait, VK_NULL_HANDLE));
    VK(vkQueueWaitIdle(queue));
    VK(vkQueueSubmit(queue, 1, &signal, VK_NULL_HANDLE));
    VK(vkQueueSubmit(queue, 1, &wait, VK_NULL_HANDLE));
    VK(vkQueueWaitIdle(queue));
    EXPECT(1, "binary semaphore signals can be consumed and reused in queue order");
    vkDestroySemaphore(device, semaphore, NULL);
    VkEvent event;
    VkEventCreateInfo ei = {.sType = VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
    VK(vkCreateEvent(device, &ei, NULL, &event));
    EXPECT(vkGetEventStatus(device, event) == VK_EVENT_RESET, "event starts reset");
    begin();
    vkCmdSetEvent(command, event, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    EXPECT(vkGetEventStatus(device, event) == VK_EVENT_RESET, "recorded event remains deferred");
    submit();
    EXPECT(vkGetEventStatus(device, event) == VK_EVENT_SET, "command execution signals event");
    VK(vkResetEvent(device, event));
    EXPECT(vkGetEventStatus(device, event) == VK_EVENT_RESET, "host event reset");
    VkCommandBufferBeginInfo simultaneous = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                              .flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT};
    VK(vkBeginCommandBuffer(command, &simultaneous));
    vkCmdWaitEvents(command, 1, &event, VK_PIPELINE_STAGE_HOST_BIT,
                    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, NULL, 0, NULL, 0, NULL);
    VK(vkEndCommandBuffer(command));
    VK(vkResetFences(device, 1, &fence));
    VkSubmitInfo pending = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                            .commandBufferCount = 1, .pCommandBuffers = &command};
    VK(vkQueueSubmit(queue, 1, &pending, VK_NULL_HANDLE));
    VK(vkQueueSubmit(queue, 1, &pending, fence));
    EXPECT(vkWaitForFences(device, 1, &fence, VK_TRUE, 1000000) == VK_TIMEOUT,
           "queue submission returns while a host event is still pending");
    VK(vkSetEvent(device, event));
    VK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
    EXPECT(vkGetFenceStatus(device, fence) == VK_SUCCESS,
           "simultaneous command submissions complete after the submitting thread signals their event");
    VK(vkResetEvent(device, event));
    struct event_thread event_signal = {device, event};
    pthread_t thread;
    pthread_create(&thread, NULL, signal_event, &event_signal);
    begin();
    vkCmdWaitEvents(command, 1, &event, VK_PIPELINE_STAGE_HOST_BIT,
                    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, NULL, 0, NULL, 0, NULL);
    submit();
    pthread_join(thread, NULL);
    EXPECT(vkGetEventStatus(device, event) == VK_EVENT_SET,
           "recorded event wait wakes for a concurrent host signal");
    vkDestroyEvent(device, event, NULL);
    struct buffer b = buffer(16, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    VkCommandBuffer secondary;
    VkCommandBufferAllocateInfo ca = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                      .commandPool = pool,
                                      .level = VK_COMMAND_BUFFER_LEVEL_SECONDARY,
                                      .commandBufferCount = 1};
    VK(vkAllocateCommandBuffers(device, &ca, &secondary));
    VkCommandBufferInheritanceInfo inheritance = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO};
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .pInheritanceInfo = &inheritance};
    VK(vkBeginCommandBuffer(secondary, &bi));
    vkCmdFillBuffer(secondary, b.handle, 0, 16, 0xaabbccdd);
    VK(vkEndCommandBuffer(secondary));
    begin();
    vkCmdExecuteCommands(command, 1, &secondary);
    submit();
    EXPECT(((uint32_t *)b.map)[3] == 0xaabbccdd, "secondary transfer command execution");
    vkFreeCommandBuffers(device, pool, 1, &secondary);
    free_buffer(b);
}
static void subpasses(int depth, uint32_t layers, int secondary)
{
    struct image a = image_levels(32, 32, depth ? VK_FORMAT_D32_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM,
                                  (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                                         : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) |
                                      VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
                                  1, layers, 0),
                 b = image_levels(32, 32, VK_FORMAT_R8G8B8A8_UNORM,
                                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                      VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
                                  1, layers, 0),
                 out = image_levels(32, 32, VK_FORMAT_R8G8B8A8_UNORM,
                                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                    1, layers, 0);
    struct buffer rb = buffer(16 + 40 * 32 * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    VkAttachmentDescription attachments[3];
    for (unsigned i = 0; i < 3; i++)
        attachments[i] = (VkAttachmentDescription){
            .format = VK_FORMAT_R8G8B8A8_UNORM,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .finalLayout = i == 2 ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
                                  : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkAttachmentReference colors[3] = {{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
                                       {1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
                                       {2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}};
    VkAttachmentReference inputs[2] = {{0, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                       {1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    uint32_t preserved = 0;
    VkSubpassDescription sub[3] = {{.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    .colorAttachmentCount = 1,
                                    .pColorAttachments = colors},
                                   {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    .colorAttachmentCount = 1,
                                    .pColorAttachments = colors + 1,
                                    .preserveAttachmentCount = 1,
                                    .pPreserveAttachments = &preserved},
                                   {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    .colorAttachmentCount = 1,
                                    .pColorAttachments = colors + 2,
                                    .inputAttachmentCount = 2,
                                    .pInputAttachments = inputs}};
    VkAttachmentReference depth_ref = {0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    if (depth) {
        attachments[0].format = VK_FORMAT_D32_SFLOAT;
        sub[0].pColorAttachments = colors + 1;
        sub[0].pDepthStencilAttachment = &depth_ref;
    }
    VkSubpassDependency deps[5] = {
        {.srcSubpass = VK_SUBPASS_EXTERNAL,
         .dstSubpass = 0,
         .srcStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
         .dstStageMask = VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
         .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
         .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT},
        {.srcSubpass = 0, .dstSubpass = 1},
        {.srcSubpass = 0, .dstSubpass = 2},
        {.srcSubpass = 1, .dstSubpass = 2},
        {.srcSubpass = 2,
         .dstSubpass = VK_SUBPASS_EXTERNAL,
         .srcStageMask = VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
         .dstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
         .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
         .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT}};
    for (unsigned i = 1; i < 4; i++) {
        deps[i].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                               VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                               VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        deps[i].dstStageMask = deps[i].srcStageMask | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        deps[i].srcAccessMask =
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        deps[i].dstAccessMask = deps[i].srcAccessMask | VK_ACCESS_INPUT_ATTACHMENT_READ_BIT;
        deps[i].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
    }
    VkRenderPassCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                 .attachmentCount = 3,
                                 .pAttachments = attachments,
                                 .subpassCount = 3,
                                 .pSubpasses = sub,
                                 .dependencyCount = 5,
                                 .pDependencies = deps};
    VkRenderPass pass;
    VK(vkCreateRenderPass(device, &ri, NULL, &pass));
    VkImageView views[3] = {a.view, b.view, out.view};
    VkFramebufferCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                  .renderPass = pass,
                                  .attachmentCount = 3,
                                  .pAttachments = views,
                                  .width = 32,
                                  .height = 32,
                                  .layers = layers};
    VkFramebuffer fb;
    VK(vkCreateFramebuffer(device, &fi, NULL, &fb));
    VkDescriptorSetLayoutBinding bindings[2] = {
        {0, VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo di = {.sType =
                                              VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                          .bindingCount = 2,
                                          .pBindings = bindings};
    VkDescriptorSetLayout set_layout;
    VK(vkCreateDescriptorSetLayout(device, &di, NULL, &set_layout));
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                     .setLayoutCount = 1,
                                     .pSetLayouts = &set_layout};
    VkPipelineLayout layout;
    VK(vkCreatePipelineLayout(device, &li, NULL, &layout));
    VkPipeline first = pipeline_shaders(builtin_vert, sizeof builtin_vert, NULL, pass, layout,
                                        triangle_frag, sizeof triangle_frag, 0, depth, 0);
    pipeline_subpass = 2;
    VkPipeline last =
        pipeline_shaders(builtin_vert, sizeof builtin_vert, NULL, pass, layout,
                         depth ? subpass_depth_frag : subpass_frag,
                         depth ? sizeof subpass_depth_frag : sizeof subpass_frag, 0, 0, 0);
    pipeline_subpass = 0;
    VkDescriptorPoolSize capacity = {VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 2};
    VkDescriptorPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                     .maxSets = 1,
                                     .poolSizeCount = 1,
                                     .pPoolSizes = &capacity};
    VkDescriptorPool dp;
    VK(vkCreateDescriptorPool(device, &pi, NULL, &dp));
    VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                      .descriptorPool = dp,
                                      .descriptorSetCount = 1,
                                      .pSetLayouts = &set_layout};
    VkDescriptorSet set;
    VK(vkAllocateDescriptorSets(device, &ai, &set));
    VkDescriptorImageInfo images[2] = {
        {VK_NULL_HANDLE, a.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {VK_NULL_HANDLE, b.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    VkWriteDescriptorSet writes[2];
    for (unsigned i = 0; i < 2; i++)
        writes[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                           .dstSet = set,
                                           .dstBinding = i,
                                           .descriptorCount = 1,
                                           .descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,
                                           .pImageInfo = images + i};
    vkUpdateDescriptorSets(device, 2, writes, 0, NULL);
    VkClearValue clear[3] = {{{{.25, .5, .75, 1}}}, {{{.1, .2, .3, 1}}}, {{{0, 0, 0, 1}}}};
    if (depth)
        clear[0].depthStencil = (VkClearDepthStencilValue){1, 0};
    VkRenderPassBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                .renderPass = pass,
                                .framebuffer = fb,
                                .renderArea = {{0, 0}, {32, 32}},
                                .clearValueCount = 3,
                                .pClearValues = clear};
#ifndef ZVK_LOADER
    begin();
    vkCmdBeginRenderPass(command, &bi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdEndRenderPass(command);
    EXPECT(vkEndCommandBuffer(command) != VK_SUCCESS,
           "render pass cannot end before its final subpass");
#endif
    VkCommandBuffer children[2] = {0};
    VkViewport vp = {0, 0, 32, 32, 0, 1};
    VkRect2D sc = {{0, 0}, {32, 32}};
    if (secondary) {
        VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                          .commandPool = pool,
                                          .level = VK_COMMAND_BUFFER_LEVEL_SECONDARY,
                                          .commandBufferCount = 2};
        VK(vkAllocateCommandBuffers(device, &ai, children));
        for (unsigned i = 0; i < 2; i++) {
            VkCommandBufferInheritanceInfo inheritance = {
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO,
                .renderPass = pass, .subpass = i ? 2 : 0,
                .framebuffer = i ? VK_NULL_HANDLE : fb};
            VkCommandBufferBeginInfo ci = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                           .flags = VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT,
                                           .pInheritanceInfo = &inheritance};
            VK(vkBeginCommandBuffer(children[i], &ci));
            vkCmdSetViewport(children[i], 0, 1, &vp);
            vkCmdSetScissor(children[i], 0, 1, &sc);
            vkCmdBindPipeline(children[i], VK_PIPELINE_BIND_POINT_GRAPHICS, i ? last : first);
            if (i)
                vkCmdBindDescriptorSets(children[i], VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        layout, 0, 1, &set, 0, NULL);
            vkCmdDraw(children[i], 3, 1, 0, 0);
            VK(vkEndCommandBuffer(children[i]));
        }
    }
    begin();
    vkCmdBeginRenderPass(command, &bi, secondary ? VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS
                                                : VK_SUBPASS_CONTENTS_INLINE);
    if (secondary)
        vkCmdExecuteCommands(command, 1, children);
    else {
        vkCmdSetViewport(command, 0, 1, &vp);
        vkCmdSetScissor(command, 0, 1, &sc);
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, first);
        vkCmdDraw(command, 3, 1, 0, 0);
    }
    vkCmdNextSubpass(command, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdNextSubpass(command, secondary ? VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS
                                       : VK_SUBPASS_CONTENTS_INLINE);
    if (secondary)
        vkCmdExecuteCommands(command, 1, children + 1);
    else {
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, last);
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, NULL);
        vkCmdDraw(command, 3, 1, 0, 0);
    }
    vkCmdEndRenderPass(command);
    readback(out.handle, rb);
    submit();
    EXPECT(pixel(rb, 16, 16, depth ? 128 : 6, depth ? 64 : 13, depth ? 64 : 147, 2),
           depth ? "subpass depth input reads a prior depth write"
                 : "subpass inputs read preserved color and late load clear");
    EXPECT(pixel(rb, 0, 0, 0, 0, 0, 0), "final subpass load clear stays outside drawing");
    // Repeating the recorded pass must reapply first-use load operations.
    VK(vkResetFences(device, 1, &fence));
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                       .commandBufferCount = 1,
                       .pCommandBuffers = &command};
    VK(vkQueueSubmit(queue, 1, &si, fence));
    VK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
    EXPECT(pixel(rb, 16, 16, depth ? 128 : 6, depth ? 64 : 13, depth ? 64 : 147, 2),
           "multiple subpasses support command-buffer resubmission");
    if (secondary)
        vkFreeCommandBuffers(device, pool, 2, children);
    vkDestroyDescriptorPool(device, dp, NULL);
    vkDestroyPipeline(device, last, NULL);
    vkDestroyPipeline(device, first, NULL);
    vkDestroyPipelineLayout(device, layout, NULL);
    vkDestroyDescriptorSetLayout(device, set_layout, NULL);
    vkDestroyFramebuffer(device, fb, NULL);
    vkDestroyRenderPass(device, pass, NULL);
    free_buffer(rb);
    free_image(out);
    free_image(b);
    free_image(a);
}
static void unused_attachment_test(void)
{
    struct image source=image(32,32,VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    struct buffer result=buffer(16+40*32*4,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    VkAttachmentDescription attachment={.format=VK_FORMAT_R8G8B8A8_UNORM,.samples=VK_SAMPLE_COUNT_1_BIT,
        .loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR,.storeOp=VK_ATTACHMENT_STORE_OP_STORE,
        .initialLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,.finalLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
    VkAttachmentReference unused[4];
    for(unsigned i=0;i<4;i++)unused[i]=(VkAttachmentReference){VK_ATTACHMENT_UNUSED,VK_IMAGE_LAYOUT_UNDEFINED};
    VkSubpassDescription subpass={.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount=4,.pColorAttachments=unused};
    VkRenderPassCreateInfo ri={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount=1,.pAttachments=&attachment,.subpassCount=1,.pSubpasses=&subpass};
    VkRenderPass pass;VK(vkCreateRenderPass(device,&ri,NULL,&pass));
    VkFramebufferCreateInfo fi={.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,.renderPass=pass,
        .attachmentCount=1,.pAttachments=&source.view,.width=32,.height=32,.layers=1};
    VkFramebuffer framebuffer;VK(vkCreateFramebuffer(device,&fi,NULL,&framebuffer));
    begin();
    barrier(source.handle,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearColorValue green={{0,1,0,1}};
    VkImageSubresourceRange range={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    vkCmdClearColorImage(command,source.handle,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&green,1,&range);
    barrier(source.handle,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkClearValue red={.color={{1,0,0,1}}};
    VkRenderPassBeginInfo bi={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,.renderPass=pass,
        .framebuffer=framebuffer,.renderArea={{0,0},{32,32}},.clearValueCount=1,.pClearValues=&red};
    vkCmdBeginRenderPass(command,&bi,VK_SUBPASS_CONTENTS_INLINE);
    VkClearAttachment clears[2]={{.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT,.colorAttachment=2,.clearValue=red},
        {.aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT,.clearValue={.depthStencil={1,255}}}};
    VkClearRect rectangle={.rect={{0,0},{32,32}},.layerCount=1};
    vkCmdClearAttachments(command,2,clears,1,&rectangle);
    vkCmdEndRenderPass(command);
    readback(source.handle,result);submit();
    EXPECT(pixel(result,16,16,0,255,0,0),
           "unused render-pass attachment transitions to final layout without its CLEAR load operation");
    vkDestroyFramebuffer(device,framebuffer,NULL);vkDestroyRenderPass(device,pass,NULL);
    struct image depth=image(32,32,VK_FORMAT_D32_SFLOAT,
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    attachment.format=VK_FORMAT_D32_SFLOAT;attachment.loadOp=VK_ATTACHMENT_LOAD_OP_LOAD;
    attachment.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.initialLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference depth_ref={0,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    subpass.pDepthStencilAttachment=&depth_ref;
    VK(vkCreateRenderPass(device,&ri,NULL,&pass));
    fi.renderPass=pass;fi.pAttachments=&depth.view;
    VK(vkCreateFramebuffer(device,&fi,NULL,&framebuffer));
    begin();
    barrier_aspect(depth.handle,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_ASPECT_DEPTH_BIT);
    VkClearDepthStencilValue quarter={0.25f,0};range.aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT;
    vkCmdClearDepthStencilImage(command,depth.handle,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&quarter,1,&range);
    barrier_aspect(depth.handle,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,VK_IMAGE_ASPECT_DEPTH_BIT);
    VkClearValue one={.depthStencil={1,255}};
    bi.renderPass=pass;bi.framebuffer=framebuffer;bi.pClearValues=&one;
    vkCmdBeginRenderPass(command,&bi,VK_SUBPASS_CONTENTS_INLINE);vkCmdEndRenderPass(command);
    barrier_aspect(depth.handle,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_ASPECT_DEPTH_BIT);
    VkBufferImageCopy copy={.imageSubresource={VK_IMAGE_ASPECT_DEPTH_BIT,0,0,1},.imageExtent={32,32,1}};
    vkCmdCopyImageToBuffer(command,depth.handle,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,result.handle,1,&copy);submit();
    EXPECT(((float *)result.map)[16*32+16]==0.25f,
           "D32 LOAD preserves depth while its nonexistent stencil aspect has a CLEAR load operation");
    vkDestroyFramebuffer(device,framebuffer,NULL);vkDestroyRenderPass(device,pass,NULL);free_image(depth);
    free_buffer(result);free_image(source);
}
static void compute_tests(void)
{
    if (!properties.limits.maxComputeWorkGroupInvocations) {
        puts("SKIP compute: Metal transport unavailable");
        return;
    }
    puts("COMPUTE_BACKEND Metal GPU");
    struct buffer out = buffer(256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                        VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                        VK_BUFFER_USAGE_TRANSFER_SRC_BIT),
                  params = buffer(16, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT),
                  indirect = buffer(16, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                                            VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    ((uint32_t *)params.map)[0] = 7;
    VkDescriptorSetLayoutBinding bindings[2] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo di = {.sType =
                                              VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                          .bindingCount = 2,
                                          .pBindings = bindings};
    VkDescriptorSetLayout set_layout;
    VK(vkCreateDescriptorSetLayout(device, &di, NULL, &set_layout));
    VkPushConstantRange push = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 4};
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                     .setLayoutCount = 1,
                                     .pSetLayouts = &set_layout,
                                     .pushConstantRangeCount = 1,
                                     .pPushConstantRanges = &push};
    VkPipelineLayout layout;
    VK(vkCreatePipelineLayout(device, &li, NULL, &layout));
    VkShaderModule shader = module(buffer_comp, sizeof buffer_comp);
    VkComputePipelineCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                  .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                  .module = shader,
                  .pName = "main"},
        .layout = layout};
    VkPipeline pipeline;
    VK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &ci, NULL, &pipeline));
    vkDestroyShaderModule(device, shader, NULL);
    VkDescriptorPoolSize capacities[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 3},
                                          {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1}};
    VkDescriptorPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                     .maxSets = 2,
                                     .poolSizeCount = 2,
                                     .pPoolSizes = capacities};
    VkDescriptorPool dp;
    VK(vkCreateDescriptorPool(device, &pi, NULL, &dp));
    VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                      .descriptorPool = dp,
                                      .descriptorSetCount = 1,
                                      .pSetLayouts = &set_layout};
    VkDescriptorSet set;
    VK(vkAllocateDescriptorSets(device, &ai, &set));
    VkDescriptorBufferInfo buffers[2] = {{out.handle, 16, 224}, {params.handle, 0, 16}};
    VkWriteDescriptorSet writes[2];
    for (unsigned i = 0; i < 2; i++)
        writes[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                           .dstSet = set,
                                           .dstBinding = i,
                                           .descriptorCount = 1,
                                           .descriptorType = bindings[i].descriptorType,
                                           .pBufferInfo = buffers + i};
    vkUpdateDescriptorSets(device, 2, writes, 0, NULL);
    memset(out.map, 0xa5, 256);
    uint32_t bias = 9, offset = 0;
    begin();
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 1,
                            &offset);
    vkCmdPushConstants(command, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &bias);
    vkCmdDispatch(command, 8, 1, 1);
    VkMemoryBarrier memory_barrier = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                      .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                                      .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &memory_barrier, 0, NULL, 0, NULL);
    VkBufferCopy copy = {16, 0, 16};
    vkCmdCopyBuffer(command, out.handle, indirect.handle, 1, &copy);
    submit();
    uint32_t *values = out.map;
    int ok = 1;
    for (unsigned i = 0; i < 64; i++)
        if (values[i] != (i >= 4 && i < 60 ? (i - 4) * 7 + 9 : 0xa5a5a5a5u))
            ok = 0;
    EXPECT(ok, "GPU compute uses storage range, uniform, push constant and runtime array length");
    EXPECT(((uint32_t *)indirect.map)[0] == 9 && ((uint32_t *)indirect.map)[3] == 30,
           "compute shader writes become visible to a following buffer transfer");
    begin();
    vkCmdFillBuffer(command, out.handle, 0, 256, 0xa5a5a5a5u);
    uint32_t groups[3] = {4, 1, 1};
    vkCmdUpdateBuffer(command, indirect.handle, 4, 12, groups);
    VkMemoryBarrier to_compute = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                  .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                  .dstAccessMask = VK_ACCESS_SHADER_READ_BIT |
                                                   VK_ACCESS_SHADER_WRITE_BIT |
                                                   VK_ACCESS_INDIRECT_COMMAND_READ_BIT};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
                         0, 1, &to_compute, 0, NULL, 0, NULL);
    offset = 16;
    bias = 17;
    ((uint32_t *)params.map)[0] = 3;
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 1,
                            &offset);
    vkCmdPushConstants(command, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &bias);
    vkCmdDispatchIndirect(command, indirect.handle, 4);
    submit();
    ok = 1;
    for (unsigned i = 0; i < 64; i++)
        if (values[i] != (i >= 8 && i < 40 ? (i - 8) * 3 + 17 : 0xa5a5a5a5u))
            ok = 0;
    EXPECT(ok, "indirect compute observes recorded updates and dynamic storage offsets");
    begin();
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 1,
                            &offset);
    vkCmdPushConstants(command, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &bias);
    vkCmdDispatch(command, 0, 1, 1);
    submit();
    EXPECT(values[8] == 17, "zero-sized compute dispatch leaves memory unchanged");
    vkDestroyPipeline(device, pipeline, NULL);
    shader = module(shared_comp, sizeof shared_comp);
    ci.stage.module = shader;
    VK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &ci, NULL, &pipeline));
    vkDestroyShaderModule(device, shader, NULL);
    buffers[0] = (VkDescriptorBufferInfo){out.handle, 0, 256};
    vkUpdateDescriptorSets(device, 1, writes, 0, NULL);
    begin();
    vkCmdFillBuffer(command, out.handle, 0, 256, 0);
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
                         0, 1, &to_compute, 0, NULL, 0, NULL);
    offset = 0;
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 1,
                            &offset);
    vkCmdDispatch(command, 8, 1, 1);
    submit();
    ok = values[0] == 8;
    for (unsigned i = 0; i < 8; i++)
        if (values[i + 1] != 64 * i + 36)
            ok = 0;
    EXPECT(ok, "GPU workgroup shared memory, barrier, control flow and relaxed atomic add");
    vkDestroyPipeline(device, pipeline, NULL);
    shader=module(reconverge_comp,sizeof reconverge_comp);ci.stage.module=shader;
    VK(vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&ci,NULL,&pipeline));
    vkDestroyShaderModule(device,shader,NULL);
    begin();vkCmdFillBuffer(command,out.handle,0,256,0xa5a5a5a5u);
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
        0,1,&to_compute,0,NULL,0,NULL);
    vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);
    vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,1,&set,1,&offset);
    vkCmdDispatch(command,1,1,1);submit();
    ok=values[0]==8;
    for(unsigned i=1;i<64;i++)
        if(values[i]!=(i<=8?54*(i-1)+63:0xa5a5a5a5u))ok=0;
    EXPECT(ok,"GPU barriers reconverge 128 invocations after divergent calls and repeated shared-memory loops");
    vkDestroyPipeline(device,pipeline,NULL);
    // Float vector storage and push constants use the same descriptor state.
    push.size = 16;
    VkPipelineLayout vector_layout;
    VK(vkCreatePipelineLayout(device, &li, NULL, &vector_layout));
    shader = module(vector_comp, sizeof vector_comp);
    ci.stage.module = shader;
    ci.layout = vector_layout;
    VK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &ci, NULL, &pipeline));
    vkDestroyShaderModule(device, shader, NULL);
    float factors[4] = {2, .5, 1, -1}, biases[4] = {.25, .5, 2, 1};
    memcpy(params.map, factors, sizeof factors);
    offset = 0;
    begin();
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, vector_layout, 0, 1, &set, 1,
                            &offset);
    vkCmdPushConstants(command, vector_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 16, biases);
    vkCmdDispatch(command, 4, 1, 1);
    submit();
    ok = 1;
    for (unsigned i = 0; i < 16; i++)
        for (unsigned k = 0; k < 4; k++)
            if (fabsf(((float *)out.map)[i * 4 + k] - (i * factors[k] + biases[k] + k + 1)) > .0001)
                ok = 0;
    EXPECT(ok, "GPU compute preserves float-vector storage and vector push constants");
    vkDestroyPipeline(device, pipeline, NULL);
    vkDestroyPipelineLayout(device, vector_layout, NULL);
    // Scalar specialization is compiled into the native Metal shader.
    uint32_t extra = 11;
    VkSpecializationMapEntry map = {7, 0, 4};
    VkSpecializationInfo spec = {
        .mapEntryCount = 1, .pMapEntries = &map, .dataSize = 4, .pData = &extra};
    shader = module(buffer_comp, sizeof buffer_comp);
    ci.stage.module = shader;
    ci.layout = layout;
    ci.stage.pSpecializationInfo = &spec;
    VK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &ci, NULL, &pipeline));
    vkDestroyShaderModule(device, shader, NULL);
    ((uint32_t *)params.map)[0] = 1;
    bias = 4;
    begin();
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 1,
                            &offset);
    vkCmdPushConstants(command, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &bias);
    vkCmdDispatch(command, 8, 1, 1);
    submit();
    EXPECT(values[0] == 15 && values[63] == 78,
           "compute scalar specialization changes GPU results");
    vkDestroyPipeline(device, pipeline, NULL);
    ci.stage.pSpecializationInfo = NULL;
    push.size = 4;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
    VkDescriptorSetLayout alias_set_layout;
    VK(vkCreateDescriptorSetLayout(device, &di, NULL, &alias_set_layout));
    li.pSetLayouts = &alias_set_layout;
    VkPipelineLayout alias_layout;
    VK(vkCreatePipelineLayout(device, &li, NULL, &alias_layout));
    VkBuffer alias;
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                             .size = 256,
                             .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
    VK(vkCreateBuffer(device, &bi, NULL, &alias));
    VK(vkBindBufferMemory(device, alias, out.memory, 0));
    shader = module(alias_comp, sizeof alias_comp);
    ci.stage.module = shader;
    ci.layout = alias_layout;
    VK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &ci, NULL, &pipeline));
    vkDestroyShaderModule(device, shader, NULL);
    VkDescriptorSet alias_set;
    ai.pSetLayouts = &alias_set_layout;
    VK(vkAllocateDescriptorSets(device, &ai, &alias_set));
    buffers[0] = (VkDescriptorBufferInfo){out.handle, 0, 256};
    buffers[1] = (VkDescriptorBufferInfo){alias, 0, 256};
    for (unsigned i = 0; i < 2; i++) {
        writes[i].dstSet = alias_set;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
    }
    vkUpdateDescriptorSets(device, 2, writes, 0, NULL);
    uint32_t offsets[2] = {0, 0};
    begin();
    vkCmdFillBuffer(command, out.handle, 0, 256, 0);
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
                         0, 1, &to_compute, 0, NULL, 0, NULL);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, alias_layout, 0, 1, &alias_set,
                            2, offsets);
    vkCmdDispatch(command, 1, 1, 1);
    submit();
    EXPECT(values[0] == 41 && values[1] == 42 && values[2] == 12,
           "distinct storage buffers aliasing one allocation share GPU writes and atomics");
    vkDestroyPipeline(device, pipeline, NULL);
    vkDestroyBuffer(device, alias, NULL);
    vkDestroyPipelineLayout(device, alias_layout, NULL);
    vkDestroyDescriptorSetLayout(device, alias_set_layout, NULL);
    pipeline = VK_NULL_HANDLE;
    puts("METAL_COMPUTE_EXECUTION_PASS");
    vkDestroyPipeline(device, pipeline, NULL);
    vkDestroyDescriptorPool(device, dp, NULL);
    vkDestroyPipelineLayout(device, layout, NULL);
    vkDestroyDescriptorSetLayout(device, set_layout, NULL);
    free_buffer(indirect);
    free_buffer(params);
    free_buffer(out);
}
#include "storage_tests.h"
#include "layered_tests.h"
#include "format_tests.h"
#include "depth_bias_tests.h"
#include "robust_tests.h"
#include "ms_tests.h"
#include "mutable_tests.h"
#include "capacity_tests.h"
#include "texture_operand_tests.h"
#include "stencil_tests.h"
#include "samplers_tests.h"
#include "atlas_tests.h"
#include "pulling_tests.h"
int main(int argc, char **argv)
{
    discovery();
    if (argc > 1 && !strcmp(argv[1], "--mutable")) {
        mutable_tests();
        goto finish;
    }
    if (argc > 1 && !strcmp(argv[1], "--graphics")) {
        graphics();
        fixed_depth_bias_tests();
        goto finish;
    }
    if (argc > 1 && !strcmp(argv[1], "--atlas")) {
        atlas_tests();
        pulling_tests();
        goto finish;
    }
    if (argc > 1 && !strcmp(argv[1], "--samplers")) {
        samplers_tests();
        queue_priority_tests();
        goto finish;
    }
    if (argc > 1 && !strcmp(argv[1], "--stencil")) {
        stencil_tests();
        goto finish;
    }
    if (argc > 1 && !strcmp(argv[1], "--textures")) {
        texture_operand_tests();
        goto finish;
    }
    if (argc > 1 && !strcmp(argv[1], "--capacity")) {
        capacity_tests();
        goto finish;
    }
    if (argc > 1 && !strcmp(argv[1], "--multisample")) {
        samplers_tests();
        queue_priority_tests();
        stencil_tests();
        ms_tests();
        ms_empty_tests();
        ms_depth_coverage_tests();
        mutable_tests();
        texture_operand_tests();
        goto finish;
    }
    if (argc > 1 && !strcmp(argv[1], "--robust")) {
        robust_tests();
        goto finish;
    }
    if (argc > 1 && !strcmp(argv[1], "--dimensions")) {
        only_dimension_tests = 1;
        format_tests();
        goto finish;
    }
    if (argc > 1 && !strcmp(argv[1], "--formats")) {
        format_tests();
        goto finish;
    }
    if (argc > 1 && !strcmp(argv[1], "--compute")) {
        compute_tests();
        storage_image_tests();
        storage_array_tests();
        storage_clear_tests();
        texel_buffer_tests();
        compute_binding_limit_tests();
        capacity_tests();
        compute_function_tests();
        compute_aggregate_math_tests();
        goto finish;
    }
    if (argc > 1 && !strcmp(argv[1], "--subpasses")) {
        unused_attachment_test();
        subpasses(0, 1, 0);
        subpasses(0, 1, 1);
        subpasses(1, 1, 0);
        subpasses(1, 1, 1);
        subpasses(0, 4, 0);
        subpasses(0, 4, 1);
        subpasses(1, 4, 0);
        subpasses(1, 4, 1);
        layered_framebuffer_tests();
    } else {
        transfers();
        robust_tests();
        compute_tests();
        storage_image_tests();
        storage_array_tests();
        storage_clear_tests();
        texel_buffer_tests();
        compute_binding_limit_tests();
        capacity_tests();
        compute_function_tests();
        compute_aggregate_math_tests();
        graphics();
        fixed_depth_bias_tests();
        color_targets();
        unused_attachment_test();
        subpasses(0, 1, 0);
        subpasses(0, 1, 1);
        subpasses(1, 1, 0);
        subpasses(1, 1, 1);
        subpasses(0, 4, 0);
        subpasses(0, 4, 1);
        subpasses(1, 4, 0);
        subpasses(1, 4, 1);
        layered_framebuffer_tests();
        texture_extensions();
        format_tests();
        multisampling();
        atlas_tests();
        pulling_tests();
        samplers_tests();
        queue_priority_tests();
        stencil_tests();
        ms_tests();
        ms_empty_tests();
        ms_depth_coverage_tests();
        mutable_tests();
        texture_operand_tests();
        mip_transfers();
        packed_depth_transfers();
        allocation_tests();
        synchronization_tests();
    }
finish:
    VK(vkDeviceWaitIdle(device));
    vkDestroyFence(device, fence, NULL);
    vkDestroyCommandPool(device, pool, NULL);
    vkDestroyDevice(device, NULL);
#ifdef ZVK_LOADER
    PFN_vkDestroyDebugUtilsMessengerEXT destroy_debug =
        (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
            instance, "vkDestroyDebugUtilsMessengerEXT");
    destroy_debug(instance, debug_messenger, NULL);
    EXPECT(validation_errors == 0, "Khronos core and synchronization validation report no errors");
#endif
    vkDestroyInstance(instance, NULL);
    printf("ZVK_RESULT %u passed %u failed\n", passed, failed);
    return failed ? 1 : 0;
}
