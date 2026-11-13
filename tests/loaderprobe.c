/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (C) 2026 Rigby Foundation */
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <string.h>
int main(int argc, char **argv)
{
    int missing = argc == 2 && !strcmp(argv[1], "--missing-driver");
    VkInstanceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    VkInstance instance = VK_NULL_HANDLE;
    VkResult result = vkCreateInstance(&ci, NULL, &instance);
    if (missing) {
        if (result != VK_ERROR_INCOMPATIBLE_DRIVER || instance != VK_NULL_HANDLE)
            return 1;
        printf("LOADER_MISSING_DRIVER_PASS\n");
        return 0;
    }
    if (result != VK_SUCCESS) {
        fprintf(stderr, "loader vkCreateInstance: %d\n", result);
        return 1;
    }
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    uint32_t count = 1;
    result = vkEnumeratePhysicalDevices(instance, &count, &physical);
    VkPhysicalDeviceProperties properties = {0};
    if (result == VK_SUCCESS && count == 1)
        vkGetPhysicalDeviceProperties(physical, &properties);
    int good = result == VK_SUCCESS && count == 1 &&
               !strncmp(properties.deviceName, "zvk experimental", 16) &&
               !vkGetInstanceProcAddr(instance, "vkCreateZwmSurfaceSIC");
    vkDestroyInstance(instance, NULL);
    printf("LOADER_DISCOVERY_%s\n", good ? "PASS" : "FAIL");
    return good ? 0 : 1;
}
