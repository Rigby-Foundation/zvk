/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (C) 2026 Rigby Foundation */
#pragma once
#include <zvk.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Experimental sic platform API. This extension name/structure number is
 * private to sic; it is not a registered Khronos platform extension. The
 * application retains the connection/window until after surface destruction.
 * Serialize all libzwm calls on this connection, including Vulkan WSI calls. */
typedef struct zwm zwm;
#define VK_SIC_ZWM_SURFACE_EXTENSION_NAME "VK_SIC_zwm_surface"
#define VK_SIC_ZWM_SURFACE_SPEC_VERSION 1
#define VK_STRUCTURE_TYPE_ZWM_SURFACE_CREATE_INFO_SIC ((VkStructureType)1000999000)
typedef struct VkZwmSurfaceCreateInfoSIC {
    VkStructureType sType;
    const void *pNext;
    VkFlags flags;
    zwm *connection;
    uint32_t window;
} VkZwmSurfaceCreateInfoSIC;
typedef VkResult(VKAPI_PTR *PFN_vkCreateZwmSurfaceSIC)(VkInstance,
                                                       const VkZwmSurfaceCreateInfoSIC *,
                                                       const VkAllocationCallbacks *,
                                                       VkSurfaceKHR *);
VKAPI_ATTR VkResult VKAPI_CALL vkCreateZwmSurfaceSIC(VkInstance, const VkZwmSurfaceCreateInfoSIC *,
                                                     const VkAllocationCallbacks *, VkSurfaceKHR *);
#ifdef __cplusplus
}
#endif
