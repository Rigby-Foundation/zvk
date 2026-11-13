/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (C) 2026 Rigby Foundation */
/* Public Vulkan only: no zVk, virgl or Metal API dependency in the application. */
#include <vulkan/vulkan.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "shaders.h"
#define VK(call)                                                                                   \
    do {                                                                                           \
        VkResult r = (call);                                                                       \
        if (r != VK_SUCCESS) {                                                                     \
            fprintf(stderr, "%s: %d\n", #call, r);                                                 \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)
int main(void)
{
    VkInstance instance;
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                             .pApplicationName = "sic compute",
                             .apiVersion = VK_API_VERSION_1_0};
    VkInstanceCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                               .pApplicationInfo = &app};
    VK(vkCreateInstance(&ii, NULL, &instance));
    VkPhysicalDevice physical;
    uint32_t count = 1;
    VK(vkEnumeratePhysicalDevices(instance, &count, &physical));
    if (!count) {
        fprintf(stderr, "No Vulkan device\n");
        return 1;
    }
    VkQueueFamilyProperties family;
    count = 1;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, &family);
    if (!(family.queueFlags & VK_QUEUE_COMPUTE_BIT)) {
        fprintf(stderr, "Compute unavailable on this backend\n");
        vkDestroyInstance(instance, NULL);
        return 77;
    }
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(physical, &properties);
    printf("Compute device: %s\n", properties.deviceName);
    float priority = 1;
    VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                  .queueFamilyIndex = 0,
                                  .queueCount = 1,
                                  .pQueuePriorities = &priority};
    VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                             .queueCreateInfoCount = 1,
                             .pQueueCreateInfos = &qi};
    VkDevice device;
    VK(vkCreateDevice(physical, &di, NULL, &device));
    VkQueue queue;
    vkGetDeviceQueue(device, 0, 0, &queue);
    VkDeviceSize align = properties.limits.minUniformBufferOffsetAlignment;
    VkDeviceSize uniform = (256 + align - 1) / align * align;
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                             .size = uniform + 16,
                             .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                      VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT};
    VkBuffer buffer;
    VK(vkCreateBuffer(device, &bi, NULL, &buffer));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, buffer, &req);
    VkPhysicalDeviceMemoryProperties memory_properties;
    vkGetPhysicalDeviceMemoryProperties(physical, &memory_properties);
    uint32_t type = UINT32_MAX;
    for (unsigned i = 0; i < memory_properties.memoryTypeCount; i++)
        if ((req.memoryTypeBits & (1u << i)) &&
            (memory_properties.memoryTypes[i].propertyFlags &
             (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            type = i;
            break;
        }
    if (type == UINT32_MAX) {
        fprintf(stderr, "No coherent mapped memory\n");
        return 1;
    }
    VkMemoryAllocateInfo mi = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                               .allocationSize = req.size,
                               .memoryTypeIndex = type};
    VkDeviceMemory memory;
    VK(vkAllocateMemory(device, &mi, NULL, &memory));
    VK(vkBindBufferMemory(device, buffer, memory, 0));
    uint8_t *data;
    VK(vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, (void **)&data));
    memset(data, 0, req.size);
    uint32_t factor = 7;
    memcpy(data + uniform, &factor, 4);
    VkDescriptorSetLayoutBinding bindings[2] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo si = {.sType =
                                              VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                          .bindingCount = 2,
                                          .pBindings = bindings};
    VkDescriptorSetLayout set_layout;
    VK(vkCreateDescriptorSetLayout(device, &si, NULL, &set_layout));
    VkPushConstantRange push = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 4};
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                     .setLayoutCount = 1,
                                     .pSetLayouts = &set_layout,
                                     .pushConstantRangeCount = 1,
                                     .pPushConstantRanges = &push};
    VkPipelineLayout layout;
    VK(vkCreatePipelineLayout(device, &li, NULL, &layout));
    VkShaderModuleCreateInfo smi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                    .codeSize = sizeof buffer_comp,
                                    .pCode = buffer_comp};
    VkShaderModule shader;
    VK(vkCreateShaderModule(device, &smi, NULL, &shader));
    VkComputePipelineCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                  .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                  .module = shader,
                  .pName = "main"},
        .layout = layout};
    VkPipeline pipeline;
    VK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &ci, NULL, &pipeline));
    VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1},
                                     {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1}};
    VkDescriptorPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                     .maxSets = 1,
                                     .poolSizeCount = 2,
                                     .pPoolSizes = sizes};
    VkDescriptorPool pool;
    VK(vkCreateDescriptorPool(device, &pi, NULL, &pool));
    VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                      .descriptorPool = pool,
                                      .descriptorSetCount = 1,
                                      .pSetLayouts = &set_layout};
    VkDescriptorSet set;
    VK(vkAllocateDescriptorSets(device, &ai, &set));
    VkDescriptorBufferInfo infos[2] = {{buffer, 0, 256}, {buffer, uniform, 16}};
    VkWriteDescriptorSet writes[2];
    for (unsigned i = 0; i < 2; i++)
        writes[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                           .dstSet = set,
                                           .dstBinding = i,
                                           .descriptorCount = 1,
                                           .descriptorType = bindings[i].descriptorType,
                                           .pBufferInfo = infos + i};
    vkUpdateDescriptorSets(device, 2, writes, 0, NULL);
    VkCommandPoolCreateInfo cpi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                   .queueFamilyIndex = 0};
    VkCommandPool command_pool;
    VK(vkCreateCommandPool(device, &cpi, NULL, &command_pool));
    VkCommandBufferAllocateInfo ca = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                      .commandPool = command_pool,
                                      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                      .commandBufferCount = 1};
    VkCommandBuffer command;
    VK(vkAllocateCommandBuffers(device, &ca, &command));
    VkCommandBufferBeginInfo cb = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    VK(vkBeginCommandBuffer(command, &cb));
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, NULL);
    uint32_t bias = 9;
    vkCmdPushConstants(command, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &bias);
    vkCmdDispatch(command, 8, 1, 1);
    VkMemoryBarrier host = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                            .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 1, &host, 0, NULL, 0, NULL);
    VK(vkEndCommandBuffer(command));
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                           .commandBufferCount = 1,
                           .pCommandBuffers = &command};
    VK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
    VK(vkQueueWaitIdle(queue));
    for (unsigned i = 0; i < 64; i++)
        if (((uint32_t *)data)[i] != i * 7 + 9) {
            fprintf(stderr, "GPU compute mismatch at %u\n", i);
            return 1;
        }
    puts("COMPUTE_DEMO_PASS 64 GPU results verified");
    vkDestroyCommandPool(device, command_pool, NULL);
    vkDestroyDescriptorPool(device, pool, NULL);
    vkDestroyPipeline(device, pipeline, NULL);
    vkDestroyShaderModule(device, shader, NULL);
    vkDestroyPipelineLayout(device, layout, NULL);
    vkDestroyDescriptorSetLayout(device, set_layout, NULL);
    vkUnmapMemory(device, memory);
    vkDestroyBuffer(device, buffer, NULL);
    vkFreeMemory(device, memory, NULL);
    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);
    return 0;
}
