/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (C) 2026 Rigby Foundation */
#include "zvk_internal.h"
enum scope { GLOBAL, INSTANCE, DEVICE };
struct entry {
    const char *name;
    PFN_vkVoidFunction function;
    enum scope scope;
    uint32_t extension;
};
static const struct entry entries[] = {
#include "procs.inc"
};
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance,
                                                               const char *name)
{
    if (!name)
        return NULL;
    for (size_t i = 0; i < sizeof entries / sizeof *entries; i++)
        if (!strcmp(name, entries[i].name))
            return entries[i].scope == GLOBAL ||
                           (instance && (!entries[i].extension || entries[i].scope == DEVICE ||
                                         (instance->extensions & entries[i].extension)))
                       ? entries[i].function
                       : NULL;
    return NULL;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char *name)
{
    if (!device || !name)
        return NULL;
    for (size_t i = 0; i < sizeof entries / sizeof *entries; i++)
        if (entries[i].scope == DEVICE && !strcmp(name, entries[i].name) &&
            (!entries[i].extension || (device->extensions & entries[i].extension)))
            return entries[i].function;
    return NULL;
}
VKAPI_ATTR VkResult VKAPI_CALL vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t *version)
{
    if (!version)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (*version > 5)
        *version = 5;
    return VK_SUCCESS;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance,
                                                                   const char *name)
{
    return vkGetInstanceProcAddr(instance, name);
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetPhysicalDeviceProcAddr(VkInstance instance,
                                                                         const char *name)
{
    if (!instance || !name)
        return NULL;
    return !strncmp(name, "vkGetPhysicalDevice", 19) ||
                   !strcmp(name, "vkEnumerateDeviceExtensionProperties") ||
                   !strcmp(name, "vkEnumerateDeviceLayerProperties")
               ? vkGetInstanceProcAddr(instance, name)
               : NULL;
}
