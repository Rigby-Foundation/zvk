# SPDX-License-Identifier: GPL-2.0-only
"""Backport device-list lookup synchronization to the pinned Khronos loader.

The old lookup takes instance_list_lock while device creation/destruction use
loader_lock. A concurrent GetDeviceProcAddr can then dereference a freed device.
Current upstream serializes this lookup with loader_lock; make that lock
recursive because the 1.3.296 trampolines already hold it when invoking lookup.
https://github.com/KhronosGroup/Vulkan-Loader/blob/main/loader/loader.c
"""
from pathlib import Path


def apply(source):
    path = Path(source) / 'loader/loader.c'
    text = path.read_text()
    start = text.index('struct loader_icd_term *loader_get_icd_and_device(')
    end = text.index('\nvoid loader_destroy_logical_device(', start)
    lookup = text[start:end].replace('&loader_global_instance_list_lock', '&loader_lock')
    text = text[:start] + lookup + text[end:]
    marker = '    loader_platform_thread_create_mutex(&loader_lock);'
    replacement = '''    /* Match the device-list mutation lock during concurrent GPA lookups. */
#if COMMON_UNIX_PLATFORMS
    pthread_mutexattr_t attributes;
    pthread_mutexattr_init(&attributes);
    pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&loader_lock, &attributes);
    pthread_mutexattr_destroy(&attributes);
#else
    loader_platform_thread_create_mutex(&loader_lock);
#endif'''
    if 'pthread_mutex_init(&loader_lock, &attributes);' not in text:
        if text.count(marker) != 1:
            raise RuntimeError('pinned loader initialization context changed')
        text = text.replace(marker, replacement)
    if text != path.read_text():
        path.write_text(text)
