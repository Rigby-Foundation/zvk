/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (C) 2026 Rigby Foundation */
#include "zvk_internal.h"
#include <math.h>
#include "pipeline_cache.h"

static void layout_unref(VkDescriptorSetLayout layout)
{
    if (layout && __atomic_sub_fetch(&layout->refs, 1, __ATOMIC_ACQ_REL) == 0)
        zvk_free(&layout->alloc, layout);
}
static void pipeline_layout_unref(VkPipelineLayout layout)
{
    if (layout && __atomic_sub_fetch(&layout->refs, 1, __ATOMIC_ACQ_REL) == 0) {
        for (uint32_t i = 0; i < layout->nsets; i++)
            layout_unref(layout->sets[i]);
        zvk_free(&layout->alloc, layout);
    }
}
static void pass_unref(VkRenderPass pass)
{
    if (pass && __atomic_sub_fetch(&pass->refs, 1, __ATOMIC_ACQ_REL) == 0) {
        zvk_free(&pass->alloc, pass->subpasses);
        zvk_free(&pass->alloc, pass);
    }
}
static int binding_index(VkDescriptorSetLayout layout, uint32_t binding)
{
    for (uint32_t i = 0; i < layout->count; i++)
        if (layout->bindings[i].binding == binding)
            return (int)i;
    return -1;
}
VKAPI_ATTR VkResult VKAPI_CALL
vkCreateDescriptorSetLayout(VkDevice d, const VkDescriptorSetLayoutCreateInfo *info,
                            const VkAllocationCallbacks *allocator, VkDescriptorSetLayout *result)
{
    *result = VK_NULL_HANDLE;
    if (info->flags)return VK_ERROR_FEATURE_NOT_PRESENT;
    uint64_t descriptors=0;
    for (uint32_t i=0;i<info->bindingCount;i++) {
        const VkDescriptorSetLayoutBinding *b=info->pBindings+i;
        if (b->descriptorCount) {
            if (b->descriptorType>VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT)
                return VK_ERROR_FEATURE_NOT_PRESENT;
            descriptors+=b->descriptorCount;
            if (descriptors>UINT32_MAX)return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        for (uint32_t j=0;j<i;j++)
            if (info->pBindings[j].binding==b->binding)return VK_ERROR_INITIALIZATION_FAILED;
    }
    uint64_t bytes=sizeof(struct VkDescriptorSetLayout_T)+
        (uint64_t)info->bindingCount*(sizeof(VkDescriptorSetLayoutBinding)+2*sizeof(uint32_t))+
        descriptors*sizeof(VkSampler);
    if (bytes>SIZE_MAX)return VK_ERROR_OUT_OF_HOST_MEMORY;
    zvk_allocator a;
    zvk_allocator_init(&a, allocator);
    VkDescriptorSetLayout layout=zvk_alloc(&a,(size_t)bytes);
    if (!layout)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    layout->alloc = a;
    layout->device = d;
    layout->refs = 1;
    layout->count=info->bindingCount;layout->ndescriptors=descriptors;
    layout->bindings=(void *)(layout+1);
    layout->starts=(void *)(layout->bindings+layout->count);
    layout->dynamic=layout->starts+layout->count;
    layout->immutable=(void *)(layout->dynamic+layout->count);
    memcpy(layout->bindings, info->pBindings, info->bindingCount * sizeof *info->pBindings);
    for (uint32_t i = 1; i < layout->count; i++) {
        VkDescriptorSetLayoutBinding b = layout->bindings[i];
        uint32_t j = i;
        while (j && layout->bindings[j - 1].binding > b.binding) {
            layout->bindings[j] = layout->bindings[j - 1];
            j--;
        }
        layout->bindings[j] = b;
    }
    uint32_t start=0;
    for (uint32_t i=0;i<layout->count;i++) {
        VkDescriptorSetLayoutBinding *b=layout->bindings+i;
        layout->starts[i]=start;layout->dynamic[i]=UINT32_MAX;
        if (b->descriptorType==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC ||
            b->descriptorType==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC) {
            layout->dynamic[i]=layout->ndynamic;
            layout->ndynamic+=b->descriptorCount;
        }
        if (b->descriptorCount && (b->descriptorType==VK_DESCRIPTOR_TYPE_SAMPLER ||
             b->descriptorType==VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) && b->pImmutableSamplers) {
            memcpy(layout->immutable+start,b->pImmutableSamplers,b->descriptorCount*sizeof(VkSampler));
            b->pImmutableSamplers=layout->immutable+start;
        } else b->pImmutableSamplers=NULL;
        start+=b->descriptorCount;
    }
    *result = layout;
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyDescriptorSetLayout(VkDevice d, VkDescriptorSetLayout layout,
                                                        const VkAllocationCallbacks *allocator)
{
    (void)d;
    (void)allocator;
    layout_unref(layout);
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreatePipelineLayout(VkDevice d,
                                                      const VkPipelineLayoutCreateInfo *info,
                                                      const VkAllocationCallbacks *allocator,
                                                      VkPipelineLayout *result)
{
    *result = VK_NULL_HANDLE;
    if (info->flags || info->setLayoutCount > ZVK_SETS)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    uint32_t bytes = 0;
    for (uint32_t i = 0; i < info->pushConstantRangeCount; i++) {
        const VkPushConstantRange *r = info->pPushConstantRanges + i;
        if ((r->offset | r->size) & 3 || !zvk_range(ZVK_PUSH, r->offset, r->size))
            return VK_ERROR_FEATURE_NOT_PRESENT;
        if (r->offset + r->size > bytes)
            bytes = r->offset + r->size;
    }
    zvk_allocator a;
    zvk_allocator_init(&a, allocator);
    VkPipelineLayout layout = zvk_alloc(&a, sizeof *layout);
    if (!layout)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    layout->alloc = a;
    layout->device = d;
    layout->refs = 1;
    layout->nsets = info->setLayoutCount;
    layout->push_bytes = bytes;
    for (uint32_t i = 0; i < layout->nsets; i++) {
        layout->sets[i] = info->pSetLayouts[i];
        __atomic_add_fetch(&layout->sets[i]->refs, 1, __ATOMIC_RELAXED);
    }
    *result = layout;
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyPipelineLayout(VkDevice d, VkPipelineLayout layout,
                                                   const VkAllocationCallbacks *allocator)
{
    (void)d;
    (void)allocator;
    pipeline_layout_unref(layout);
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateDescriptorPool(VkDevice d,
                                                      const VkDescriptorPoolCreateInfo *info,
                                                      const VkAllocationCallbacks *allocator,
                                                      VkDescriptorPool *result)
{
    *result = VK_NULL_HANDLE;
    if (info->flags & ~VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    zvk_allocator a;
    zvk_allocator_init(&a, allocator);
    VkDescriptorPool pool = zvk_alloc(&a, sizeof *pool);
    if (!pool)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    pool->alloc = a;
    pool->device = d;
    pool->max_sets = info->maxSets;
    for (uint32_t i = 0; i < info->poolSizeCount; i++) {
        VkDescriptorType t = info->pPoolSizes[i].type;
        uint32_t n = info->pPoolSizes[i].descriptorCount;
        if ((t != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER &&
             t != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC &&
             t != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER &&
             t != VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE && t != VK_DESCRIPTOR_TYPE_SAMPLER &&
             t != VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT && t != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER &&
             t != VK_DESCRIPTOR_TYPE_STORAGE_IMAGE &&
             t != VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER &&
             t != VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER &&
             t != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC) ||
            n > UINT32_MAX - pool->capacity[t]) {
            zvk_free(&a, pool);
            return VK_ERROR_FEATURE_NOT_PRESENT;
        }
        pool->capacity[t] += n;
    }
    *result = pool;
    return VK_SUCCESS;
}
static void free_set(VkDescriptorPool pool, VkDescriptorSet set)
{
    VkDescriptorSet *link = &pool->sets;
    while (*link && *link != set)
        link = &(*link)->next;
    if (!*link)
        return;
    *link = set->next;
    for (uint32_t i = 0; i < set->layout->count; i++) {
        VkDescriptorSetLayoutBinding *b = set->layout->bindings + i;
        if (b->descriptorCount)pool->used[b->descriptorType]-=b->descriptorCount;
    }
    layout_unref(set->layout);
    pool->allocated--;
    zvk_free(&pool->alloc, set);
}
VKAPI_ATTR VkResult VKAPI_CALL vkResetDescriptorPool(VkDevice d, VkDescriptorPool pool,
                                                     VkDescriptorPoolResetFlags flags)
{
    (void)d;
    (void)flags;
    while (pool->sets)
        free_set(pool, pool->sets);
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyDescriptorPool(VkDevice d, VkDescriptorPool pool,
                                                   const VkAllocationCallbacks *allocator)
{
    (void)allocator;
    if (pool) {
        vkResetDescriptorPool(d, pool, 0);
        zvk_free(&pool->alloc, pool);
    }
}
VKAPI_ATTR VkResult VKAPI_CALL vkAllocateDescriptorSets(VkDevice d,
                                                        const VkDescriptorSetAllocateInfo *info,
                                                        VkDescriptorSet *sets)
{
    (void)d;
    VkDescriptorPool pool = info->descriptorPool;
    uint32_t total[11] = {0};
    for (uint32_t i = 0; i < info->descriptorSetCount; i++)
        sets[i] = VK_NULL_HANDLE;
    if (info->descriptorSetCount > pool->max_sets - pool->allocated)
        return VK_ERROR_OUT_OF_POOL_MEMORY;
    for (uint32_t i = 0; i < info->descriptorSetCount; i++)
        for (uint32_t j = 0; j < info->pSetLayouts[i]->count; j++) {
            VkDescriptorSetLayoutBinding *b = info->pSetLayouts[i]->bindings + j;
            if (!b->descriptorCount)continue;
            if (b->descriptorCount > UINT32_MAX - total[b->descriptorType])
                return VK_ERROR_OUT_OF_POOL_MEMORY;
            total[b->descriptorType] += b->descriptorCount;
        }
    for (int i = 0; i < 11; i++)
        if (total[i] > pool->capacity[i] - pool->used[i])
            return VK_ERROR_OUT_OF_POOL_MEMORY;
    for (uint32_t i = 0; i < info->descriptorSetCount; i++) {
        VkDescriptorSetLayout set_layout=info->pSetLayouts[i];
        uint64_t bytes=sizeof(struct VkDescriptorSet_T)+(uint64_t)set_layout->ndescriptors*sizeof(zvk_descriptor);
        VkDescriptorSet set=bytes>SIZE_MAX?NULL:zvk_alloc(&pool->alloc,bytes);
        if (!set) {
            for (uint32_t j = 0; j < i; j++) {
                free_set(pool, sets[j]);
                sets[j] = VK_NULL_HANDLE;
            }
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        set->pool = pool;
        set->layout=info->pSetLayouts[i];set->descriptors=(void *)(set+1);
        __atomic_add_fetch(&set->layout->refs, 1, __ATOMIC_RELAXED);
        for (uint32_t j = 0; j < set->layout->count; j++) {
            VkDescriptorSetLayoutBinding *b = set->layout->bindings + j;
            if (!b->descriptorCount)continue;
            pool->used[b->descriptorType]+=b->descriptorCount;
            for (uint32_t k = 0; k < b->descriptorCount; k++) {
                set->descriptors[set->layout->starts[j]+k].type = b->descriptorType;
                if (b->pImmutableSamplers)
                    set->descriptors[set->layout->starts[j]+k].sampler = b->pImmutableSamplers[k];
            }
        }
        set->next = pool->sets;
        pool->sets = set;
        pool->allocated++;
        sets[i] = set;
    }
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkFreeDescriptorSets(VkDevice d, VkDescriptorPool pool,
                                                    uint32_t count, const VkDescriptorSet *sets)
{
    (void)d;
    for (uint32_t i = 0; i < count; i++)
        if (sets[i])
            free_set(pool, sets[i]);
    return VK_SUCCESS;
}
static zvk_descriptor *descriptor_at(VkDescriptorSet set, uint32_t *binding, uint32_t *element,
                                     VkDescriptorType type)
{
    int ix = binding_index(set->layout, *binding);
    if (ix < 0)
        return NULL;
    while (*element >= set->layout->bindings[ix].descriptorCount) {
        *element -= set->layout->bindings[ix].descriptorCount;
        ix++;
        if (ix >= (int)set->layout->count)
            return NULL;
        *binding = set->layout->bindings[ix].binding;
    }
    if (set->layout->bindings[ix].descriptorType != type)
        return NULL;
    return set->descriptors+set->layout->starts[ix]+*element;
}
VKAPI_ATTR void VKAPI_CALL vkUpdateDescriptorSets(VkDevice d, uint32_t nwrite,
                                                  const VkWriteDescriptorSet *writes,
                                                  uint32_t ncopy, const VkCopyDescriptorSet *copies)
{
    (void)d;
    for (uint32_t i = 0; i < nwrite; i++) {
        const VkWriteDescriptorSet *w = writes + i;
        uint32_t binding = w->dstBinding, element = w->dstArrayElement;
        for (uint32_t j = 0; j < w->descriptorCount; j++, element++) {
            zvk_descriptor *dst = descriptor_at(w->dstSet, &binding, &element, w->descriptorType);
            if (!dst) {
                zvk_error("descriptor write outside compatible bindings");
                return;
            }
            if (w->descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
                w->descriptorType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ||
                w->descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER ||
                w->descriptorType == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT ||
                w->descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) {
                VkDescriptorSetLayoutBinding *layout_binding =
                    w->dstSet->layout->bindings + binding_index(w->dstSet->layout, binding);
                dst->sampler = layout_binding->pImmutableSamplers
                                   ? layout_binding->pImmutableSamplers[element]
                               : w->descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
                                       w->descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER
                                   ? w->pImageInfo[j].sampler
                                   : VK_NULL_HANDLE;
                dst->image = w->descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER
                                 ? VK_NULL_HANDLE
                                 : w->pImageInfo[j].imageView;
                dst->layout = w->pImageInfo[j].imageLayout;
            } else if (w->descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER ||
                       w->descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) {
                dst->texel = w->pTexelBufferView[j];
            } else {
                dst->buffer = w->pBufferInfo[j].buffer;
                dst->offset = w->pBufferInfo[j].offset;
                dst->range = w->pBufferInfo[j].range;
            }
        }
    }
    for (uint32_t i = 0; i < ncopy; i++) {
        const VkCopyDescriptorSet *w = copies + i;
        int ix = binding_index(w->srcSet->layout, w->srcBinding);
        if (ix < 0) {
            zvk_error("invalid descriptor copy binding");
            return;
        }
        VkDescriptorType type = w->srcSet->layout->bindings[ix].descriptorType;
        uint32_t sb = w->srcBinding, se = w->srcArrayElement, db = w->dstBinding,
                 de = w->dstArrayElement;
        for (uint32_t j = 0; j < w->descriptorCount; j++, se++, de++) {
            zvk_descriptor *src = descriptor_at(w->srcSet, &sb, &se, type),
                           *dst = descriptor_at(w->dstSet, &db, &de, type);
            if (!src || !dst) {
                zvk_error("descriptor copy outside compatible bindings");
                return;
            }
            VkSampler immutable = dst->sampler;
            *dst = *src;
            if (w->dstSet->layout->bindings[binding_index(w->dstSet->layout, db)]
                    .pImmutableSamplers)
                dst->sampler = immutable;
        }
    }
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateRenderPass(VkDevice d, const VkRenderPassCreateInfo *info,
                                                  const VkAllocationCallbacks *allocator,
                                                  VkRenderPass *result)
{
    *result = VK_NULL_HANDLE;
    if (info->flags || !info->subpassCount)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    for (uint32_t i = 0; i < info->attachmentCount; i++)
        if (!zvk_format(info->pAttachments[i].format) ||
            !(zvk_samples(d->physical, info->pAttachments[i].format) &
              info->pAttachments[i].samples) ||
            !info->pAttachments[i].samples ||
            (info->pAttachments[i].samples & (info->pAttachments[i].samples - 1)))
            return VK_ERROR_FORMAT_NOT_SUPPORTED;
    for (uint32_t i = 0; i < info->dependencyCount; i++) {
        const VkSubpassDependency *dep = info->pDependencies + i;
        if ((dep->srcSubpass != VK_SUBPASS_EXTERNAL && dep->srcSubpass >= info->subpassCount) ||
            (dep->dstSubpass != VK_SUBPASS_EXTERNAL && dep->dstSubpass >= info->subpassCount))
            return VK_ERROR_INITIALIZATION_FAILED;
    }
    zvk_allocator a;
    zvk_allocator_init(&a, allocator);
    size_t attachment_bytes = (size_t)info->attachmentCount *
                              (sizeof(VkAttachmentDescription) + sizeof(uint32_t));
    if (attachment_bytes > SIZE_MAX - sizeof(struct VkRenderPass_T))
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    VkRenderPass pass = zvk_alloc(&a, sizeof *pass + attachment_bytes);
    if (!pass)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    pass->alloc = a;
    pass->device = d;
    pass->refs = 1;
    pass->count = info->attachmentCount;
    pass->attachments = (void *)(pass + 1);
    pass->first_use = (void *)(pass->attachments + pass->count);
    pass->nsubpasses = info->subpassCount;
    size_t subpass_stride = sizeof *pass->subpasses + (size_t)pass->count * sizeof(VkImageLayout);
    size_t subpass_bytes = (size_t)info->subpassCount * subpass_stride;
    if (subpass_bytes / subpass_stride != info->subpassCount) {
        pass_unref(pass);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    pass->subpasses = zvk_alloc(&a, subpass_bytes);
    if (!pass->subpasses) {
        pass_unref(pass);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    if (pass->count)
        memcpy(pass->attachments, info->pAttachments, pass->count * sizeof *info->pAttachments);
    for (uint32_t i = 0; i < pass->count; i++)
        pass->first_use[i] = UINT32_MAX;
    VkResult error = VK_ERROR_INITIALIZATION_FAILED;
    for (uint32_t j = 0; j < pass->nsubpasses; j++) {
        const VkSubpassDescription *s = info->pSubpasses + j;
        struct zvk_subpass *out = pass->subpasses + j;
        out->layouts = (VkImageLayout *)(pass->subpasses + pass->nsubpasses) + (size_t)j * pass->count;
        if (s->flags || s->pipelineBindPoint != VK_PIPELINE_BIND_POINT_GRAPHICS ||
            s->colorAttachmentCount > ZVK_COLORS || s->inputAttachmentCount > ZVK_ATTACHMENTS) {
            error = VK_ERROR_FEATURE_NOT_PRESENT;
            goto fail;
        }
        out->ncolors = s->colorAttachmentCount;
        out->ninputs = s->inputAttachmentCount;
        out->depth = s->pDepthStencilAttachment ? s->pDepthStencilAttachment->attachment
                                                : VK_ATTACHMENT_UNUSED;
        for (uint32_t k = 0; k < 4; k++) {
            const VkAttachmentReference *refs = k == 0   ? s->pColorAttachments
                                                : k == 1 ? s->pResolveAttachments
                                                : k == 2 ? s->pInputAttachments
                                                         : s->pDepthStencilAttachment;
            uint32_t count = k < 2 ? out->ncolors : k == 2 ? out->ninputs : 1;
            for (uint32_t i = 0; i < count; i++) {
                uint32_t attachment = refs ? refs[i].attachment : VK_ATTACHMENT_UNUSED;
                if (k == 0)
                    out->colors[i] = attachment;
                if (k == 1)
                    out->resolves[i] = attachment;
                if (k == 2)
                    out->inputs[i] = attachment;
                if (attachment == VK_ATTACHMENT_UNUSED)
                    continue;
                if (attachment >= pass->count)
                    goto fail;
                if ((out->layouts[attachment] != VK_IMAGE_LAYOUT_UNDEFINED) && out->layouts[attachment] != refs[i].layout)
                    goto fail;
                out->layouts[attachment] = refs[i].layout;
                if (pass->first_use[attachment] == UINT32_MAX)
                    pass->first_use[attachment] = j;
                if (k == 0 || k == 3) {
                    VkSampleCountFlagBits samples = pass->attachments[attachment].samples;
                    if (out->samples && samples != out->samples)
                        goto fail;
                    out->samples = samples;
                }
            }
        }
        for (uint32_t i = 0; i < out->ncolors; i++) {
            uint32_t color = out->colors[i], resolve = out->resolves[i];
            if (resolve != VK_ATTACHMENT_UNUSED &&
                (color == VK_ATTACHMENT_UNUSED ||
                 pass->attachments[color].samples == VK_SAMPLE_COUNT_1_BIT ||
                 pass->attachments[resolve].samples != VK_SAMPLE_COUNT_1_BIT ||
                 pass->attachments[resolve].format != pass->attachments[color].format))
                goto fail;
        }
        for (uint32_t i = 0; i < s->preserveAttachmentCount; i++)
            if (s->pPreserveAttachments[i] >= pass->count ||
                (out->layouts[s->pPreserveAttachments[i]] != VK_IMAGE_LAYOUT_UNDEFINED))
                goto fail;
        if (!out->samples)
            out->samples = VK_SAMPLE_COUNT_1_BIT;
    }
    *result = pass;
    return VK_SUCCESS;
fail:
    pass_unref(pass);
    return error;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyRenderPass(VkDevice d, VkRenderPass pass,
                                               const VkAllocationCallbacks *allocator)
{
    (void)d;
    (void)allocator;
    pass_unref(pass);
}
VKAPI_ATTR void VKAPI_CALL vkGetRenderAreaGranularity(VkDevice d, VkRenderPass pass,
                                                      VkExtent2D *extent)
{
    (void)d;
    (void)pass;
    *extent = (VkExtent2D){1, 1};
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateFramebuffer(VkDevice d, const VkFramebufferCreateInfo *info,
                                                   const VkAllocationCallbacks *allocator,
                                                   VkFramebuffer *result)
{
    *result = VK_NULL_HANDLE;
    if (info->flags || !info->layers || info->layers > 256 || !info->width || !info->height ||
        info->width > 4096 || info->height > 4096 ||
        info->attachmentCount != info->renderPass->count)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    for (uint32_t i = 0; i < info->attachmentCount; i++)
        if (info->pAttachments[i]->format != info->renderPass->attachments[i].format ||
            info->pAttachments[i]->image->info.samples !=
                info->renderPass->attachments[i].samples ||
            zvk_mip(info->pAttachments[i]->image->info.extent.width,
                    info->pAttachments[i]->range.baseMipLevel) < info->width ||
            zvk_mip(info->pAttachments[i]->image->info.extent.height,
                    info->pAttachments[i]->range.baseMipLevel) < info->height ||
            info->pAttachments[i]->range.levelCount != 1 ||
            info->pAttachments[i]->range.layerCount < info->layers ||
            info->pAttachments[i]->target==PIPE_TEXTURE_3D)
            return VK_ERROR_INITIALIZATION_FAILED;
    zvk_allocator a;
    zvk_allocator_init(&a, allocator);
    size_t view_bytes = (size_t)info->attachmentCount * sizeof(VkImageView);
    if (view_bytes > SIZE_MAX - sizeof(struct VkFramebuffer_T))
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    VkFramebuffer fb = zvk_alloc(&a, sizeof *fb + view_bytes);
    if (!fb)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    fb->alloc = a;
    fb->device = d;
    fb->pass = info->renderPass;
    __atomic_add_fetch(&fb->pass->refs, 1, __ATOMIC_RELAXED);
    fb->count = info->attachmentCount;
    fb->views = (void *)(fb + 1);
    fb->width = info->width;
    fb->height = info->height;
    fb->layers = info->layers;
    memcpy(fb->views, info->pAttachments, fb->count * sizeof *info->pAttachments);
    *result = fb;
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyFramebuffer(VkDevice d, VkFramebuffer fb,
                                                const VkAllocationCallbacks *allocator)
{
    (void)d;
    (void)allocator;
    if (fb) {
        pass_unref(fb->pass);
        zvk_free(&fb->alloc, fb);
    }
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreatePipelineCache(VkDevice d,
                                                     const VkPipelineCacheCreateInfo *info,
                                                     const VkAllocationCallbacks *allocator,
                                                     VkPipelineCache *result)
{
    *result=VK_NULL_HANDLE;
    if (info->flags & ~(d->cache_control?VK_PIPELINE_CACHE_CREATE_EXTERNALLY_SYNCHRONIZED_BIT:0))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    zvk_allocator a;
    zvk_allocator_init(&a, allocator);
    *result = zvk_alloc(&a, sizeof **result);
    if (!*result)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    (*result)->alloc = a;
    (*result)->device = d;
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyPipelineCache(VkDevice d, VkPipelineCache cache,
                                                  const VkAllocationCallbacks *allocator)
{
    (void)d;
    (void)allocator;
    if (cache) {
        pthread_mutex_lock(&d->mutex);
        for (struct zvk_cache_link *link=cache->entries;link;) {
            struct zvk_cache_link *next=link->next;cache_release(link->shader);
            zvk_free(&cache->alloc,link);link=next;
        }
        pthread_mutex_unlock(&d->mutex);
        zvk_free(&cache->alloc, cache);
    }
}
VKAPI_ATTR VkResult VKAPI_CALL vkGetPipelineCacheData(VkDevice d, VkPipelineCache cache,
                                                      size_t *size, void *data)
{
    (void)cache;
    VkPipelineCacheHeaderVersionOne header = {
        .headerSize = sizeof header, .headerVersion = VK_PIPELINE_CACHE_HEADER_VERSION_ONE};
    memcpy(header.pipelineCacheUUID, "zvk-virgl-v1", 12);
    if (!data) {
        *size = sizeof header;
        return VK_SUCCESS;
    }
    if (*size < sizeof header) {
        *size = 0;
        return VK_INCOMPLETE;
    }
    memcpy(data, &header, sizeof header);
    *size = sizeof header;
    (void)d;
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkMergePipelineCaches(VkDevice d, VkPipelineCache dst,
                                                     uint32_t count, const VkPipelineCache *src)
{
    pthread_mutex_lock(&d->mutex);
    for (unsigned i=0;i<count;i++) {
        for (struct zvk_cache_link *link=src[i]->entries;link;link=link->next) {
            struct zvk_cached_shader *entry=link->shader;
            struct zvk_cache_link *existing=dst->entries;
            while (existing && (existing->shader->key_size!=entry->key_size ||
                   memcmp(existing->shader->key,entry->key,entry->key_size)))existing=existing->next;
            if (existing)continue;
            struct zvk_cache_link *copy=zvk_alloc(&dst->alloc,sizeof *copy);
            if (!copy){pthread_mutex_unlock(&d->mutex);return VK_ERROR_OUT_OF_HOST_MEMORY;}
            entry->refs++;copy->shader=entry;copy->next=dst->entries;dst->entries=copy;
        }
    }
    pthread_mutex_unlock(&d->mutex);
    return VK_SUCCESS;
}
static VkResult shader_layout(VkPipelineLayout layout, zvk_shader *shader)
{
    if (shader->push_bytes > layout->push_bytes) {
        zvk_error("shader push constants exceed pipeline layout");
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    for (uint32_t i = 0; i < shader->nbindings; i++) {
        zvk_shader_binding *b = shader->bindings + i;
        if (b->set >= layout->nsets) {
            zvk_error("shader descriptor set is absent from pipeline layout");
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        if (b->separate_sampler) {
            if (b->sampler_set >= layout->nsets)
                return VK_ERROR_INITIALIZATION_FAILED;
            VkDescriptorSetLayout samplers = layout->sets[b->sampler_set];
            int sampler = binding_index(samplers, b->sampler_binding);
            if (sampler>=0)b->sampler_index=samplers->starts[sampler]+b->sampler_element;
            if (sampler < 0 ||
                (samplers->bindings[sampler].descriptorType != VK_DESCRIPTOR_TYPE_SAMPLER &&
                 samplers->bindings[sampler].descriptorType != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) ||
                b->sampler_element >= samplers->bindings[sampler].descriptorCount)
                return VK_ERROR_INITIALIZATION_FAILED;
        }
        int ix = binding_index(layout->sets[b->set], b->binding);
        if (ix < 0)
            return VK_ERROR_INITIALIZATION_FAILED;
        if (b->element >= layout->sets[b->set]->bindings[ix].descriptorCount)
            return VK_ERROR_INITIALIZATION_FAILED;
        VkDescriptorSetLayout set_layout=layout->sets[b->set];
        b->index=set_layout->starts[ix]+b->element;
        b->dynamic_index=set_layout->dynamic[ix]==UINT32_MAX?UINT32_MAX:set_layout->dynamic[ix]+b->element;
        VkDescriptorType t=set_layout->bindings[ix].descriptorType;
        /* Combined descriptors may be accessed through separate SPIR-V
         * image and sampler variables at the same set/binding. */
        if (b->type==VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE && t==VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)
            b->type=t;
        if (t != b->type &&
            !(b->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER &&
              t == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC) &&
            !(b->type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER &&
              t == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC))
            return VK_ERROR_INITIALIZATION_FAILED;
    }
    for (unsigned i=0;i<shader->nsamplers;i++) {
        zvk_shader_binding *b=shader->samplers+i;
        if (b->set>=layout->nsets)return VK_ERROR_INITIALIZATION_FAILED;
        VkDescriptorSetLayout set=layout->sets[b->set];int ix=binding_index(set,b->binding);
        if (ix<0 || b->element>=set->bindings[ix].descriptorCount ||
            (set->bindings[ix].descriptorType!=VK_DESCRIPTOR_TYPE_SAMPLER &&
             set->bindings[ix].descriptorType!=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER))
            return VK_ERROR_INITIALIZATION_FAILED;
        b->index=set->starts[ix]+b->element;
    }
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyPipeline(VkDevice d, VkPipeline p,
                                             const VkAllocationCallbacks *allocator)
{
    (void)allocator;
    if (!p)
        return;
    pthread_mutex_lock(&d->mutex);
    if (p->h_cs && !p->cached[2])
        virgl_metal_destroy(d->gpu, p->h_cs);
    if (p->h_vs && !p->cached[0])
        virgl_destroy_object(d->gpu, p->h_vs, VIRGL_OBJECT_SHADER);
    if (p->h_fs && !p->cached[1])
        virgl_destroy_object(d->gpu, p->h_fs, VIRGL_OBJECT_SHADER);
    if (p->h_blend)
        virgl_destroy_object(d->gpu, p->h_blend, VIRGL_OBJECT_BLEND);
    if (p->h_rs)
        virgl_destroy_object(d->gpu, p->h_rs, VIRGL_OBJECT_RASTERIZER);
    if (p->h_dsa)
        virgl_destroy_object(d->gpu, p->h_dsa, VIRGL_OBJECT_DSA);
    if (p->h_ve)
        virgl_destroy_object(d->gpu, p->h_ve, VIRGL_OBJECT_VERTEX_ELEMENTS);
    for (unsigned i=0;i<3;i++)cache_release(p->cached[i]);
    pthread_mutex_unlock(&d->mutex);
    if (!p->cached[2])zvk_shader_finish(&p->cs);
    if (!p->cached[0])zvk_shader_finish(&p->vs);
    if (!p->cached[1])zvk_shader_finish(&p->fs);
    pipeline_layout_unref(p->layout);
    pass_unref(p->pass);
    zvk_free(&p->alloc, p);
}
static int dynamic_state(const VkGraphicsPipelineCreateInfo *info, VkDynamicState state)
{
    if (info->pDynamicState)
        for (uint32_t i = 0; i < info->pDynamicState->dynamicStateCount; i++)
            if (info->pDynamicState->pDynamicStates[i] == state)
                return 1;
    return 0;
}
static VkResult create_pipeline(VkDevice d, VkPipelineCache cache,const VkGraphicsPipelineCreateInfo *info,
                                const VkAllocationCallbacks *allocator, VkPipeline *result)
{
    *result = VK_NULL_HANDLE;
    if (!info->pRasterizationState || !info->renderPass || info->subpass>=info->renderPass->nsubpasses)
        return VK_ERROR_INITIALIZATION_FAILED;
    const struct zvk_subpass *sub=info->renderPass->subpasses+info->subpass;
    VkGraphicsPipelineCreateInfo adjusted=*info;
    VkPipelineMultisampleStateCreateInfo multisample={.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                                      .rasterizationSamples=sub->samples};
    VkViewport viewport={0,0,1,1,0,1};VkRect2D scissor={{0,0},{1,1}};
    VkPipelineViewportStateCreateInfo viewport_state={.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount=1,.pViewports=&viewport,.scissorCount=1,.pScissors=&scissor};
    VkPipelineRasterizationStateCreateInfo raster=*info->pRasterizationState;
    if (raster.rasterizerDiscardEnable) {
        adjusted.pMultisampleState=&multisample;
        adjusted.pViewportState=&viewport_state;
        adjusted.pColorBlendState=NULL;adjusted.pDepthStencilState=NULL;
        raster.depthClampEnable=VK_FALSE;raster.depthBiasEnable=VK_FALSE;raster.polygonMode=VK_POLYGON_MODE_FILL;
        raster.lineWidth=1;adjusted.pRasterizationState=&raster;
    }
    if (!sub->ncolors)adjusted.pColorBlendState=NULL;
    if (sub->depth==VK_ATTACHMENT_UNUSED)adjusted.pDepthStencilState=NULL;
    info=&adjusted;

    if ((info->flags &
         ~(VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT | VK_PIPELINE_CREATE_ALLOW_DERIVATIVES_BIT |
           VK_PIPELINE_CREATE_DERIVATIVE_BIT |
           (d->cache_control?(VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT |
                             VK_PIPELINE_CREATE_EARLY_RETURN_ON_FAILURE_BIT):0))) ||
        !info->pVertexInputState || !info->pInputAssemblyState || !info->pRasterizationState ||
        info->pRasterizationState->depthClampEnable ||
        info->pRasterizationState->polygonMode != VK_POLYGON_MODE_FILL ||
        (!dynamic_state(info, VK_DYNAMIC_STATE_LINE_WIDTH) &&
         info->pRasterizationState->lineWidth != 1) ||
        (info->pRasterizationState->depthBiasEnable &&
         !dynamic_state(info, VK_DYNAMIC_STATE_DEPTH_BIAS) &&
         info->pRasterizationState->depthBiasClamp != 0 && !d->enabled_features.depthBiasClamp) ||
        info->subpass >= info->renderPass->nsubpasses)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    if (info->pInputAssemblyState->topology > VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN ||
        info->pVertexInputState->vertexAttributeDescriptionCount > ZVK_ATTRS ||
        info->pVertexInputState->vertexBindingDescriptionCount > ZVK_ATTRS)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    if (!info->pMultisampleState ||
        ((sub->ncolors || sub->depth != VK_ATTACHMENT_UNUSED) &&
         info->pMultisampleState->rasterizationSamples != sub->samples) ||
        !(zvk_samples(d->physical, VK_FORMAT_R8G8B8A8_UNORM) &
          info->pMultisampleState->rasterizationSamples) ||
        !info->pMultisampleState->rasterizationSamples ||
        (info->pMultisampleState->rasterizationSamples &
         (info->pMultisampleState->rasterizationSamples - 1)) ||
        info->pMultisampleState->sampleShadingEnable || info->pMultisampleState->alphaToOneEnable)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    if (info->pDepthStencilState && info->pDepthStencilState->depthBoundsTestEnable)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    if (info->pColorBlendState && (info->pColorBlendState->logicOpEnable ||
                                   info->pColorBlendState->attachmentCount > ZVK_COLORS))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    zvk_allocator a;
    zvk_allocator_init(&a, allocator);
    VkPipeline p = zvk_alloc(&a, sizeof *p);
    if (!p)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    p->samples = info->pMultisampleState->rasterizationSamples;
    p->alpha_coverage = info->pMultisampleState->alphaToCoverageEnable;
    p->sample_mask =
        info->pMultisampleState->pSampleMask ? info->pMultisampleState->pSampleMask[0] : ~0u;
    p->alloc = a;
    p->device = d;
    p->layout = info->layout;
    __atomic_add_fetch(&p->layout->refs, 1, __ATOMIC_RELAXED);
    p->pass = info->renderPass;
    p->subpass = info->subpass;
    __atomic_add_fetch(&p->pass->refs, 1, __ATOMIC_RELAXED);
    p->topology = info->pInputAssemblyState->topology;
    p->restart = info->pInputAssemblyState->primitiveRestartEnable;
    p->raster = *info->pRasterizationState;
    p->raster.pNext = NULL;
    if (info->pDepthStencilState) {
        p->depth = *info->pDepthStencilState;
        p->depth.pNext = NULL;
    }
    if (info->pColorBlendState) {
        if (info->pColorBlendState->attachmentCount != p->pass->subpasses[p->subpass].ncolors) {
            vkDestroyPipeline(d, p, NULL);
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        for (uint32_t i = 0; i < p->pass->subpasses[p->subpass].ncolors; i++) {
            p->blend[i] = info->pColorBlendState->pAttachments[i];
            if (i && !d->enabled_features.independentBlend &&
                memcmp(&p->blend[0], &p->blend[i], sizeof p->blend[i])) {
                vkDestroyPipeline(d, p, NULL);
                return VK_ERROR_FEATURE_NOT_PRESENT;
            }
        }
        memcpy(p->blend_constants, info->pColorBlendState->blendConstants,
               sizeof p->blend_constants);
    } else
        for (uint32_t i = 0; i < p->pass->subpasses[p->subpass].ncolors; i++)
            p->blend[i].colorWriteMask = 15;
    p->nattrs = info->pVertexInputState->vertexAttributeDescriptionCount;
    p->nbindings = info->pVertexInputState->vertexBindingDescriptionCount;
    memcpy(p->attrs, info->pVertexInputState->pVertexAttributeDescriptions,
           p->nattrs * sizeof *p->attrs);
    memcpy(p->bindings, info->pVertexInputState->pVertexBindingDescriptions,
           p->nbindings * sizeof *p->bindings);
    VkResult r = VK_SUCCESS;
    for (uint32_t i = 0; i < p->nattrs; i++)
        if (p->attrs[i].location >= ZVK_ATTRS || p->attrs[i].binding >= ZVK_ATTRS ||
            !zvk_format(p->attrs[i].format) || p->attrs[i].offset > 2047) {
            r = VK_ERROR_FORMAT_NOT_SUPPORTED;
            goto error;
        }
    for (uint32_t i = 0; i < p->nbindings; i++)
        if (p->bindings[i].binding >= ZVK_ATTRS || p->bindings[i].stride > 2048) {
            r = VK_ERROR_FEATURE_NOT_PRESENT;
            goto error;
        }
    if (info->pDynamicState)
        for (uint32_t i = 0; i < info->pDynamicState->dynamicStateCount; i++)
            switch (info->pDynamicState->pDynamicStates[i]) {
            case VK_DYNAMIC_STATE_LINE_WIDTH:
                p->dynamic_line_width = 1;
                break;
            case VK_DYNAMIC_STATE_DEPTH_BIAS:
                p->dynamic_depth_bias = 1;
                break;
            case VK_DYNAMIC_STATE_VIEWPORT:
                p->dynamic_viewport = 1;
                break;
            case VK_DYNAMIC_STATE_SCISSOR:
                p->dynamic_scissor = 1;
                break;
            case VK_DYNAMIC_STATE_DEPTH_BOUNDS:
                break;
            case VK_DYNAMIC_STATE_BLEND_CONSTANTS:
                p->dynamic_blend = 1;
                break;
            case VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK:
                p->dynamic_stencil_compare = 1;
                break;
            case VK_DYNAMIC_STATE_STENCIL_WRITE_MASK:
                p->dynamic_stencil_write = 1;
                break;
            case VK_DYNAMIC_STATE_STENCIL_REFERENCE:
                p->dynamic_stencil_reference = 1;
                break;
            default:
                r = VK_ERROR_FEATURE_NOT_PRESENT;
                goto error;
            }
    if (p->dynamic_line_width)
        p->raster.lineWidth = 1;
    if (p->dynamic_depth_bias || !p->raster.depthBiasEnable) {
        p->raster.depthBiasConstantFactor = 0;
        p->raster.depthBiasSlopeFactor = 0;
        p->raster.depthBiasClamp = 0;
    }
    if (!info->pViewportState || info->pViewportState->viewportCount != 1 ||
        info->pViewportState->scissorCount != 1) {
        r = VK_ERROR_FEATURE_NOT_PRESENT;
        goto error;
    }
    if (!p->dynamic_viewport)
        p->viewport = info->pViewportState->pViewports[0];
    if (!p->dynamic_scissor)
        p->scissor = info->pViewportState->pScissors[0];
    /* A float-backed native D24 buffer may use a depth-dependent offset unit.
     * Fixed-point Vulkan attachments require a constant unit. Lower bias on
     * single-sample triangles before any shader reads FragCoord. */
    const struct zvk_subpass *bias_sub=p->pass->subpasses+p->subpass;
    if(p->raster.depthBiasEnable && !d->enabled_features.depthBiasClamp &&
       p->samples==VK_SAMPLE_COUNT_1_BIT &&
       p->topology>=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST &&
       p->topology<=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN && bias_sub->depth!=VK_ATTACHMENT_UNUSED) {
        VkFormat format=p->pass->attachments[bias_sub->depth].format;
        p->fixed_depth_bias_bits=format==VK_FORMAT_D16_UNORM?16:
                                 format==VK_FORMAT_D24_UNORM_S8_UINT?24:0;
    }
    int vs = 0, fs = 0;
    for (uint32_t i = 0; i < info->stageCount; i++) {
        const VkPipelineShaderStageCreateInfo *s = info->pStages + i;
        if (s->stage==VK_SHADER_STAGE_FRAGMENT_BIT && p->raster.rasterizerDiscardEnable)continue;
        zvk_shader *out;
        if (s->stage == VK_SHADER_STAGE_VERTEX_BIT && !vs++) {
            out = &p->vs;
        } else if (s->stage == VK_SHADER_STAGE_FRAGMENT_BIT && !fs++) {
            out = &p->fs;
        } else {
            r = VK_ERROR_FEATURE_NOT_PRESENT;
            goto error;
        }
        r=cache_compile(d,cache,s,info->flags,
                        s->stage==VK_SHADER_STAGE_FRAGMENT_BIT && p->fixed_depth_bias_bits,out,p->cached+(s->stage==VK_SHADER_STAGE_FRAGMENT_BIT));
        if (r != VK_SUCCESS)
            goto error;
        if(out->early_fragment_tests && p->alpha_coverage) {
            r=VK_ERROR_FEATURE_NOT_PRESENT;
            goto error;
        }
        if (out->native_intrinsics &&
            !(d->physical->caps.capability_bits_v2 & VIRGL_CAP_V2_ZVK_GLSL_INTRINSICS)) {
            r = VK_ERROR_FEATURE_NOT_PRESENT;
            goto error;
        }
        if (out->resource_atlas &&
            !(d->physical->caps.capability_bits_v2 & VIRGL_CAP_V2_ZVK_RESOURCE_ATLAS)) {
            r = VK_ERROR_FEATURE_NOT_PRESENT;
            goto error;
        }
        if (out->vertex_pulling &&
            !(d->physical->caps.capability_bits_v2&VIRGL_CAP_V2_ZVK_VERTEX_PULLING)) {
            r=VK_ERROR_FEATURE_NOT_PRESENT;goto error;
        }
        if (out->structured_flow &&
            !(d->physical->caps.capability_bits_v2&VIRGL_CAP_V2_ZVK_STRUCTURED_FLOW)) {
            r=VK_ERROR_FEATURE_NOT_PRESENT;goto error;
        }
        if (out->wide_varyings &&
            !(d->physical->caps.capability_bits_v2&VIRGL_CAP_V2_ZVK_WIDE_VARYINGS)) {
            r=VK_ERROR_FEATURE_NOT_PRESENT;goto error;
        }
        r = shader_layout(p->layout, out);
        if (r != VK_SUCCESS)
            goto error;
    }
    if (vs!=1) {
        r = VK_ERROR_FEATURE_NOT_PRESENT;
        goto error;
    }
    /* The host graphics linker needs a fragment stage even when Vulkan
     * omits it with rasterizerDiscardEnable. Keep vertex execution intact. */
    if (!fs) {
        p->fs.tgsi = strdup("FRAG\nEND\n");
        if (!p->fs.tgsi) {
            r = VK_ERROR_OUT_OF_HOST_MEMORY;
            goto error;
        }
    }
    for (uint32_t i = 0; i < p->fs.nbindings; i++) {
        zvk_shader_binding *b = p->fs.bindings + i;
        const struct zvk_subpass *sub = p->pass->subpasses + p->subpass;
        if (b->type == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT &&
            (b->input_attachment >= sub->ninputs ||
             sub->inputs[b->input_attachment] == VK_ATTACHMENT_UNUSED)) {
            zvk_error("shader input attachment is absent from the subpass");
            r = VK_ERROR_INITIALIZATION_FAILED;
            goto error;
        }
    }
    for (uint32_t loc = 0; loc < ZVK_ATTRS; loc++)
        if (p->vs.inputs & (1u << loc)) {
            int found = 0;
            for (uint32_t j = 0; j < p->nattrs; j++)
                if (p->attrs[j].location == loc)
                    found = 1;
            if (!found) {
                zvk_error("vertex location %u has no attribute description", loc);
                r = VK_ERROR_INITIALIZATION_FAILED;
                goto error;
            }
        }
    *result = p;
    return VK_SUCCESS;
error:
    vkDestroyPipeline(d, p, allocator);
    return r;
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateGraphicsPipelines(VkDevice d, VkPipelineCache cache,
                                                         uint32_t count,
                                                         const VkGraphicsPipelineCreateInfo *infos,
                                                         const VkAllocationCallbacks *allocator,
                                                         VkPipeline *pipelines)
{
    (void)cache;
    VkResult result = VK_SUCCESS;
    memset(pipelines,0,count*sizeof *pipelines);
    for (uint32_t i = 0; i < count; i++) {
        /* Reuse retained shader entries before honoring a request to avoid
         * compilation. Cache misses are handled by create_pipeline. */
        VkResult r=create_pipeline(d,cache,infos+i,allocator,pipelines+i);
        if (r != VK_SUCCESS) {
            result = r;
            if (infos[i].flags&VK_PIPELINE_CREATE_EARLY_RETURN_ON_FAILURE_BIT)break;
        }
    }
    return result;
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateComputePipelines(VkDevice d, VkPipelineCache cache,
                                                        uint32_t count,
                                                        const VkComputePipelineCreateInfo *infos,
                                                        const VkAllocationCallbacks *allocator,
                                                        VkPipeline *pipelines)
{
    (void)cache;
    VkResult result = VK_SUCCESS;
    memset(pipelines,0,count*sizeof *pipelines);
    for (uint32_t i = 0; i < count; i++) {
        pipelines[i] = VK_NULL_HANDLE;
        const VkComputePipelineCreateInfo *info = infos + i;
        if (!(d->physical->caps.capability_bits_v2 & VIRGL_CAP_V2_ZVK_METAL) ||
            (info->flags &
             ~(VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT |
               VK_PIPELINE_CREATE_ALLOW_DERIVATIVES_BIT | VK_PIPELINE_CREATE_DERIVATIVE_BIT |
               (d->cache_control?(VK_PIPELINE_CREATE_EARLY_RETURN_ON_FAILURE_BIT |
                                  VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT):0))) ||
            info->stage.stage != VK_SHADER_STAGE_COMPUTE_BIT) {
            result = VK_ERROR_FEATURE_NOT_PRESENT;
            if (info->flags&VK_PIPELINE_CREATE_EARLY_RETURN_ON_FAILURE_BIT)break;
            continue;
        }
        zvk_allocator a;
        zvk_allocator_init(&a, allocator);
        VkPipeline p = zvk_alloc(&a, sizeof *p);
        if (!p) {
            result = VK_ERROR_OUT_OF_HOST_MEMORY;
            if (info->flags&VK_PIPELINE_CREATE_EARLY_RETURN_ON_FAILURE_BIT)break;
            continue;
        }
        p->alloc = a;
        p->device = d;
        p->layout = info->layout;
        p->point = VK_PIPELINE_BIND_POINT_COMPUTE;
        __atomic_add_fetch(&p->layout->refs, 1, __ATOMIC_RELAXED);
        VkResult r=cache_compile(d,cache,&info->stage,info->flags,0,&p->cs,p->cached+2);
        if (r == VK_SUCCESS && p->cs.image_metadata &&
            !(d->physical->caps.capability_bits_v2 & VIRGL_CAP_V2_ZVK_METAL_IMAGES))
            r = VK_ERROR_FEATURE_NOT_PRESENT;
        if (r == VK_SUCCESS && p->cs.extended_storage_formats &&
            !d->enabled_features.shaderStorageImageExtendedFormats)
            r = VK_ERROR_FEATURE_NOT_PRESENT;
        if (r == VK_SUCCESS)
            r = shader_layout(p->layout, &p->cs);
        if (r == VK_SUCCESS) {
            pthread_mutex_lock(&d->mutex);
            if (p->cached[2] && p->cached[2]->handle)p->h_cs=p->cached[2]->handle;
            else {
                p->h_cs=virgl_handle(d->gpu);
                if (p->cached[2])p->cached[2]->handle=p->h_cs;
                if (virgl_metal_shader(d->gpu,p->h_cs,p->cs.metal))r=VK_ERROR_INVALID_SHADER_NV;
            }
            pthread_mutex_unlock(&d->mutex);
        }
        if (r == VK_SUCCESS)
            pipelines[i] = p;
        else {
            vkDestroyPipeline(d, p, allocator);
            result = r;
            if (info->flags&VK_PIPELINE_CREATE_EARLY_RETURN_ON_FAILURE_BIT)break;
        }
    }
    return result;
}
static uint32_t blend_factor(VkBlendFactor factor)
{
    static const uint32_t map[] = {PIPE_BLENDFACTOR_ZERO,
                                   PIPE_BLENDFACTOR_ONE,
                                   PIPE_BLENDFACTOR_SRC_COLOR,
                                   PIPE_BLENDFACTOR_INV_SRC_COLOR,
                                   PIPE_BLENDFACTOR_DST_COLOR,
                                   PIPE_BLENDFACTOR_INV_DST_COLOR,
                                   PIPE_BLENDFACTOR_SRC_ALPHA,
                                   PIPE_BLENDFACTOR_INV_SRC_ALPHA,
                                   PIPE_BLENDFACTOR_DST_ALPHA,
                                   PIPE_BLENDFACTOR_INV_DST_ALPHA,
                                   PIPE_BLENDFACTOR_CONST_COLOR,
                                   PIPE_BLENDFACTOR_INV_CONST_COLOR,
                                   PIPE_BLENDFACTOR_CONST_ALPHA,
                                   PIPE_BLENDFACTOR_INV_CONST_ALPHA,
                                   PIPE_BLENDFACTOR_SRC_ALPHA_SATURATE};
    return (unsigned)factor < ZVK_ARRAY_SIZE(map) ? map[factor] : PIPE_BLENDFACTOR_ZERO;
}
VkResult zvk_pipeline_gpu(VkPipeline p)
{
    if (p->h_vs)
        return VK_SUCCESS;
    virgl *v = p->device->gpu;
    p->h_vs=p->cached[0]&&p->cached[0]->handle?p->cached[0]->handle:virgl_handle(v);
    p->h_fs=p->fs.tgsi?(p->cached[1]&&p->cached[1]->handle?p->cached[1]->handle:virgl_handle(v)):0;
    p->h_blend = virgl_handle(v);
    p->h_rs = virgl_handle(v);
    p->h_dsa = virgl_handle(v);
    if (!p->cached[0] || !p->cached[0]->handle)
        virgl_create_shader(v,p->h_vs,PIPE_SHADER_VERTEX,p->vs.tgsi);
    if (p->h_fs && (!p->cached[1] || !p->cached[1]->handle))
        virgl_create_shader(v, p->h_fs, PIPE_SHADER_FRAGMENT, p->fs.tgsi);
    if (p->cached[0])p->cached[0]->handle=p->h_vs;
    if (p->cached[1])p->cached[1]->handle=p->h_fs;
    uint32_t blend[VIRGL_OBJ_BLEND_SIZE - 1] = {0};
    blend[0] =
        VIRGL_OBJ_BLEND_S0_INDEPENDENT_BLEND_ENABLE(p->device->enabled_features.independentBlend) |
        VIRGL_OBJ_BLEND_S0_ALPHA_TO_COVERAGE(p->alpha_coverage);
    for (uint32_t i = 0; i < p->pass->subpasses[p->subpass].ncolors; i++) {
        VkPipelineColorBlendAttachmentState *b = &p->blend[i];
        blend[2 + i] =
            VIRGL_OBJ_BLEND_S2_RT_BLEND_ENABLE(b->blendEnable) |
            VIRGL_OBJ_BLEND_S2_RT_COLORMASK(b->colorWriteMask) |
            VIRGL_OBJ_BLEND_S2_RT_RGB_FUNC(b->colorBlendOp) |
            VIRGL_OBJ_BLEND_S2_RT_ALPHA_FUNC(b->alphaBlendOp) |
            VIRGL_OBJ_BLEND_S2_RT_RGB_SRC_FACTOR(blend_factor(b->srcColorBlendFactor)) |
            VIRGL_OBJ_BLEND_S2_RT_RGB_DST_FACTOR(blend_factor(b->dstColorBlendFactor)) |
            VIRGL_OBJ_BLEND_S2_RT_ALPHA_SRC_FACTOR(blend_factor(b->srcAlphaBlendFactor)) |
            VIRGL_OBJ_BLEND_S2_RT_ALPHA_DST_FACTOR(blend_factor(b->dstAlphaBlendFactor));
    }
    virgl_create_blend(v, p->h_blend, blend);
    uint32_t raster[VIRGL_OBJ_RS_SIZE - 1] = {0};
    zvk_raster(p, &p->raster, raster);
    virgl_create_rasterizer(v, p->h_rs, raster);
    uint32_t depth[VIRGL_OBJ_DSA_SIZE - 1];
    zvk_dsa(p, &p->depth, depth);
    virgl_create_dsa(v, p->h_dsa, depth);
    uint32_t n = 0, offsets[ZVK_ATTRS] = {0}, bindings[ZVK_ATTRS] = {0}, formats[ZVK_ATTRS] = {0},
             divisors[ZVK_ATTRS] = {0};
    for (uint32_t i = 0; !p->vs.vertex_pulling && i < p->nattrs; i++) {
        VkVertexInputAttributeDescription *a = p->attrs + i;
        if (!(p->vs.inputs & (1u << a->location)))
            continue;
        uint32_t slot = p->vs.input_map[a->location];
        offsets[slot] = 0;
        bindings[slot] = slot;
        formats[slot] = zvk_format(a->format);
        const zvk_format_info *f = zvk_get_format_info(a->format);
        int constant = 0;
        for (uint32_t j = 0; j < p->nbindings; j++)
            if (p->bindings[j].binding == a->binding)
                constant = !p->bindings[j].stride;
        if (constant || zvk_format_integer(a->format) ||
            (f->bits[0] != 8 && f->bits[0] != 16 && f->bits[0] != 32)) {
            /* Integer attributes are expanded to raw uint32 lanes, including
             * correctly typed (0,0,0,1) defaults, before the float-bit TGSI input. */
            formats[slot] = VIRGL_FORMAT_R32G32B32A32_FLOAT;
            offsets[slot] = 0;
        }
        if (slot >= n)
            n = slot + 1;
        for (uint32_t j = 0; j < p->nbindings; j++)
            if (p->bindings[j].binding == a->binding)
                divisors[slot] = p->bindings[j].inputRate == VK_VERTEX_INPUT_RATE_INSTANCE;
    }
    for (uint32_t i = 0; i < n; i++)
        if (!formats[i])
            formats[i] = VIRGL_FORMAT_R32_FLOAT;
    if (n) {
        p->h_ve = virgl_handle(v);
        virgl_create_vertex_elements_ex(v, p->h_ve, n, offsets, divisors, bindings, formats);
    }
    return VK_SUCCESS;
}

void zvk_raster(VkPipeline p, const VkPipelineRasterizationStateCreateInfo *state, uint32_t *words)
{
    memset(words, 0, (VIRGL_OBJ_RS_SIZE - 1) * sizeof *words);
    words[0] = VIRGL_OBJ_RS_S0_MULTISAMPLE(p->samples != VK_SAMPLE_COUNT_1_BIT) |
               VIRGL_OBJ_RS_S0_DEPTH_CLIP(1) | VIRGL_OBJ_RS_S0_SCISSOR(1) |
               VIRGL_OBJ_RS_S0_HALF_PIXEL_CENTER(1) | VIRGL_OBJ_RS_S0_FLATSHADE_FIRST(1) |
               VIRGL_OBJ_RS_S0_CULL_FACE(state->cullMode) |
               VIRGL_OBJ_RS_S0_FRONT_CCW(state->frontFace == VK_FRONT_FACE_COUNTER_CLOCKWISE) |
               VIRGL_OBJ_RS_S0_RASTERIZER_DISCARD(state->rasterizerDiscardEnable) |
               VIRGL_OBJ_RS_S0_OFFSET_TRI(state->depthBiasEnable && !p->fixed_depth_bias_bits) |
               VIRGL_OBJ_RS_S0_OFFSET_LINE(state->depthBiasEnable && !p->fixed_depth_bias_bits) |
               VIRGL_OBJ_RS_S0_OFFSET_POINT(state->depthBiasEnable && !p->fixed_depth_bias_bits);
    float point_size = 1;
    memcpy(words + VIRGL_OBJ_RS_POINT_SIZE - 2, &point_size, 4);
    memcpy(words + VIRGL_OBJ_RS_LINE_WIDTH - 2, &state->lineWidth, 4);
    memcpy(words + VIRGL_OBJ_RS_OFFSET_UNITS - 2, &state->depthBiasConstantFactor, 4);
    memcpy(words + VIRGL_OBJ_RS_OFFSET_SCALE - 2, &state->depthBiasSlopeFactor, 4);
    memcpy(words + VIRGL_OBJ_RS_OFFSET_CLAMP - 2, &state->depthBiasClamp, 4);
}

void zvk_dsa(VkPipeline pipeline, const VkPipelineDepthStencilStateCreateInfo *state,
             uint32_t *words)
{
    memset(words, 0, (VIRGL_OBJ_DSA_SIZE - 1) * sizeof *words);
    words[0] = VIRGL_OBJ_DSA_S0_DEPTH_ENABLE(state->depthTestEnable) |
               VIRGL_OBJ_DSA_S0_DEPTH_WRITEMASK(state->depthWriteEnable) |
               VIRGL_OBJ_DSA_S0_DEPTH_FUNC(state->depthCompareOp) |
               VIRGL_OBJ_DSA_S0_ALPHA_ENABLED(pipeline->alpha_coverage && pipeline->samples == 1) |
               VIRGL_OBJ_DSA_S0_ALPHA_FUNC(PIPE_FUNC_GREATER);
    static const uint32_t ops[] = {PIPE_STENCIL_OP_KEEP,      PIPE_STENCIL_OP_ZERO,
                                   PIPE_STENCIL_OP_REPLACE,   PIPE_STENCIL_OP_INCR,
                                   PIPE_STENCIL_OP_DECR,      PIPE_STENCIL_OP_INVERT,
                                   PIPE_STENCIL_OP_INCR_WRAP, PIPE_STENCIL_OP_DECR_WRAP};
    const VkStencilOpState *faces[] = {&state->front, &state->back};
    for (int i = 0; i < 2; i++) {
        const VkStencilOpState *s = faces[i];
        words[1 + i] =
            VIRGL_OBJ_DSA_S1_STENCIL_ENABLED(state->stencilTestEnable) |
            VIRGL_OBJ_DSA_S1_STENCIL_FUNC(s->compareOp) |
            VIRGL_OBJ_DSA_S1_STENCIL_FAIL_OP(ops[(unsigned)s->failOp < 8 ? s->failOp : 0]) |
            VIRGL_OBJ_DSA_S1_STENCIL_ZPASS_OP(ops[(unsigned)s->passOp < 8 ? s->passOp : 0]) |
            VIRGL_OBJ_DSA_S1_STENCIL_ZFAIL_OP(
                ops[(unsigned)s->depthFailOp < 8 ? s->depthFailOp : 0]) |
            VIRGL_OBJ_DSA_S1_STENCIL_VALUEMASK(s->compareMask) |
            VIRGL_OBJ_DSA_S1_STENCIL_WRITEMASK(s->writeMask);
    }
}
