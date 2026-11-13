/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (C) 2026 Rigby Foundation */
#pragma once
#include "zvk.h"
#include "zvk_zwm.h"
#include <vulkan/vk_icd.h>
#include <virgl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>

#define ZVK_ATTRS 16
#define ZVK_BINDINGS 32
#define ZVK_SETS 4
#define ZVK_UBOS 12
#define ZVK_SAMPLERS 16
#define ZVK_ATTACHMENTS 8
#define ZVK_COLORS 4
#define ZVK_RESOURCES 52
#define ZVK_MIPS 13
#define ZVK_META_SAMPLER (ZVK_RESOURCES * (1 + ZVK_MIPS))
#define ZVK_META_RANGES (ZVK_META_SAMPLER + 2 * (ZVK_RESOURCES + ZVK_SAMPLERS))
#define ZVK_PUSH 128
#define ZVK_INDIRECT_DRAWS 65535
#define ZVK_MAX_ALLOC (256u * 1024u * 1024u)
#define ZVK_BUFFER_SAMPLER 256u /* PIPE_BUFFER is zero, also the frontend's no-image sentinel. */
#define ZVK_ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

typedef struct {
    VkAllocationCallbacks cb;
    int custom;
} zvk_allocator;
typedef struct {
    uint32_t set, binding, element, slot, bytes, target;
    VkDescriptorType type;
    uint32_t input_attachment;
    VkFormat image_format;
    uint32_t image_dim, image_array, image_kind, image_ms;
    uint32_t sampler_set, sampler_binding, sampler_element;
    uint32_t index, dynamic_index, sampler_index;
    int separate_sampler, texture_use, uses_sampling;
} zvk_shader_binding;
typedef struct {
    char *tgsi;
    char *metal;
    uint32_t local_size[3];
    uint32_t inputs; /* vertex attribute locations read */
    uint8_t input_map[ZVK_ATTRS]; /* Vulkan location -> dense TGSI input */
    uint32_t nbindings;
    zvk_shader_binding bindings[ZVK_RESOURCES];
    uint32_t nsamplers;
    zvk_shader_binding samplers[ZVK_SAMPLERS];
    uint32_t push_bytes;
    uint32_t sample_queries;
    int instance_index;
    int sample_mask_in;
    int sample_mask_out;
    int image_metadata;
    int packed_buffers;
    int extended_storage_formats;
    int native_intrinsics;
    int sampling;
    int resource_atlas;
    int vertex_pulling;
    int structured_flow;
    int early_fragment_tests;
    int fixed_depth_bias;
    int wide_varyings;
} zvk_shader;

struct VkPhysicalDevice_T {
    VK_LOADER_DATA loader;
    VkInstance instance;
    struct virgl_caps_v2 caps;
};
struct VkInstance_T {
    VK_LOADER_DATA loader;
    zvk_allocator alloc;
    struct VkPhysicalDevice_T physical;
    int available;
    uint32_t extensions;
    pthread_mutex_t surface_mutex;
    VkSurfaceKHR surfaces;
};
struct VkQueue_T {
    VK_LOADER_DATA loader;
    VkDevice device;
    uint32_t priority; /* low [0,.5), high [.5,1]; one queue per device */
};
struct VkDevice_T {
    VK_LOADER_DATA loader;
    zvk_allocator alloc;
    VkPhysicalDevice physical;
    struct VkQueue_T queue;
    virgl *gpu;
    pthread_mutex_t mutex;
    pthread_mutex_t memory_mutex;
    VkDeviceSize allocated_bytes;
    uint32_t allocation_count;
    pthread_cond_t condition;
    pthread_t worker;
    int worker_started, worker_stop;
    struct zvk_submission *submissions, *submission_tail;
    VkResult lost;
    uint32_t extensions;
    VkPhysicalDeviceFeatures enabled_features;
    int cache_control, host_query_reset;
    VkSwapchainKHR swapchains;
};
struct VkDeviceMemory_T {
    zvk_allocator alloc;
    VkDevice device;
    uint8_t *data;
    VkDeviceSize size;
    int mapped;
    virgl_res *compute_gpu;
};
struct VkBuffer_T {
    zvk_allocator alloc;
    VkDevice device;
    VkDeviceSize size, offset;
    VkBufferUsageFlags usage;
    VkDeviceMemory memory;
    virgl_res *gpu;
};
struct VkBufferView_T {
    zvk_allocator alloc;
    VkDevice device;
    VkBuffer buffer;
    VkFormat format;
    VkDeviceSize offset, range;
    virgl_res *gpu;
    uint32_t sampler_view, sampler;
    int uploaded;
};
struct VkImage_T {
    zvk_allocator alloc;
    VkDevice device;
    VkImageCreateInfo info;
    VkDeviceMemory memory;
    VkDeviceSize offset, size;
    uint32_t format, bpp, target;
    VkDeviceSize level_offsets[13];
    VkImageLayout *layouts;
    uint8_t *transfer_valid;
    uint8_t *transfer_map;
    virgl_res *gpu;
    VkSwapchainKHR swapchain;
};
enum {
    ZVK_EXT_SURFACE = 1,
    ZVK_EXT_HEADLESS = 2,
    ZVK_EXT_ZWM = 4,
    ZVK_EXT_SWAPCHAIN = 8,
    ZVK_EXT_DEBUG = 16,
    ZVK_EXT_PROPERTIES2 = 32,
    ZVK_EXT_CACHE_CONTROL = 64,
    ZVK_EXT_HOST_QUERY_RESET = 128
};
struct VkSurfaceKHR_T {
    zvk_allocator alloc;
    VkInstance instance;
    VkSurfaceKHR next;
    zwm *connection;
    uint32_t window;
    VkExtent2D extent;
    VkSwapchainKHR current, latest;
    uint32_t current_image;
    int refs, lost;
};
struct VkSwapchainKHR_T {
    zvk_allocator alloc;
    VkDevice device;
    VkSurfaceKHR surface;
    VkSwapchainKHR next;
    VkExtent2D extent;
    VkFormat format;
    uint32_t count, cursor;
    VkImage images[3];
    virgl_res *display[3];
    int state[3], retired;
};
struct VkImageView_T {
    zvk_allocator alloc;
    VkDevice device;
    VkImage image;
    VkImage reinterpret;
    VkFormat format;
    uint32_t surface, sampler_view, target;
    virgl_res *sample_gpu;
    uint32_t input_view;
    virgl_res *input_gpu;
    virgl_res *compute_image, *stencil_gpu;
    uint32_t compute_view, compute_sampler;
    VkDeviceMemory compute_memory;
    VkImageSubresourceRange range;
    VkComponentMapping swizzle;
};
struct VkSampler_T {
    zvk_allocator alloc;
    VkDevice device;
    struct virgl_sampler_info info;
    VkSamplerCreateInfo create;
    uint32_t handle;
};
struct VkShaderModule_T {
    zvk_allocator alloc;
    VkDevice device;
    size_t words;
    uint32_t *code;
};
struct VkDescriptorSetLayout_T {
    zvk_allocator alloc;
    VkDevice device;
    uint32_t count, ndescriptors, ndynamic;
    int refs;
    VkDescriptorSetLayoutBinding *bindings;
    uint32_t *starts, *dynamic;
    VkSampler *immutable;
};
typedef struct {
    VkDescriptorType type;
    VkBuffer buffer;
    VkDeviceSize offset, range;
    VkSampler sampler;
    VkImageView image;
    VkImageLayout layout;
    VkBufferView texel;
} zvk_descriptor;
struct VkDescriptorSet_T {
    VkDescriptorPool pool;
    VkDescriptorSetLayout layout;
    zvk_descriptor *descriptors;
    VkDescriptorSet next;
};
struct VkDescriptorPool_T {
    zvk_allocator alloc;
    VkDevice device;
    uint32_t max_sets, allocated;
    VkDescriptorSet sets;
    uint32_t capacity[11], used[11];
};
struct VkPipelineLayout_T {
    zvk_allocator alloc;
    VkDevice device;
    uint32_t nsets;
    VkDescriptorSetLayout sets[ZVK_SETS];
    uint32_t push_bytes;
    int refs;
};
struct zvk_subpass {
    uint32_t colors[ZVK_COLORS], resolves[ZVK_COLORS], ncolors, depth;
    uint32_t inputs[ZVK_ATTACHMENTS], ninputs;
    VkImageLayout *layouts;
    VkSampleCountFlagBits samples;
};
struct VkRenderPass_T {
    zvk_allocator alloc;
    VkDevice device;
    uint32_t count;
    int refs;
    VkAttachmentDescription *attachments;
    uint32_t nsubpasses, *first_use;
    struct zvk_subpass *subpasses;
};
struct VkFramebuffer_T {
    zvk_allocator alloc;
    VkDevice device;
    VkRenderPass pass;
    uint32_t count, width, height, layers;
    VkImageView *views;
};
struct VkPipelineCache_T {
    zvk_allocator alloc;
    VkDevice device;
    struct zvk_cache_link *entries;
};
struct VkPipeline_T {
    zvk_allocator alloc;
    VkDevice device;
    VkPipelineLayout layout;
    VkRenderPass pass;
    uint32_t subpass;
    zvk_shader vs, fs;
    zvk_shader cs;
    struct zvk_cached_shader *cached[3];
    uint32_t h_cs;
    VkPipelineBindPoint point;
    uint32_t h_vs, h_fs, h_blend, h_rs, h_dsa, h_ve;
    VkPrimitiveTopology topology;
    VkBool32 restart;
    uint32_t nattrs, nbindings;
    VkVertexInputAttributeDescription attrs[ZVK_ATTRS];
    VkVertexInputBindingDescription bindings[ZVK_ATTRS];
    VkViewport viewport;
    VkRect2D scissor;
    int dynamic_viewport, dynamic_scissor, dynamic_blend;
    int dynamic_stencil_compare, dynamic_stencil_write, dynamic_stencil_reference;
    int dynamic_line_width, dynamic_depth_bias;
    unsigned fixed_depth_bias_bits;
    uint32_t sample_mask, samples;
    int alpha_coverage;
    VkPipelineRasterizationStateCreateInfo raster;
    VkPipelineDepthStencilStateCreateInfo depth;
    VkPipelineColorBlendAttachmentState blend[ZVK_COLORS];
    float blend_constants[4];
};
struct VkFence_T {
    zvk_allocator alloc;
    VkDevice device;
    int signaled;
};
struct VkSemaphore_T {
    zvk_allocator alloc;
    VkDevice device;
    int signaled;
};
struct VkEvent_T {
    zvk_allocator alloc;
    VkDevice device;
    int signaled;
};

struct zvk_query {
    uint32_t handle;
    virgl_res *result;
    uint64_t value;
    int reset, active, available, suspended;
};
struct VkQueryPool_T {
    zvk_allocator alloc;
    VkDevice device;
    VkQueryType type;
    uint32_t count;
    struct zvk_query *queries;
};
struct zvk_wait_events {
    uint32_t count;
    VkEvent events[];
};
struct zvk_blit {
    VkImage src, dst;
    VkImageLayout src_layout, dst_layout;
    VkFilter filter;
    VkImageBlit region;
};
struct zvk_attachment_clear {
    VkClearAttachment attachment;
    VkClearRect rect;
};
struct zvk_indirect {
    VkBuffer buffer;
    VkDeviceSize offset;
    uint32_t count, stride;
    int indexed;
};
struct zvk_stencil {
    VkStencilFaceFlags faces;
    uint32_t value, kind;
};
struct zvk_query_command {
    VkQueryPool pool;
    uint32_t first, count;
};
struct zvk_query_copy {
    VkQueryPool pool;
    uint32_t first, count;
    VkBuffer buffer;
    VkDeviceSize offset, stride;
    VkQueryResultFlags flags;
};
enum zvk_op {
    ZVK_BEGIN_PASS,
    ZVK_BIND_COMPUTE,
    ZVK_BIND_COMPUTE_SETS,
    ZVK_DISPATCH,
    ZVK_NEXT_SUBPASS,
    ZVK_END_PASS,
    ZVK_BIND_PIPELINE,
    ZVK_VIEWPORT,
    ZVK_SCISSOR,
    ZVK_BIND_VERTEX,
    ZVK_BIND_INDEX,
    ZVK_BIND_SETS,
    ZVK_PUSH_CONSTANTS,
    ZVK_DRAW,
    ZVK_DRAW_INDEXED,
    ZVK_COPY_BUFFER,
    ZVK_FILL_BUFFER,
    ZVK_UPDATE_BUFFER,
    ZVK_CLEAR_IMAGE,
    ZVK_COPY_IMAGE_BUFFER,
    ZVK_COPY_BUFFER_IMAGE,
    ZVK_COPY_IMAGE,
    ZVK_BARRIER,
    ZVK_BLEND_CONSTANTS,
    ZVK_EXECUTE,
    ZVK_SET_EVENT,
    ZVK_RESET_EVENT,
    ZVK_WAIT_EVENTS,
    ZVK_BLIT_IMAGE,
    ZVK_CLEAR_ATTACHMENT,
    ZVK_DRAW_INDIRECT,
    ZVK_STENCIL,
    ZVK_LINE_WIDTH,
    ZVK_DEPTH_BIAS,
    ZVK_RESET_QUERY,
    ZVK_BEGIN_QUERY,
    ZVK_END_QUERY,
    ZVK_COPY_QUERY
};
struct zvk_command {
    uint32_t op, size;
};
struct VkCommandPool_T {
    zvk_allocator alloc;
    VkDevice device;
    VkCommandPoolCreateFlags flags;
    VkCommandBuffer buffers;
};
struct VkCommandBuffer_T {
    VK_LOADER_DATA loader;
    VkCommandPool pool;
    VkCommandBuffer next;
    VkCommandBufferLevel level;
    VkCommandBufferUsageFlags usage;
    uint32_t pending;
    enum { ZVK_INITIAL, ZVK_RECORDING, ZVK_EXECUTABLE, ZVK_PENDING, ZVK_INVALID } state;
    VkResult error;
    uint8_t *commands;
    size_t size, capacity;
    int in_pass;
    VkFramebuffer inherited_framebuffer;
    VkRenderPass pass;
    uint32_t subpass;
};
struct zvk_begin_pass {
    VkRenderPass pass;
    VkFramebuffer fb;
    VkRect2D area;
    uint32_t nclear;
    VkClearValue clear[];
};
struct zvk_bind_vertex {
    uint32_t first, count;
    VkBuffer buffers[ZVK_ATTRS];
    VkDeviceSize offsets[ZVK_ATTRS];
};
struct zvk_bind_index {
    VkBuffer buffer;
    VkDeviceSize offset;
    VkIndexType type;
};
struct zvk_bind_sets {
    VkPipelineLayout layout;
    uint32_t first, count, ndynamic;
    VkDescriptorSet sets[ZVK_SETS];
    uint32_t dynamic[ZVK_SETS * ZVK_BINDINGS];
};
struct zvk_push {
    VkShaderStageFlags stages;
    uint32_t offset, size;
    uint8_t data[ZVK_PUSH];
};
struct zvk_draw {
    uint32_t count, instances, first, first_instance;
    int32_t vertex_offset;
};
struct zvk_dispatch {
    uint32_t groups[3];
    VkBuffer indirect;
    VkDeviceSize offset;
};
struct zvk_copy_buffer {
    VkBuffer src, dst;
    VkBufferCopy region;
};
struct zvk_fill {
    VkBuffer buffer;
    VkDeviceSize offset, size;
    uint32_t value;
};
struct zvk_clear {
    VkImage image;
    VkImageLayout layout;
    VkClearValue value;
    VkImageSubresourceRange range;
    int depth;
};
struct zvk_image_buffer {
    VkImage image;
    VkBuffer buffer;
    VkImageLayout layout;
    VkBufferImageCopy region;
};
struct zvk_copy_image {
    VkImage src, dst;
    VkImageLayout src_layout, dst_layout;
    VkImageCopy region;
};
struct zvk_transition {
    VkImage image;
    VkImageLayout old_layout, layout;
    VkImageSubresourceRange range;
};

void zvk_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void zvk_allocator_init(zvk_allocator *out, const VkAllocationCallbacks *in);
void *zvk_alloc(const zvk_allocator *a, size_t size);
void *zvk_alloc_aligned(const zvk_allocator *a, size_t size, size_t alignment);
void zvk_free(const zvk_allocator *a, void *ptr);
void *zvk_grow(const zvk_allocator *a, void *old, size_t oldsize, size_t size);
uint32_t zvk_format(VkFormat format);
VkResult zvk_sample_snapshot(VkImageView view, VkDeviceMemory *memory);
VkResult zvk_blit_volume(const struct zvk_blit *blit);
void *zvk_image_map(VkImage image);
unsigned zvk_block(VkFormat format);
int zvk_bc_supported(VkPhysicalDevice physical);
uint32_t zvk_block_count(VkFormat format, uint32_t pixels);
VkDeviceSize zvk_level_size(VkImage image, uint32_t mip);
VkDeviceSize zvk_gpu_image_offset(VkImage image, uint32_t mip, uint32_t layer);
unsigned zvk_gpu_bpp(VkImage image);
void zvk_decode_bc(VkFormat format, const uint8_t *blocks, void *pixels, uint32_t width,
                   uint32_t height);
int zvk_block_region(VkImage image, uint32_t mip, int32_t x, int32_t y, uint32_t width,
                     uint32_t height);
enum zvk_format_type {
    ZVK_FMT_UNORM,
    ZVK_FMT_SNORM,
    ZVK_FMT_UINT,
    ZVK_FMT_SINT,
    ZVK_FMT_FLOAT,
    ZVK_FMT_SRGB,
    ZVK_FMT_UFLOAT,
    ZVK_FMT_SHARED,
    ZVK_FMT_BC
};
typedef struct {
    VkFormat format;
    uint32_t backend;
    enum zvk_format_type type;
    uint8_t bpp, bits[4], shift[4];
    VkImageAspectFlags aspects;
} zvk_format_info;
const zvk_format_info *zvk_get_format_info(VkFormat format);
VkImageAspectFlags zvk_format_aspects(VkFormat format);
int zvk_format_integer(VkFormat format);
void zvk_pack_clear(VkFormat format, const VkClearColorValue *color, uint8_t texel[16]);
uint32_t zvk_aspect_bpp(VkImage image, VkImageAspectFlags aspect);
uint32_t zvk_bpp(VkFormat format);
int zvk_texel_format(VkFormat format, uint32_t *kind, uint32_t *components);
int zvk_storage_format(VkFormat format);
int zvk_range(VkDeviceSize size, VkDeviceSize offset, VkDeviceSize count);
VkResult zvk_spirv_graphics(const uint32_t *code, size_t words, const char *entry,
                   VkShaderStageFlagBits stage, const VkSpecializationInfo *specialization,
                   unsigned fixed_depth_bias, zvk_shader *shader);
VkResult zvk_spirv(const uint32_t *code, size_t words, const char *entry,
                   VkShaderStageFlagBits stage, const VkSpecializationInfo *specialization,
                   zvk_shader *shader);
VkResult zvk_spirv_groups(const uint32_t *code,size_t words,uint32_t **expanded,size_t *count);
typedef struct { uint32_t *ids,count; } zvk_id_map;
VkResult zvk_spirv_ids(const uint32_t *code,size_t words,zvk_id_map *map);
uint32_t zvk_spirv_id(const zvk_id_map *map,uint32_t id);
VkResult zvk_spirv_metal(const uint32_t *code, size_t words, const char *entry,
                         const VkSpecializationInfo *spec, zvk_shader *shader);
VkResult zvk_spirv_metal_graphics(const uint32_t *code,size_t words,const char *entry,
                                  const VkSpecializationInfo *spec,VkShaderStageFlagBits stage,zvk_shader *out);
void zvk_shader_finish(zvk_shader *shader);
VkResult zvk_shader_check(const uint32_t *code, size_t words);
void *zvk_record(VkCommandBuffer command, enum zvk_op op, size_t bytes);
int zvk_passes_compatible(VkRenderPass a, VkRenderPass b);
VkResult zvk_execute(VkQueue queue, VkCommandBuffer command, uint32_t depth);
VkResult zvk_queue_start(VkDevice device);
void zvk_queue_finish(VkDevice device);
VkResult zvk_pipeline_gpu(VkPipeline pipeline);
VkResult zvk_image_gpu(VkImage image);
VkResult zvk_buffer_upload(VkBuffer buffer);
uint64_t zvk_now_ns(void);

VkResult zvk_instance_extensions(uint32_t *count, VkExtensionProperties *properties);
VkResult zvk_device_extensions(uint32_t *count, VkExtensionProperties *properties);
VkResult zvk_enable_extensions(uint32_t count, const char *const *names, int device,
                               uint32_t *bits);
void zvk_wsi_device_finish(VkDevice device);

void zvk_dsa(VkPipeline pipeline, const VkPipelineDepthStencilStateCreateInfo *state,
             uint32_t *words);
void zvk_raster(VkPipeline pipeline, const VkPipelineRasterizationStateCreateInfo *state,
                uint32_t *words);
VkResult zvk_query_begin(VkQueryPool pool, uint32_t index);
VkResult zvk_query_reset(VkQueryPool pool, uint32_t first, uint32_t count);
VkResult zvk_query_pause(VkQueryPool pool, uint32_t index);
void zvk_query_resume(VkQueryPool pool, uint32_t index);
VkResult zvk_query_end(VkQueryPool pool, uint32_t index);
VkResult zvk_query_results(VkQueryPool pool, uint32_t first, uint32_t count, size_t size,
                           void *data, VkDeviceSize stride, VkQueryResultFlags flags);

uint32_t zvk_mip(uint32_t size, uint32_t level);
VkDeviceSize zvk_image_offset(VkImage image, uint32_t mip, uint32_t layer);
int zvk_image_range(VkImage image, const VkImageSubresourceRange *range,
                    VkImageSubresourceRange *out);
int zvk_layout_is(VkImage image, uint32_t mip, uint32_t layer, VkImageLayout layout);
void zvk_layout_set(VkImage image, const VkImageSubresourceRange *range, VkImageLayout layout);

VkSampleCountFlags zvk_samples(VkPhysicalDevice physical, VkFormat format);
