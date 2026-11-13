/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (C) 2026 Rigby Foundation */
#pragma once
#include <vulkan/vulkan.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Diagnostic for the last error on the calling thread (including SPIR-V).
 * No extra API is needed to render: use the Vulkan entry points normally. */
const char *zvkGetLastError(void);
#ifdef __cplusplus
}
#endif
