/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (C) 2026 Rigby Foundation */
#include "zvk_internal.h"

VKAPI_ATTR VkResult VKAPI_CALL vkCreateQueryPool(VkDevice device, const VkQueryPoolCreateInfo *info,
                                                 const VkAllocationCallbacks *allocator,
                                                 VkQueryPool *result)
{
    *result = VK_NULL_HANDLE;
    /* The Mac host exposes timer commands with zero counter precision. Do not
     * manufacture CPU timestamps for a queue with timestampValidBits == 0. */
    if (info->queryType != VK_QUERY_TYPE_OCCLUSION ||
        !device->physical->caps.v1.bset.occlusion_query || info->flags || info->pipelineStatistics)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    if (!info->queryCount || info->queryCount > 4096)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    zvk_allocator a;
    zvk_allocator_init(&a, allocator);
    VkQueryPool pool = zvk_alloc(&a, sizeof *pool);
    if (!pool)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    pool->queries = zvk_alloc(&a, info->queryCount * sizeof *pool->queries);
    if (!pool->queries) {
        zvk_free(&a, pool);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    pool->alloc = a;
    pool->device = device;
    pool->type = info->queryType;
    pool->count = info->queryCount;
    *result = pool;
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyQueryPool(VkDevice device, VkQueryPool pool,
                                              const VkAllocationCallbacks *allocator)
{
    (void)allocator;
    if (!pool)
        return;
    pthread_mutex_lock(&device->mutex);
    for (uint32_t i = 0; i < pool->count; i++) {
        struct zvk_query *q = pool->queries + i;
        if (q->handle)
            virgl_destroy_object(device->gpu, q->handle, VIRGL_OBJECT_QUERY);
        if (q->result)
            virgl_res_destroy(device->gpu, q->result);
    }
    pthread_mutex_unlock(&device->mutex);
    zvk_free(&pool->alloc, pool->queries);
    zvk_free(&pool->alloc, pool);
}
VkResult zvk_query_reset(VkQueryPool pool, uint32_t first, uint32_t count)
{
    if (!zvk_range(pool->count, first, count))
        return VK_ERROR_INITIALIZATION_FAILED;
    for (uint32_t i=0;i<count;i++) {
        struct zvk_query *q=pool->queries+first+i;
        if (q->active)
            return VK_ERROR_INITIALIZATION_FAILED;
        q->reset=1;
        q->available=q->suspended=0;
        q->value=0;
    }
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkResetQueryPoolEXT(VkDevice device, VkQueryPool pool,
                                               uint32_t first, uint32_t count)
{
    pthread_mutex_lock(&device->mutex);
    if (zvk_query_reset(pool, first, count)!=VK_SUCCESS)
        zvk_error("invalid host query reset range or active query");
    pthread_mutex_unlock(&device->mutex);
}
VkResult zvk_query_begin(VkQueryPool pool, uint32_t index)
{
    struct zvk_query *q = pool->queries + index;
    virgl *gpu = pool->device->gpu;
    if (!q->reset || q->active)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (!q->result) {
        q->result = virgl_res_create(gpu, PIPE_BUFFER, VIRGL_FORMAT_R8_UNORM, VIRGL_BIND_CUSTOM,
                                     4096, 1, 1, 0, 4096);
        if (!q->result || !q->result->map) {
            if (q->result)
                virgl_res_destroy(gpu, q->result);
            q->result = NULL;
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
    }
    if (!q->handle) {
        q->handle = virgl_handle(gpu);
        virgl_create_query(gpu, q->handle, PIPE_QUERY_OCCLUSION_COUNTER, 0, q->result->id, 0);
    }
    memset(q->result->map, 0, sizeof(struct virgl_host_query_state));
    q->active = 1;
    q->value = 0;
    q->suspended = 0;
    q->available = 0;
    q->reset = 0;
    virgl_begin_query(gpu, q->handle);
    return VK_SUCCESS;
}
static VkResult query_segment(VkQueryPool pool, uint32_t index, int finish)
{
    struct zvk_query *q = pool->queries + index;
    virgl *gpu = pool->device->gpu;
    if (!q->active || q->suspended)
        return VK_ERROR_INITIALIZATION_FAILED;
    virgl_end_query(gpu, q->handle);
    virgl_get_query_result(gpu, q->handle, 1);
    if (virgl_flush_fenced(gpu))
        return VK_ERROR_DEVICE_LOST;
    volatile struct virgl_host_query_state *result = q->result->map;
    if (result->query_state != VIRGL_QUERY_STATE_DONE) {
        zvk_error("host occlusion query did not complete");
        return VK_ERROR_DEVICE_LOST;
    }
    q->value += result->result;
    q->active = !finish;
    q->available = finish;
    q->suspended = !finish;
    pthread_cond_broadcast(&pool->device->condition);
    return VK_SUCCESS;
}
VkResult zvk_query_end(VkQueryPool pool, uint32_t index)
{
    return query_segment(pool, index, 1);
}
VkResult zvk_query_pause(VkQueryPool pool, uint32_t index)
{
    return query_segment(pool, index, 0);
}
void zvk_query_resume(VkQueryPool pool, uint32_t index)
{
    struct zvk_query *q = pool->queries + index;
    memset(q->result->map, 0, sizeof(struct virgl_host_query_state));
    virgl_begin_query(pool->device->gpu, q->handle);
    q->suspended = 0;
}
VkResult zvk_query_results(VkQueryPool pool, uint32_t first, uint32_t count, size_t size,
                           void *data, VkDeviceSize stride, VkQueryResultFlags flags)
{
    VkDeviceSize word = flags & VK_QUERY_RESULT_64_BIT ? 8 : 4,
                 bytes = word * (flags & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT ? 2 : 1);
    if (first > pool->count || count > pool->count - first ||
        (count > 1 && (stride < bytes || stride % word)) ||
        (count &&
         (stride > UINT64_MAX / count || !zvk_range(size, 0, (count - 1) * stride + bytes))))
        return VK_ERROR_INITIALIZATION_FAILED;
    VkResult result = VK_SUCCESS;
    for (uint32_t i = 0; i < count; i++) {
        struct zvk_query *q = pool->queries + first + i;
        uint8_t *out = (uint8_t *)data + i * stride;
        if (!q->available)
            result = VK_NOT_READY;
        if (q->available || (flags & VK_QUERY_RESULT_PARTIAL_BIT)) {
            uint64_t value = q->available ? q->value : 0;
            memcpy(out, &value, (size_t)word);
        }
        if (flags & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) {
            uint64_t available = q->available != 0;
            memcpy(out + word, &available, (size_t)word);
        }
    }
    return result;
}
VKAPI_ATTR VkResult VKAPI_CALL vkGetQueryPoolResults(VkDevice device, VkQueryPool pool,
                                                     uint32_t first, uint32_t count, size_t size,
                                                     void *data, VkDeviceSize stride,
                                                     VkQueryResultFlags flags)
{
    pthread_mutex_lock(&device->mutex);
    if (first > pool->count || count > pool->count - first) {
        pthread_mutex_unlock(&device->mutex);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (flags & VK_QUERY_RESULT_WAIT_BIT)
        for (;;) {
            int ready = 1;
            for (uint32_t i = 0; i < count; i++)
                if (!pool->queries[first + i].available)
                    ready = 0;
            if (ready || device->lost)
                break;
            pthread_cond_wait(&device->condition, &device->mutex);
        }
    VkResult result = device->lost
                          ? device->lost
                          : zvk_query_results(pool, first, count, size, data, stride, flags);
    pthread_mutex_unlock(&device->mutex);
    return result;
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateBufferView(VkDevice device,
                                                  const VkBufferViewCreateInfo *info,
                                                  const VkAllocationCallbacks *allocator,
                                                  VkBufferView *result)
{
    *result = VK_NULL_HANDLE;
    uint32_t kind, components;
    if (!(device->physical->caps.capability_bits_v2 & VIRGL_CAP_V2_ZVK_METAL_IMAGES) ||
        info->flags || !info->buffer || info->buffer->device != device ||
        !(info->buffer->usage &
          (VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT)) ||
        !zvk_texel_format(info->format, &kind, &components) || (info->offset & 15) ||
        ((info->buffer->usage & VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT) &&
         !zvk_storage_format(info->format)) ||
        info->offset >= info->buffer->size)
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    VkDeviceSize bpp = zvk_bpp(info->format);
    VkDeviceSize range = info->range == VK_WHOLE_SIZE
                             ? (info->buffer->size - info->offset) / bpp * bpp
                             : info->range;
    if (!range || range % bpp || range / bpp > 65536 ||
        !zvk_range(info->buffer->size, info->offset, range))
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    zvk_allocator a;
    zvk_allocator_init(&a, allocator);
    VkBufferView view = zvk_alloc(&a, sizeof *view);
    if (!view)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    *view = (struct VkBufferView_T){.alloc = a, .device = device, .buffer = info->buffer,
                                   .format = info->format, .offset = info->offset, .range = range};
    *result = view;
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyBufferView(VkDevice device, VkBufferView view,
                                               const VkAllocationCallbacks *allocator)
{
    (void)device;
    (void)allocator;
    if (view) {
        pthread_mutex_lock(&device->mutex);
        if (view->sampler_view)
            virgl_destroy_object(device->gpu, view->sampler_view, VIRGL_OBJECT_SAMPLER_VIEW);
        if (view->sampler)
            virgl_destroy_object(device->gpu, view->sampler, VIRGL_OBJECT_SAMPLER_STATE);
        if (view->gpu)
            virgl_res_destroy(device->gpu, view->gpu);
        pthread_mutex_unlock(&device->mutex);
        zvk_free(&view->alloc, view);
    }
}
