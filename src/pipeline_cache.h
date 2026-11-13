/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Cache immutable compiler results and native shader objects independently
 * of pipeline/layout lifetime. Exact keys include SPIR-V, entry point and
 * specialization values; cache hits never invoke a shader compiler. */
struct zvk_cached_shader {
    zvk_allocator alloc;
    VkDevice device;
    unsigned refs,handle;
    VkShaderStageFlagBits stage;
    size_t key_size;
    uint8_t *key;
    zvk_shader shader;
};
struct zvk_cache_link {
    struct zvk_cache_link *next;
    struct zvk_cached_shader *shader;
};
/* The device mutex protects lists and GPU objects. */
static void cache_release(struct zvk_cached_shader *entry)
{
    if (!entry || --entry->refs)return;
    if (entry->handle) {
        if (entry->stage==VK_SHADER_STAGE_COMPUTE_BIT)virgl_metal_destroy(entry->device->gpu,entry->handle);
        else virgl_destroy_object(entry->device->gpu,entry->handle,VIRGL_OBJECT_SHADER);
    }
    zvk_shader_finish(&entry->shader);
    zvk_free(&entry->alloc,entry->key);
    zvk_free(&entry->alloc,entry);
}
static VkResult cache_key(VkPipelineCache cache,const VkPipelineShaderStageCreateInfo *stage,
                          unsigned options,uint8_t **out,size_t *size)
{
    size_t name=strlen(stage->pName?stage->pName:"main")+1;
    size_t total=stage->module->words*4+name+20;
    const VkSpecializationInfo *spec=stage->pSpecializationInfo;
    if (spec)for (unsigned i=0;i<spec->mapEntryCount;i++) {
        const VkSpecializationMapEntry *m=spec->pMapEntries+i;
        if (!zvk_range(spec->dataSize,m->offset,m->size) || m->size>SIZE_MAX-total-16)
            return VK_ERROR_INITIALIZATION_FAILED;
        total+=16+m->size;
    }
    uint8_t *key=zvk_alloc(&cache->alloc,total);
    if (!key)return VK_ERROR_OUT_OF_HOST_MEMORY;
    uint32_t header[5]={stage->stage,stage->module->words,name,spec?spec->mapEntryCount:0,options};
    memcpy(key,header,20);size_t offset=20;
    memcpy(key+offset,stage->module->code,stage->module->words*4);offset+=stage->module->words*4;
    memcpy(key+offset,stage->pName?stage->pName:"main",name);offset+=name;
    if (spec)for (unsigned i=0;i<spec->mapEntryCount;i++) {
        const VkSpecializationMapEntry *m=spec->pMapEntries+i;
        uint64_t record[2]={m->constantID,m->size};memcpy(key+offset,record,16);offset+=16;
        memcpy(key+offset,(const uint8_t *)spec->pData+m->offset,m->size);offset+=m->size;
    }
    *out=key;*size=total;return VK_SUCCESS;
}
static VkResult cache_compile(VkDevice device,VkPipelineCache cache,
                               const VkPipelineShaderStageCreateInfo *stage,VkPipelineCreateFlags flags,
                               unsigned options,zvk_shader *shader,struct zvk_cached_shader **owner)
{
    uint8_t *key=NULL;size_t key_size=0;
    VkResult result;
    if (cache) {
        result=cache_key(cache,stage,options,&key,&key_size);
        if (result!=VK_SUCCESS)return result;
        pthread_mutex_lock(&device->mutex);
        for (struct zvk_cache_link *link=cache->entries;link;link=link->next) {
            struct zvk_cached_shader *e=link->shader;
            if (e->key_size!=key_size || memcmp(e->key,key,key_size))continue;
            e->refs++;*shader=e->shader;*owner=e;
            pthread_mutex_unlock(&device->mutex);zvk_free(&cache->alloc,key);return VK_SUCCESS;
        }
        pthread_mutex_unlock(&device->mutex);
    }
    if (device->cache_control && (flags&VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT)) {
        if (cache)zvk_free(&cache->alloc,key);
        return VK_PIPELINE_COMPILE_REQUIRED;
    }
    if (stage->stage==VK_SHADER_STAGE_COMPUTE_BIT)
        result=zvk_spirv_metal(stage->module->code,stage->module->words,stage->pName,
                               stage->pSpecializationInfo,shader);
    else result=zvk_spirv_graphics(stage->module->code,stage->module->words,stage->pName,stage->stage,
                           stage->pSpecializationInfo,options,shader);
    if (result!=VK_SUCCESS || !cache) {
        if (cache)zvk_free(&cache->alloc,key);
        return result;
    }
    struct zvk_cached_shader *entry=zvk_alloc(&cache->alloc,sizeof *entry);
    struct zvk_cache_link *link=zvk_alloc(&cache->alloc,sizeof *link);
    if (!entry || !link) {
        zvk_free(&cache->alloc,entry);zvk_free(&cache->alloc,link);zvk_free(&cache->alloc,key);
        zvk_shader_finish(shader);return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    *entry=(struct zvk_cached_shader){.alloc=cache->alloc,.device=device,.refs=2,.stage=stage->stage,
        .key_size=key_size,.key=key,.shader=*shader};
    pthread_mutex_lock(&device->mutex);
    link->shader=entry;link->next=cache->entries;cache->entries=link;
    pthread_mutex_unlock(&device->mutex);*owner=entry;return VK_SUCCESS;
}
