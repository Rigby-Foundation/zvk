/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (C) 2026 Rigby Foundation */
#include "zvk_internal.h"

void *zvk_record(VkCommandBuffer command, enum zvk_op op, size_t bytes)
{
    if (command->state != ZVK_RECORDING || command->error != VK_SUCCESS) {
        if (command->error == VK_SUCCESS)
            command->error = VK_ERROR_INITIALIZATION_FAILED;
        return NULL;
    }
    if (bytes > SIZE_MAX - sizeof(struct zvk_command) - 7) {
        command->error = VK_ERROR_OUT_OF_HOST_MEMORY;
        return NULL;
    }
    size_t total = (sizeof(struct zvk_command) + bytes + 7) & ~(size_t)7;
    if (total > SIZE_MAX - command->size) {
        command->error = VK_ERROR_OUT_OF_HOST_MEMORY;
        return NULL;
    }
    size_t need = command->size + total;
    if (need > command->capacity) {
        size_t cap = need > SIZE_MAX / 2 ? need : need * 2;
        void *p = zvk_grow(&command->pool->alloc, command->commands, command->size, cap);
        if (!p) {
            command->error = VK_ERROR_OUT_OF_HOST_MEMORY;
            return NULL;
        }
        command->commands = p;
        command->capacity = cap;
    }
    struct zvk_command *header = (void *)(command->commands + command->size);
    header->op = op;
    header->size = (uint32_t)total;
    command->size = need;
    return header + 1;
}
static void unsupported(VkCommandBuffer command, const char *name)
{
    command->error = VK_ERROR_FEATURE_NOT_PRESENT;
    zvk_error("%s is not supported by the current zvk graphics profile", name);
    if (getenv("ZVK_DEBUG_SHADERS"))
        fprintf(stderr, "%s\n", zvkGetLastError());
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateCommandPool(VkDevice d, const VkCommandPoolCreateInfo *info,
                                                   const VkAllocationCallbacks *allocator,
                                                   VkCommandPool *result)
{
    *result = VK_NULL_HANDLE;
    if (info->queueFamilyIndex ||
        (info->flags &
         ~(VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT)))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    zvk_allocator a;
    zvk_allocator_init(&a, allocator);
    VkCommandPool pool = zvk_alloc(&a, sizeof *pool);
    if (!pool)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    pool->alloc = a;
    pool->device = d;
    pool->flags = info->flags;
    *result = pool;
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkFreeCommandBuffers(VkDevice d, VkCommandPool pool, uint32_t count,
                                                const VkCommandBuffer *buffers)
{
    (void)d;
    for (uint32_t i = 0; i < count; i++) {
        VkCommandBuffer command = buffers[i];
        if (!command)
            continue;
        VkCommandBuffer *link = &pool->buffers;
        while (*link && *link != command)
            link = &(*link)->next;
        if (*link)
            *link = command->next;
        zvk_free(&pool->alloc, command->commands);
        zvk_free(&pool->alloc, command);
    }
}
VKAPI_ATTR void VKAPI_CALL vkDestroyCommandPool(VkDevice d, VkCommandPool pool,
                                                const VkAllocationCallbacks *allocator)
{
    (void)allocator;
    if (pool) {
        while (pool->buffers)
            vkFreeCommandBuffers(d, pool, 1, &pool->buffers);
        zvk_free(&pool->alloc, pool);
    }
}
VKAPI_ATTR VkResult VKAPI_CALL vkResetCommandPool(VkDevice d, VkCommandPool pool,
                                                  VkCommandPoolResetFlags flags)
{
    (void)d;
    for (VkCommandBuffer c = pool->buffers; c; c = c->next) {
        if (c->state == ZVK_PENDING)
            return VK_ERROR_INITIALIZATION_FAILED;
        c->size = 0;
        c->error = VK_SUCCESS;
        c->in_pass = 0;
        c->state = ZVK_INITIAL;
        if (flags & VK_COMMAND_POOL_RESET_RELEASE_RESOURCES_BIT) {
            zvk_free(&pool->alloc, c->commands);
            c->commands = NULL;
            c->capacity = 0;
        }
    }
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkAllocateCommandBuffers(VkDevice d,
                                                        const VkCommandBufferAllocateInfo *info,
                                                        VkCommandBuffer *buffers)
{
    (void)d;
    for (uint32_t i = 0; i < info->commandBufferCount; i++)
        buffers[i] = VK_NULL_HANDLE;
    for (uint32_t i = 0; i < info->commandBufferCount; i++) {
        VkCommandBuffer c = zvk_alloc(&info->commandPool->alloc, sizeof *c);
        if (!c) {
            vkFreeCommandBuffers(d, info->commandPool, i, buffers);
            for (uint32_t j = 0; j < i; j++)
                buffers[j] = VK_NULL_HANDLE;
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        set_loader_magic_value(c);
        c->pool = info->commandPool;
        c->level = info->level;
        c->next = c->pool->buffers;
        c->pool->buffers = c;
        buffers[i] = c;
    }
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkResetCommandBuffer(VkCommandBuffer c,
                                                    VkCommandBufferResetFlags flags)
{
    if (c->state == ZVK_PENDING ||
        !(c->pool->flags & VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT))
        return VK_ERROR_INITIALIZATION_FAILED;
    c->state = ZVK_INITIAL;
    c->size = 0;
    c->error = VK_SUCCESS;
    c->in_pass = 0;
    if (flags & VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT) {
        zvk_free(&c->pool->alloc, c->commands);
        c->commands = NULL;
        c->capacity = 0;
    }
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkBeginCommandBuffer(VkCommandBuffer c,
                                                    const VkCommandBufferBeginInfo *info)
{
    if (c->state != ZVK_INITIAL && c->state != ZVK_PENDING && c->state != ZVK_RECORDING &&
        (c->pool->flags & VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT))
        vkResetCommandBuffer(c, 0);
    if (c->state != ZVK_INITIAL)
        return VK_ERROR_INITIALIZATION_FAILED;
    int continuation = c->level == VK_COMMAND_BUFFER_LEVEL_SECONDARY &&
                       (info->flags & VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT);
    if (c->level == VK_COMMAND_BUFFER_LEVEL_SECONDARY && !info->pInheritanceInfo)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (continuation && (!info->pInheritanceInfo->renderPass ||
                         info->pInheritanceInfo->subpass >= info->pInheritanceInfo->renderPass->nsubpasses))
        return VK_ERROR_INITIALIZATION_FAILED;
    c->size = 0;
    c->error = VK_SUCCESS;
    c->in_pass = continuation;
    c->pass = continuation ? info->pInheritanceInfo->renderPass : VK_NULL_HANDLE;
    c->subpass = continuation ? info->pInheritanceInfo->subpass : 0;
    c->inherited_framebuffer = continuation ? info->pInheritanceInfo->framebuffer : VK_NULL_HANDLE;
    c->usage = info->flags;
    c->state = ZVK_RECORDING;
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkEndCommandBuffer(VkCommandBuffer c)
{
    if (c->state != ZVK_RECORDING) {
        c->state = ZVK_INVALID;
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (c->error != VK_SUCCESS) {
        c->state = ZVK_INVALID;
        return c->error;
    }
    if (c->in_pass && c->level == VK_COMMAND_BUFFER_LEVEL_PRIMARY) {
        c->state = ZVK_INVALID;
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    c->state = ZVK_EXECUTABLE;
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkCmdBeginRenderPass(VkCommandBuffer c,
                                                const VkRenderPassBeginInfo *info,
                                                VkSubpassContents contents)
{
    if (contents > VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS || c->in_pass ||
        c->level != VK_COMMAND_BUFFER_LEVEL_PRIMARY ||
        info->renderArea.offset.x < 0 ||
        info->renderArea.offset.y < 0 ||
        !zvk_range(info->framebuffer->width, (uint32_t)info->renderArea.offset.x,
                   info->renderArea.extent.width) ||
        !zvk_range(info->framebuffer->height, (uint32_t)info->renderArea.offset.y,
                   info->renderArea.extent.height)) {
        unsupported(c, "render-pass configuration");
        return;
    }
    if (info->clearValueCount > (SIZE_MAX - sizeof(struct zvk_begin_pass)) / sizeof(VkClearValue)) {
        c->error = VK_ERROR_OUT_OF_HOST_MEMORY;
        return;
    }
    struct zvk_begin_pass *p = zvk_record(c, ZVK_BEGIN_PASS,
                                         sizeof *p + info->clearValueCount * sizeof(VkClearValue));
    if (!p)
        return;
    p->pass = info->renderPass;
    p->fb = info->framebuffer;
    p->area = info->renderArea;
    p->nclear = info->clearValueCount;
    if (p->nclear)
        memcpy(p->clear, info->pClearValues, p->nclear * sizeof *p->clear);
    c->in_pass = 1;
    c->pass = info->renderPass;
    c->subpass = 0;
}
VKAPI_ATTR void VKAPI_CALL vkCmdEndRenderPass(VkCommandBuffer c)
{
    if (!c->in_pass || c->level != VK_COMMAND_BUFFER_LEVEL_PRIMARY ||
        c->subpass + 1 != c->pass->nsubpasses) {
        unsupported(c, "ending an inactive render pass");
        return;
    }
    if (zvk_record(c, ZVK_END_PASS, 0))
        c->in_pass = 0;
}
VKAPI_ATTR void VKAPI_CALL vkCmdBindPipeline(VkCommandBuffer c, VkPipelineBindPoint point,
                                             VkPipeline pipeline)
{
    if ((point != VK_PIPELINE_BIND_POINT_GRAPHICS && point != VK_PIPELINE_BIND_POINT_COMPUTE) ||
        pipeline->point != point) {
        unsupported(c, "pipeline bind point mismatch");
        return;
    }
    VkPipeline *p = zvk_record(
        c, point == VK_PIPELINE_BIND_POINT_COMPUTE ? ZVK_BIND_COMPUTE : ZVK_BIND_PIPELINE,
        sizeof *p);
    if (p)
        *p = pipeline;
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetViewport(VkCommandBuffer c, uint32_t first, uint32_t count,
                                            const VkViewport *viewports)
{
    if (first || count != 1 || viewports[0].width <= 0 || viewports[0].height <= 0) {
        unsupported(c, "viewport configuration");
        return;
    }
    VkViewport *p = zvk_record(c, ZVK_VIEWPORT, sizeof *p);
    if (p)
        *p = viewports[0];
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetScissor(VkCommandBuffer c, uint32_t first, uint32_t count,
                                           const VkRect2D *scissors)
{
    if (first || count != 1 || scissors[0].offset.x < 0 || scissors[0].offset.y < 0) {
        unsupported(c, "scissor configuration");
        return;
    }
    VkRect2D *p = zvk_record(c, ZVK_SCISSOR, sizeof *p);
    if (p)
        *p = scissors[0];
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetBlendConstants(VkCommandBuffer c, const float values[4])
{
    float *p = zvk_record(c, ZVK_BLEND_CONSTANTS, 4 * sizeof(float));
    if (p)
        memcpy(p, values, 4 * sizeof(float));
}
VKAPI_ATTR void VKAPI_CALL vkCmdBindVertexBuffers(VkCommandBuffer c, uint32_t first, uint32_t count,
                                                  const VkBuffer *buffers,
                                                  const VkDeviceSize *offsets)
{
    if (first > ZVK_ATTRS || count > ZVK_ATTRS - first) {
        unsupported(c, "vertex buffer binding range");
        return;
    }
    struct zvk_bind_vertex *p = zvk_record(c, ZVK_BIND_VERTEX, sizeof *p);
    if (!p)
        return;
    p->first = first;
    p->count = count;
    if (count) {
        memcpy(p->buffers, buffers, count * sizeof *buffers);
        memcpy(p->offsets, offsets, count * sizeof *offsets);
    }
}
VKAPI_ATTR void VKAPI_CALL vkCmdBindIndexBuffer(VkCommandBuffer c, VkBuffer buffer,
                                                VkDeviceSize offset, VkIndexType type)
{
    if (type != VK_INDEX_TYPE_UINT16 && type != VK_INDEX_TYPE_UINT32) {
        unsupported(c, "index type");
        return;
    }
    struct zvk_bind_index *p = zvk_record(c, ZVK_BIND_INDEX, sizeof *p);
    if (p)
        *p = (struct zvk_bind_index){buffer, offset, type};
}
VKAPI_ATTR void VKAPI_CALL vkCmdBindDescriptorSets(VkCommandBuffer c, VkPipelineBindPoint point,
                                                   VkPipelineLayout layout, uint32_t first,
                                                   uint32_t count, const VkDescriptorSet *sets,
                                                   uint32_t ndynamic, const uint32_t *dynamic)
{
    if ((point != VK_PIPELINE_BIND_POINT_GRAPHICS && point != VK_PIPELINE_BIND_POINT_COMPUTE) ||
        first > ZVK_SETS || count > ZVK_SETS - first || ndynamic > ZVK_SETS * ZVK_BINDINGS) {
        unsupported(c, "descriptor binding configuration");
        return;
    }
    struct zvk_bind_sets *p = zvk_record(
        c, point == VK_PIPELINE_BIND_POINT_COMPUTE ? ZVK_BIND_COMPUTE_SETS : ZVK_BIND_SETS,
        sizeof *p);
    if (!p)
        return;
    p->layout = layout;
    p->first = first;
    p->count = count;
    p->ndynamic = ndynamic;
    if (count)
        memcpy(p->sets, sets, count * sizeof *sets);
    if (ndynamic)
        memcpy(p->dynamic, dynamic, ndynamic * sizeof *dynamic);
}
VKAPI_ATTR void VKAPI_CALL vkCmdPushConstants(VkCommandBuffer c, VkPipelineLayout layout,
                                              VkShaderStageFlags stages, uint32_t offset,
                                              uint32_t size, const void *values)
{
    if ((offset | size) & 3 || !zvk_range(layout->push_bytes, offset, size)) {
        unsupported(c, "push-constant range");
        return;
    }
    struct zvk_push *p = zvk_record(c, ZVK_PUSH_CONSTANTS, sizeof *p);
    if (!p)
        return;
    p->stages = stages;
    p->offset = offset;
    p->size = size;
    memcpy(p->data, values, size);
}
VKAPI_ATTR void VKAPI_CALL vkCmdDraw(VkCommandBuffer c, uint32_t count, uint32_t instances,
                                     uint32_t first, uint32_t first_instance)
{
    if (!c->in_pass) {
        unsupported(c, "draw outside a render pass");
        return;
    }
    struct zvk_draw *p = zvk_record(c, ZVK_DRAW, sizeof *p);
    if (p)
        *p = (struct zvk_draw){count, instances, first, first_instance, 0};
}
VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndexed(VkCommandBuffer c, uint32_t count, uint32_t instances,
                                            uint32_t first, int32_t vertex_offset,
                                            uint32_t first_instance)
{
    if (!c->in_pass) {
        unsupported(c, "indexed draw configuration");
        return;
    }
    struct zvk_draw *p = zvk_record(c, ZVK_DRAW_INDEXED, sizeof *p);
    if (p)
        *p = (struct zvk_draw){count, instances, first, first_instance, vertex_offset};
}
VKAPI_ATTR void VKAPI_CALL vkCmdCopyBuffer(VkCommandBuffer c, VkBuffer src, VkBuffer dst,
                                           uint32_t count, const VkBufferCopy *regions)
{
    for (uint32_t i = 0; i < count; i++) {
        if (!zvk_range(src->size, regions[i].srcOffset, regions[i].size) ||
            !zvk_range(dst->size, regions[i].dstOffset, regions[i].size)) {
            unsupported(c, "buffer copy range");
            return;
        }
        struct zvk_copy_buffer *p = zvk_record(c, ZVK_COPY_BUFFER, sizeof *p);
        if (p)
            *p = (struct zvk_copy_buffer){src, dst, regions[i]};
    }
}
VKAPI_ATTR void VKAPI_CALL vkCmdFillBuffer(VkCommandBuffer c, VkBuffer buffer, VkDeviceSize offset,
                                           VkDeviceSize size, uint32_t value)
{
    if (size == VK_WHOLE_SIZE && offset <= buffer->size)
        size = (buffer->size - offset) & ~(VkDeviceSize)3;
    if ((offset | size) & 3 || !zvk_range(buffer->size, offset, size)) {
        unsupported(c, "buffer fill range");
        return;
    }
    struct zvk_fill *p = zvk_record(c, ZVK_FILL_BUFFER, sizeof *p);
    if (p)
        *p = (struct zvk_fill){buffer, offset, size, value};
}
VKAPI_ATTR void VKAPI_CALL vkCmdUpdateBuffer(VkCommandBuffer c, VkBuffer buffer,
                                             VkDeviceSize offset, VkDeviceSize size,
                                             const void *data)
{
    if ((offset | size) & 3 || size > 65536 || !zvk_range(buffer->size, offset, size)) {
        unsupported(c, "buffer update range");
        return;
    }
    struct zvk_fill *p = zvk_record(c, ZVK_UPDATE_BUFFER, sizeof *p + (size_t)size);
    if (p) {
        *p = (struct zvk_fill){buffer, offset, size, 0};
        memcpy(p + 1, data, (size_t)size);
    }
}
static int valid_range(VkImage image, const VkImageSubresourceRange *r)
{
    VkImageSubresourceRange out;
    return zvk_image_range(image, r, &out);
}
static int subresource(VkImage image, const VkImageSubresourceLayers *s)
{
    return s->mipLevel < image->info.mipLevels && s->layerCount &&
           zvk_range(image->info.arrayLayers, s->baseArrayLayer, s->layerCount);
}
VKAPI_ATTR void VKAPI_CALL vkCmdClearColorImage(VkCommandBuffer c, VkImage image,
                                                VkImageLayout layout,
                                                const VkClearColorValue *color, uint32_t count,
                                                const VkImageSubresourceRange *ranges)
{
    if (zvk_block(image->info.format) != 1) {
        unsupported(c, "compressed images cannot be cleared");
        return;
    }
    for (uint32_t i = 0; i < count; i++) {
        if (!valid_range(image, ranges + i) || ranges[i].aspectMask != VK_IMAGE_ASPECT_COLOR_BIT) {
            unsupported(c, "color clear range");
            return;
        }
        struct zvk_clear *p = zvk_record(c, ZVK_CLEAR_IMAGE, sizeof *p);
        if (p) {
            p->image = image;
            p->layout = layout;
            p->value.color = *color;
            p->range = ranges[i];
            p->depth = 0;
        }
    }
}
VKAPI_ATTR void VKAPI_CALL vkCmdClearDepthStencilImage(VkCommandBuffer c, VkImage image,
                                                       VkImageLayout layout,
                                                       const VkClearDepthStencilValue *value,
                                                       uint32_t count,
                                                       const VkImageSubresourceRange *ranges)
{
    for (uint32_t i = 0; i < count; i++) {
        if (!valid_range(image, ranges + i) ||
            (ranges[i].aspectMask & ~(VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))) {
            unsupported(c, "depth clear range");
            return;
        }
        struct zvk_clear *p = zvk_record(c, ZVK_CLEAR_IMAGE, sizeof *p);
        if (p) {
            p->image = image;
            p->layout = layout;
            p->value.depthStencil = *value;
            p->range = ranges[i];
            p->depth = 1;
        }
    }
}
static void image_buffer_record(VkCommandBuffer c, enum zvk_op op, VkImage image, VkBuffer buffer,
                                VkImageLayout layout, uint32_t count,
                                const VkBufferImageCopy *regions)
{
    for (uint32_t i = 0; i < count; i++) {
        const VkBufferImageCopy *r = regions + i;
        if (image->info.samples != VK_SAMPLE_COUNT_1_BIT || !r->imageExtent.width ||
            !r->imageExtent.height || !subresource(image, &r->imageSubresource) ||
            r->imageOffset.x < 0 || r->imageOffset.y < 0 || r->imageOffset.z < 0 ||
            !r->imageExtent.depth ||
            !zvk_range(zvk_mip(image->info.extent.depth, r->imageSubresource.mipLevel),
                       (uint32_t)r->imageOffset.z, r->imageExtent.depth) ||
            !zvk_range(zvk_mip(image->info.extent.width, r->imageSubresource.mipLevel),
                       (uint32_t)r->imageOffset.x, r->imageExtent.width) ||
            !zvk_range(zvk_mip(image->info.extent.height, r->imageSubresource.mipLevel),
                       (uint32_t)r->imageOffset.y, r->imageExtent.height) ||
            (r->bufferRowLength && r->bufferRowLength < r->imageExtent.width) ||
            (r->bufferImageHeight && r->bufferImageHeight < r->imageExtent.height)) {
            unsupported(c, "image/buffer copy extent");
            return;
        }
        uint32_t bpp = zvk_aspect_bpp(image, r->imageSubresource.aspectMask);
        if (!bpp) {
            unsupported(c, "image/buffer copy aspect");
            return;
        }
        unsigned block = zvk_block(image->info.format);
        if (!zvk_block_region(image, r->imageSubresource.mipLevel, r->imageOffset.x,
                              r->imageOffset.y, r->imageExtent.width, r->imageExtent.height) ||
            (block > 1 && ((r->bufferRowLength % block) || (r->bufferImageHeight % block) ||
                           (r->bufferOffset % bpp)))) {
            unsupported(c, "compressed copy block alignment");
            return;
        }
        VkDeviceSize row = (VkDeviceSize)zvk_block_count(
                               image->info.format,
                               r->bufferRowLength ? r->bufferRowLength : r->imageExtent.width) *
                           bpp;
        VkDeviceSize height =
            zvk_block_count(image->info.format,
                            r->bufferImageHeight ? r->bufferImageHeight : r->imageExtent.height);
        VkDeviceSize rows = zvk_block_count(image->info.format, r->imageExtent.height);
        VkDeviceSize tail =
            row * (rows - 1) +
            (VkDeviceSize)zvk_block_count(image->info.format, r->imageExtent.width) * bpp;
        VkDeviceSize layer_bytes = 0;
        unsigned extra_layers = r->imageSubresource.layerCount * r->imageExtent.depth - 1;
        if (extra_layers) {
            if (!row || height > UINT64_MAX / row || row * height > buffer->size / extra_layers) {
                unsupported(c, "image/buffer copy layer pitch");
                return;
            }
            layer_bytes = row * height;
        }
        if (tail > buffer->size || layer_bytes * extra_layers > buffer->size - tail) {
            unsupported(c, "image/buffer copy pitch");
            return;
        }
        VkDeviceSize bytes = layer_bytes * extra_layers + tail;
        if (!zvk_range(buffer->size, r->bufferOffset, bytes)) {
            unsupported(c, "image/buffer copy memory range");
            return;
        }
        for (uint32_t layer = 0; layer < r->imageSubresource.layerCount; layer++) {
            struct zvk_image_buffer *p = zvk_record(c, op, sizeof *p);
            if (p) {
                *p = (struct zvk_image_buffer){image, buffer, layout, *r};
                p->region.imageSubresource.baseArrayLayer += layer;
                p->region.imageSubresource.layerCount = 1;
                p->region.bufferOffset += layer_bytes * layer;
            }
        }
    }
}
VKAPI_ATTR void VKAPI_CALL vkCmdCopyImageToBuffer(VkCommandBuffer c, VkImage image,
                                                  VkImageLayout layout, VkBuffer buffer,
                                                  uint32_t count, const VkBufferImageCopy *regions)
{
    image_buffer_record(c, ZVK_COPY_IMAGE_BUFFER, image, buffer, layout, count, regions);
}
VKAPI_ATTR void VKAPI_CALL vkCmdCopyBufferToImage(VkCommandBuffer c, VkBuffer buffer, VkImage image,
                                                  VkImageLayout layout, uint32_t count,
                                                  const VkBufferImageCopy *regions)
{
    image_buffer_record(c, ZVK_COPY_BUFFER_IMAGE, image, buffer, layout, count, regions);
}
VKAPI_ATTR void VKAPI_CALL vkCmdCopyImage(VkCommandBuffer c, VkImage src, VkImageLayout src_layout,
                                          VkImage dst, VkImageLayout dst_layout, uint32_t count,
                                          const VkImageCopy *regions)
{
    for (uint32_t i = 0; i < count; i++) {
        const VkImageCopy *r = regions + i;
        unsigned sb = zvk_block(src->info.format), db = zvk_block(dst->info.format);
        uint32_t dw = zvk_block_count(src->info.format, r->extent.width) * db;
        uint32_t dh = zvk_block_count(src->info.format, r->extent.height) * db;
        uint32_t dmw = zvk_mip(dst->info.extent.width, r->dstSubresource.mipLevel);
        uint32_t dmh = zvk_mip(dst->info.extent.height, r->dstSubresource.mipLevel);
        if ((uint64_t)(uint32_t)r->dstOffset.x + dw > dmw && db > 1)
            dw = dmw > (uint32_t)r->dstOffset.x ? dmw - (uint32_t)r->dstOffset.x : 0;
        if ((uint64_t)(uint32_t)r->dstOffset.y + dh > dmh && db > 1)
            dh = dmh > (uint32_t)r->dstOffset.y ? dmh - (uint32_t)r->dstOffset.y : 0;
        int s3 = src->info.imageType == VK_IMAGE_TYPE_3D;
        int d3 = dst->info.imageType == VK_IMAGE_TYPE_3D;
        uint32_t slices = s3 ? r->extent.depth : r->srcSubresource.layerCount;
        uint32_t dslices = d3 ? r->extent.depth : r->dstSubresource.layerCount;
        int color = r->srcSubresource.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT;
        if (src->info.samples != dst->info.samples || src->bpp != dst->bpp ||
            (!color && src->info.format != dst->info.format) ||
            (src->info.imageType != dst->info.imageType &&
             !((s3 && dst->info.imageType == VK_IMAGE_TYPE_2D) ||
               (d3 && src->info.imageType == VK_IMAGE_TYPE_2D))) ||
            r->srcSubresource.aspectMask != r->dstSubresource.aspectMask ||
            !(r->srcSubresource.aspectMask & zvk_format_aspects(src->info.format)) ||
            !(r->dstSubresource.aspectMask & zvk_format_aspects(dst->info.format)) ||
            !subresource(src, &r->srcSubresource) || !subresource(dst, &r->dstSubresource) ||
            slices != dslices || r->srcOffset.x < 0 || r->srcOffset.y < 0 ||
            r->dstOffset.x < 0 || r->dstOffset.y < 0 || r->srcOffset.z < 0 || r->dstOffset.z < 0 ||
            !r->extent.depth || !dw || !dh ||
            !zvk_range(zvk_mip(src->info.extent.depth, r->srcSubresource.mipLevel),
                       (uint32_t)r->srcOffset.z, s3 ? r->extent.depth : 1) ||
            !zvk_range(zvk_mip(dst->info.extent.depth, r->dstSubresource.mipLevel),
                       (uint32_t)r->dstOffset.z, d3 ? r->extent.depth : 1) ||
            !zvk_block_region(src, r->srcSubresource.mipLevel, r->srcOffset.x, r->srcOffset.y,
                              r->extent.width, r->extent.height) ||
            !zvk_block_region(dst, r->dstSubresource.mipLevel, r->dstOffset.x, r->dstOffset.y,
                              dw, dh) ||
            (r->extent.width + sb - 1) / sb != (dw + db - 1) / db ||
            (r->extent.height + sb - 1) / sb != (dh + db - 1) / db) {
            unsupported(c, "image copy extent/format");
            return;
        }
        /* Normalize layers and volume slices into single-slice records. Each
         * resource keeps its own texel-block dimensions and raw byte format. */
        for (uint32_t slice = 0; slice < slices; slice++) {
            struct zvk_copy_image *p = zvk_record(c, ZVK_COPY_IMAGE, sizeof *p);
            if (p) {
                *p = (struct zvk_copy_image){src, dst, src_layout, dst_layout, *r};
                if (s3)
                    p->region.srcOffset.z += slice;
                else
                    p->region.srcSubresource.baseArrayLayer += slice;
                if (d3)
                    p->region.dstOffset.z += slice;
                else
                    p->region.dstSubresource.baseArrayLayer += slice;
                p->region.srcSubresource.layerCount = p->region.dstSubresource.layerCount = 1;
                p->region.extent.depth = 1;
            }
        }
    }
}
VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier(VkCommandBuffer c, VkPipelineStageFlags src,
                                                VkPipelineStageFlags dst, VkDependencyFlags flags,
                                                uint32_t nm, const VkMemoryBarrier *memory,
                                                uint32_t nb, const VkBufferMemoryBarrier *buffers,
                                                uint32_t ni, const VkImageMemoryBarrier *images)
{
    (void)src;
    (void)dst;
    (void)flags;
    (void)nm;
    (void)memory;
    (void)nb;
    (void)buffers;
    if (!zvk_record(c, ZVK_BARRIER, 0))
        return;
    for (uint32_t i = 0; i < ni; i++) {
        if (!valid_range(images[i].image, &images[i].subresourceRange)) {
            unsupported(c, "image barrier range");
            return;
        }
        struct zvk_transition *p = zvk_record(c, ZVK_BARRIER, sizeof *p);
        if (p) {
            *p = (struct zvk_transition){images[i].image, images[i].oldLayout, images[i].newLayout,
                                         images[i].subresourceRange};
            zvk_image_range(p->image, &p->range, &p->range);
        }
    }
}
VKAPI_ATTR void VKAPI_CALL vkCmdExecuteCommands(VkCommandBuffer c, uint32_t count,
                                                const VkCommandBuffer *commands)
{
    if (c->level != VK_COMMAND_BUFFER_LEVEL_PRIMARY) {
        unsupported(c, "nested secondary command buffer");
        return;
    }
    for (uint32_t i = 0; i < count; i++) {
        if (commands[i]->level != VK_COMMAND_BUFFER_LEVEL_SECONDARY ||
            commands[i]->state != ZVK_EXECUTABLE || commands[i]->in_pass != c->in_pass ||
            (c->in_pass && (commands[i]->subpass != c->subpass ||
                            !zvk_passes_compatible(commands[i]->pass, c->pass)))) {
            unsupported(c, "secondary command buffer state");
            return;
        }
        VkCommandBuffer *p = zvk_record(c, ZVK_EXECUTE, sizeof *p);
        if (p)
            *p = commands[i];
    }
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetEvent(VkCommandBuffer c, VkEvent event,
                                         VkPipelineStageFlags stage)
{
    (void)stage;
    VkEvent *p = zvk_record(c, ZVK_SET_EVENT, sizeof *p);
    if (p)
        *p = event;
}
VKAPI_ATTR void VKAPI_CALL vkCmdResetEvent(VkCommandBuffer c, VkEvent event,
                                           VkPipelineStageFlags stage)
{
    (void)stage;
    VkEvent *p = zvk_record(c, ZVK_RESET_EVENT, sizeof *p);
    if (p)
        *p = event;
}
/* These commands cannot be used by this profile. Record a checked failure
 * instead of silently treating an unsupported operation as completed. */
VKAPI_ATTR void VKAPI_CALL vkCmdDispatch(VkCommandBuffer c, uint32_t x, uint32_t y, uint32_t z)
{
    if (c->in_pass ||
        !(c->pool->device->physical->caps.capability_bits_v2 & VIRGL_CAP_V2_ZVK_METAL) ||
        x > 65535 || y > 65535 || z > 65535) {
        unsupported(c, "compute dispatch limits/backend");
        return;
    }
    struct zvk_dispatch *p = zvk_record(c, ZVK_DISPATCH, sizeof *p);
    if (p)
        *p = (struct zvk_dispatch){.groups = {x, y, z}};
}
VKAPI_ATTR void VKAPI_CALL vkCmdDispatchIndirect(VkCommandBuffer c, VkBuffer b, VkDeviceSize offset)
{
    if (c->in_pass ||
        !(c->pool->device->physical->caps.capability_bits_v2 & VIRGL_CAP_V2_ZVK_METAL) ||
        (offset & 3) || !zvk_range(b->size, offset, 12)) {
        unsupported(c, "indirect compute dispatch range/backend");
        return;
    }
    struct zvk_dispatch *p = zvk_record(c, ZVK_DISPATCH, sizeof *p);
    if (p)
        *p = (struct zvk_dispatch){.indirect = b, .offset = offset};
}
static void indirect(VkCommandBuffer c, VkBuffer buffer, VkDeviceSize offset, uint32_t count,
                     uint32_t stride, int indexed)
{
    VkDeviceSize bytes =
        indexed ? sizeof(VkDrawIndexedIndirectCommand) : sizeof(VkDrawIndirectCommand);
    VkDeviceSize span = count ? (VkDeviceSize)(count - 1) * stride + bytes : 0;
    if (!c->in_pass || count > ZVK_INDIRECT_DRAWS || (offset & 3) ||
        (count > 1 && ((stride & 3) || stride < bytes)) || !zvk_range(buffer->size, offset, span)) {
        unsupported(c, "indirect draw range");
        return;
    }
    struct zvk_indirect *p = zvk_record(c, ZVK_DRAW_INDIRECT, sizeof *p);
    if (p)
        *p = (struct zvk_indirect){buffer, offset, count, stride, indexed};
}
VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndirect(VkCommandBuffer c, VkBuffer buffer,
                                             VkDeviceSize offset, uint32_t count, uint32_t stride)
{
    indirect(c, buffer, offset, count, stride, 0);
}
VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndexedIndirect(VkCommandBuffer c, VkBuffer buffer,
                                                    VkDeviceSize offset, uint32_t count,
                                                    uint32_t stride)
{
    indirect(c, buffer, offset, count, stride, 1);
}
VKAPI_ATTR void VKAPI_CALL vkCmdNextSubpass(VkCommandBuffer c, VkSubpassContents contents)
{
    if (!c->in_pass || c->level != VK_COMMAND_BUFFER_LEVEL_PRIMARY ||
        contents > VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS ||
        c->subpass + 1 >= c->pass->nsubpasses) {
        unsupported(c, "subpass transition");
        return;
    }
    if (zvk_record(c, ZVK_NEXT_SUBPASS, 0))
        c->subpass++;
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetLineWidth(VkCommandBuffer c, float width)
{
    if (width != 1.0f) {
        unsupported(c, "line width without wideLines");
        return;
    }
    float *p = zvk_record(c, ZVK_LINE_WIDTH, sizeof *p);
    if (p)
        *p = width;
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthBias(VkCommandBuffer c, float a, float b, float d)
{
    float *p = zvk_record(c, ZVK_DEPTH_BIAS, 3 * sizeof *p);
    if (p) {
        p[0] = a;
        p[1] = b;
        p[2] = d;
    }
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthBounds(VkCommandBuffer c, float a, float b)
{
    /* The optional depth-bounds test remains disabled. Setting its unused
     * dynamic state is legal and must not invalidate a command buffer. */
    if (c->state != ZVK_RECORDING || a < 0 || a > 1 || b < 0 || b > 1)
        unsupported(c, "depth bounds state");
}
static void stencil(VkCommandBuffer c, VkStencilFaceFlags faces, uint32_t value, uint32_t kind)
{
    struct zvk_stencil *p = zvk_record(c, ZVK_STENCIL, sizeof *p);
    if (p)
        *p = (struct zvk_stencil){faces, value, kind};
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetStencilCompareMask(VkCommandBuffer c, VkStencilFaceFlags faces,
                                                      uint32_t value)
{
    stencil(c, faces, value, 0);
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetStencilWriteMask(VkCommandBuffer c, VkStencilFaceFlags faces,
                                                    uint32_t value)
{
    stencil(c, faces, value, 1);
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetStencilReference(VkCommandBuffer c, VkStencilFaceFlags faces,
                                                    uint32_t value)
{
    stencil(c, faces, value, 2);
}

VKAPI_ATTR void VKAPI_CALL vkCmdWaitEvents(VkCommandBuffer c, uint32_t count, const VkEvent *events,
                                           VkPipelineStageFlags src, VkPipelineStageFlags dst,
                                           uint32_t nm, const VkMemoryBarrier *memory, uint32_t nb,
                                           const VkBufferMemoryBarrier *buffers, uint32_t ni,
                                           const VkImageMemoryBarrier *images)
{
    if (count > 65536) {
        unsupported(c, "event count");
        return;
    }
    struct zvk_wait_events *p = zvk_record(c, ZVK_WAIT_EVENTS, sizeof *p + count * sizeof *events);
    if (!p)
        return;
    p->count = count;
    memcpy(p->events, events, count * sizeof *events);
    vkCmdPipelineBarrier(c, src, dst, 0, nm, memory, nb, buffers, ni, images);
}
VKAPI_ATTR void VKAPI_CALL vkCmdBlitImage(VkCommandBuffer c, VkImage src, VkImageLayout src_layout,
                                          VkImage dst, VkImageLayout dst_layout, uint32_t count,
                                          const VkImageBlit *regions, VkFilter filter)
{
    VkFormatProperties sp, dp;
    vkGetPhysicalDeviceFormatProperties(src->device->physical, src->info.format, &sp);
    vkGetPhysicalDeviceFormatProperties(dst->device->physical, dst->info.format, &dp);
    int depth = (zvk_format_aspects(src->info.format) & VK_IMAGE_ASPECT_DEPTH_BIT) != 0;
    if (src->info.imageType != dst->info.imageType || src->info.samples != VK_SAMPLE_COUNT_1_BIT ||
        dst->info.samples != VK_SAMPLE_COUNT_1_BIT || c->in_pass ||
        (filter != VK_FILTER_NEAREST && filter != VK_FILTER_LINEAR) ||
        !(sp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) ||
        !(dp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) ||
        (filter == VK_FILTER_LINEAR &&
         !(sp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)) ||
        zvk_format_integer(src->info.format) != zvk_format_integer(dst->info.format) ||
        (depth && (src->info.format != dst->info.format || filter != VK_FILTER_NEAREST)) ||
        depth != !!(zvk_format_aspects(dst->info.format) & VK_IMAGE_ASPECT_DEPTH_BIT)) {
        unsupported(c, "unsupported image blit format/filter");
        return;
    }
    for (uint32_t i = 0; i < count; i++) {
        const VkImageBlit *r = regions + i;
        if (r->srcSubresource.aspectMask != r->dstSubresource.aspectMask ||
            !(r->srcSubresource.aspectMask & zvk_format_aspects(src->info.format)) ||
            !subresource(src, &r->srcSubresource) || !subresource(dst, &r->dstSubresource) ||
            r->srcSubresource.layerCount != r->dstSubresource.layerCount ||
            (src->info.imageType != VK_IMAGE_TYPE_3D &&
             (r->srcOffsets[0].z || r->dstOffsets[0].z || r->srcOffsets[1].z != 1 ||
              r->dstOffsets[1].z != 1))) {
            unsupported(c, "image blit subresource");
            return;
        }
        for (int j = 0; j < 2; j++)
            if (r->srcOffsets[j].x < 0 || r->srcOffsets[j].y < 0 || r->dstOffsets[j].x < 0 ||
                r->dstOffsets[j].y < 0 || r->srcOffsets[j].z < 0 || r->dstOffsets[j].z < 0 ||
                (uint32_t)r->srcOffsets[j].z >
                    zvk_mip(src->info.extent.depth, r->srcSubresource.mipLevel) ||
                (uint32_t)r->dstOffsets[j].z >
                    zvk_mip(dst->info.extent.depth, r->dstSubresource.mipLevel) ||
                (uint32_t)r->srcOffsets[j].x >
                    zvk_mip(src->info.extent.width, r->srcSubresource.mipLevel) ||
                (uint32_t)r->srcOffsets[j].y >
                    zvk_mip(src->info.extent.height, r->srcSubresource.mipLevel) ||
                (uint32_t)r->dstOffsets[j].x >
                    zvk_mip(dst->info.extent.width, r->dstSubresource.mipLevel) ||
                (uint32_t)r->dstOffsets[j].y >
                    zvk_mip(dst->info.extent.height, r->dstSubresource.mipLevel)) {
                unsupported(c, "image blit coordinates");
                return;
            }
        for (uint32_t layer = 0; layer < r->srcSubresource.layerCount; layer++) {
            struct zvk_blit *p = zvk_record(c, ZVK_BLIT_IMAGE, sizeof *p);
            if (p) {
                *p = (struct zvk_blit){src, dst, src_layout, dst_layout, filter, *r};
                p->region.srcSubresource.baseArrayLayer += layer;
                p->region.dstSubresource.baseArrayLayer += layer;
                p->region.srcSubresource.layerCount = p->region.dstSubresource.layerCount = 1;
            }
        }
    }
}
VKAPI_ATTR void VKAPI_CALL vkCmdClearAttachments(VkCommandBuffer c, uint32_t count,
                                                 const VkClearAttachment *attachments,
                                                 uint32_t nrects, const VkClearRect *rects)
{
    if (!c->in_pass) {
        unsupported(c, "attachment clear outside a render pass");
        return;
    }
    for (uint32_t i = 0; i < count; i++)
        for (uint32_t j = 0; j < nrects; j++) {
            if (!rects[j].layerCount || rects[j].rect.offset.x < 0 || rects[j].rect.offset.y < 0) {
                unsupported(c, "attachment clear range");
                return;
            }
            struct zvk_attachment_clear *p = zvk_record(c, ZVK_CLEAR_ATTACHMENT, sizeof *p);
            if (p)
                *p = (struct zvk_attachment_clear){attachments[i], rects[j]};
        }
}
VKAPI_ATTR void VKAPI_CALL vkCmdResetQueryPool(VkCommandBuffer c, VkQueryPool pool, uint32_t first,
                                               uint32_t count)
{
    if (c->in_pass || first > pool->count || count > pool->count - first) {
        unsupported(c, "query reset range");
        return;
    }
    struct zvk_query_command *p = zvk_record(c, ZVK_RESET_QUERY, sizeof *p);
    if (p)
        *p = (struct zvk_query_command){pool, first, count};
}
VKAPI_ATTR void VKAPI_CALL vkCmdBeginQuery(VkCommandBuffer c, VkQueryPool pool, uint32_t index,
                                           VkQueryControlFlags flags)
{
    if (index >= pool->count || (flags & ~VK_QUERY_CONTROL_PRECISE_BIT) ||
        ((flags & VK_QUERY_CONTROL_PRECISE_BIT) &&
         !pool->device->enabled_features.occlusionQueryPrecise)) {
        unsupported(c, "occlusion query configuration");
        return;
    }
    struct zvk_query_command *p = zvk_record(c, ZVK_BEGIN_QUERY, sizeof *p);
    if (p)
        *p = (struct zvk_query_command){pool, index, 1};
}
VKAPI_ATTR void VKAPI_CALL vkCmdEndQuery(VkCommandBuffer c, VkQueryPool pool, uint32_t index)
{
    if (index >= pool->count) {
        unsupported(c, "query end range");
        return;
    }
    struct zvk_query_command *p = zvk_record(c, ZVK_END_QUERY, sizeof *p);
    if (p)
        *p = (struct zvk_query_command){pool, index, 1};
}
VKAPI_ATTR void VKAPI_CALL vkCmdCopyQueryPoolResults(VkCommandBuffer c, VkQueryPool pool,
                                                     uint32_t first, uint32_t count,
                                                     VkBuffer buffer, VkDeviceSize offset,
                                                     VkDeviceSize stride, VkQueryResultFlags flags)
{
    VkDeviceSize word = flags & VK_QUERY_RESULT_64_BIT ? 8 : 4,
                 bytes = word * (flags & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT ? 2 : 1);
    if (c->in_pass || first > pool->count || count > pool->count - first || (offset % word) ||
        (count > 1 && (stride < bytes || stride % word)) ||
        (count && (stride > UINT64_MAX / count ||
                   !zvk_range(buffer->size, offset, (count - 1) * stride + bytes)))) {
        unsupported(c, "query result copy range");
        return;
    }
    struct zvk_query_copy *p = zvk_record(c, ZVK_COPY_QUERY, sizeof *p);
    if (p)
        *p = (struct zvk_query_copy){pool, first, count, buffer, offset, stride, flags};
}
VKAPI_ATTR void VKAPI_CALL vkCmdWriteTimestamp(VkCommandBuffer c, VkPipelineStageFlagBits stage,
                                               VkQueryPool pool, uint32_t index)
{
    (void)stage;
    (void)pool;
    (void)index;
    unsupported(c, "timestamps: this queue advertises zero timestampValidBits");
}
VKAPI_ATTR void VKAPI_CALL vkCmdResolveImage(VkCommandBuffer c, VkImage src, VkImageLayout sl,
                                             VkImage dst, VkImageLayout dl, uint32_t count,
                                             const VkImageResolve *regions)
{
    if (c->in_pass || src->info.samples == VK_SAMPLE_COUNT_1_BIT ||
        dst->info.samples != VK_SAMPLE_COUNT_1_BIT || src->info.format != dst->info.format ||
        !zvk_aspect_bpp(src, VK_IMAGE_ASPECT_COLOR_BIT)) {
        unsupported(c, "resolve profile");
        return;
    }
    for (uint32_t i = 0; i < count; i++) {
        const VkImageResolve *r = regions + i;
        if (!subresource(src, &r->srcSubresource) || !subresource(dst, &r->dstSubresource) ||
            r->srcSubresource.layerCount != r->dstSubresource.layerCount ||
            r->srcSubresource.aspectMask != VK_IMAGE_ASPECT_COLOR_BIT ||
            r->dstSubresource.aspectMask != VK_IMAGE_ASPECT_COLOR_BIT || r->srcOffset.x < 0 ||
            r->srcOffset.y < 0 || r->dstOffset.x < 0 || r->dstOffset.y < 0 || r->srcOffset.z < 0 ||
            r->dstOffset.z < 0 || !r->extent.depth ||
            !zvk_range(zvk_mip(src->info.extent.depth, r->srcSubresource.mipLevel),
                       (uint32_t)r->srcOffset.z, r->extent.depth) ||
            !zvk_range(zvk_mip(dst->info.extent.depth, r->dstSubresource.mipLevel),
                       (uint32_t)r->dstOffset.z, r->extent.depth) ||
            !zvk_range(zvk_mip(src->info.extent.width, r->srcSubresource.mipLevel), r->srcOffset.x,
                       r->extent.width) ||
            !zvk_range(zvk_mip(src->info.extent.height, r->srcSubresource.mipLevel), r->srcOffset.y,
                       r->extent.height) ||
            !zvk_range(zvk_mip(dst->info.extent.width, r->dstSubresource.mipLevel), r->dstOffset.x,
                       r->extent.width) ||
            !zvk_range(zvk_mip(dst->info.extent.height, r->dstSubresource.mipLevel), r->dstOffset.y,
                       r->extent.height)) {
            unsupported(c, "resolve extent");
            return;
        }
        for (uint32_t l = 0; l < r->srcSubresource.layerCount; l++) {
            struct zvk_blit *p = zvk_record(c, ZVK_BLIT_IMAGE, sizeof *p);
            if (p) {
                *p = (struct zvk_blit){
                    .src = src,
                    .dst = dst,
                    .src_layout = sl,
                    .dst_layout = dl,
                    .filter = VK_FILTER_NEAREST,
                    .region = {.srcSubresource = r->srcSubresource,
                               .dstSubresource = r->dstSubresource,
                               .srcOffsets = {r->srcOffset,
                                              {r->srcOffset.x + (int32_t)r->extent.width,
                                               r->srcOffset.y + (int32_t)r->extent.height, 1}},
                               .dstOffsets = {r->dstOffset,
                                              {r->dstOffset.x + (int32_t)r->extent.width,
                                               r->dstOffset.y + (int32_t)r->extent.height, 1}}}};
                p->region.srcSubresource.baseArrayLayer += l;
                p->region.dstSubresource.baseArrayLayer += l;
                p->region.srcSubresource.layerCount = p->region.dstSubresource.layerCount = 1;
            }
        }
    }
}
