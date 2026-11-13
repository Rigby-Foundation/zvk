/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (C) 2026 Rigby Foundation */
#include "zvk_internal.h"
#include "shaders.h"
#include <spirv/unified1/spirv.h>
static unsigned passed, failed;
#define CHECK(x, label)                                                                            \
    do {                                                                                           \
        if (x) {                                                                                   \
            passed++;                                                                              \
            printf("PASS %s\n", label);                                                            \
        } else {                                                                                   \
            failed++;                                                                              \
            printf("FAIL %s: %s\n", label, zvkGetLastError());                                     \
        }                                                                                          \
    } while (0)
static int compile(const uint32_t *code, size_t words, const char *entry,
                   VkShaderStageFlagBits stage)
{
    zvk_shader shader;
    VkResult result = zvk_spirv(code, words, entry, stage, NULL, &shader);
    zvk_shader_finish(&shader);
    return result == VK_SUCCESS;
}
int main(void)
{
    CHECK(compile(triangle_vert, sizeof triangle_vert / 4, "main", VK_SHADER_STAGE_VERTEX_BIT),
          "vertex compiler");
    CHECK(compile(flow_frag, sizeof flow_frag / 4, "main", VK_SHADER_STAGE_FRAGMENT_BIT),
          "control flow compiler");
    CHECK(compile(dynamic_insert_frag_spvasm,sizeof dynamic_insert_frag_spvasm/4,"main",
                  VK_SHADER_STAGE_FRAGMENT_BIT),
          "dynamic insertion, half quantization and debug function parameters compiler");
    CHECK(compile(subpass_frag, sizeof subpass_frag / 4, "main", VK_SHADER_STAGE_FRAGMENT_BIT),
          "input attachments and shared implicit/explicit FragCoord");
    CHECK(
        compile(phi_frag_spvasm, sizeof phi_frag_spvasm / 4, "main", VK_SHADER_STAGE_FRAGMENT_BIT),
        "phi compiler");
    CHECK(!compile(triangle_vert, sizeof triangle_vert / 4, "missing", VK_SHADER_STAGE_VERTEX_BIT),
          "missing entry point rejected");
    CHECK(!compile(triangle_vert, sizeof triangle_vert / 4, "main", VK_SHADER_STAGE_FRAGMENT_BIT),
          "stage mismatch rejected");
    CHECK(!compile(triangle_vert, sizeof triangle_vert / 4, "main", VK_SHADER_STAGE_COMPUTE_BIT),
          "compute stage rejected");
    CHECK(!compile(early_frag, sizeof early_frag / 4, "main", VK_SHADER_STAGE_FRAGMENT_BIT),
          "early fragment discard remains rejected on the GL path");
    CHECK(compile(early_keep_depth_frag,sizeof early_keep_depth_frag/4,"main",VK_SHADER_STAGE_FRAGMENT_BIT),
          "early tests without shader coverage changes ignore FragDepth writes");
    CHECK(compile(output_initializer_frag_spvasm,sizeof output_initializer_frag_spvasm/4,"main",VK_SHADER_STAGE_FRAGMENT_BIT),
          "ordinary and builtin fragment output initializers compiler");
    int ok = 1;
    for (size_t i = 0; i < sizeof triangle_vert / 4; i++)
        if (compile(triangle_vert, i, "main", VK_SHADER_STAGE_VERTEX_BIT))
            ok = 0;
    CHECK(ok, "all truncated modules rejected");
    zvk_shader compute;
    CHECK(zvk_spirv_metal_graphics(dynamic_insert_frag_spvasm,sizeof dynamic_insert_frag_spvasm/4,
                                  "main",NULL,VK_SHADER_STAGE_FRAGMENT_BIT,&compute)==VK_SUCCESS,
          "Metal dynamic insertion, half specialization and debug function parameters compiler");
    zvk_shader_finish(&compute);
    CHECK(zvk_spirv_metal(buffer_comp, sizeof buffer_comp / 4, "main", NULL, &compute) ==
              VK_SUCCESS,
          "independent Metal storage-buffer compute compiler");
    zvk_shader_finish(&compute);
    CHECK(zvk_spirv_metal(shared_comp, sizeof shared_comp / 4, "main", NULL, &compute) ==
              VK_SUCCESS,
          "workgroup/barrier/atomic compute compiler");
    zvk_shader_finish(&compute);
    CHECK(zvk_spirv_metal(storage_comp, sizeof storage_comp / 4, "main", NULL, &compute) ==
              VK_SUCCESS,
          "typed storage images, queries and image atomics compiler");
    zvk_shader_finish(&compute);
    CHECK(zvk_spirv_metal(texel_comp, sizeof texel_comp / 4, "main", NULL, &compute) == VK_SUCCESS,
          "uniform/storage texel buffers compiler");
    zvk_shader_finish(&compute);
    CHECK(zvk_spirv_metal(texel_bgra_comp, sizeof texel_bgra_comp / 4, "main", NULL, &compute) ==
              VK_SUCCESS,
          "uniform texel size and floating-point fetch compiler");
    zvk_shader_finish(&compute);
    CHECK(zvk_spirv_metal(storage_array_comp, sizeof storage_array_comp / 4, "main", NULL,
                          &compute) == VK_SUCCESS,
          "static storage image descriptor arrays compiler");
    zvk_shader_finish(&compute);
    CHECK(zvk_spirv_metal(storage_unorm_comp, sizeof storage_unorm_comp / 4, "main", NULL,
                          &compute) == VK_SUCCESS,
          "typed image format conversion compiler");
    zvk_shader_finish(&compute);
    CHECK(zvk_spirv_metal(many_bindings_comp, sizeof many_bindings_comp / 4, "main", NULL,
                          &compute) == VK_SUCCESS,
          "maximum compute bindings and static texel arrays compiler");
    zvk_shader_finish(&compute);
    CHECK(
        compile(texture_1d_frag, sizeof texture_1d_frag / 4, "main", VK_SHADER_STAGE_FRAGMENT_BIT),
        "1D sampled image compiler");
    CHECK(compile(texture_1d_array_frag, sizeof texture_1d_array_frag / 4, "main",
                  VK_SHADER_STAGE_FRAGMENT_BIT),
          "1D array sampled image compiler");
    CHECK(
        compile(texture_3d_frag, sizeof texture_3d_frag / 4, "main", VK_SHADER_STAGE_FRAGMENT_BIT),
        "3D sampled image compiler");
    CHECK(zvk_spirv_metal(dimensions_1d_comp, sizeof dimensions_1d_comp / 4, "main", NULL,
                          &compute) == VK_SUCCESS,
          "1D array storage image/query compiler");
    zvk_shader_finish(&compute);
    CHECK(zvk_spirv_metal(dimensions_3d_comp, sizeof dimensions_3d_comp / 4, "main", NULL,
                          &compute) == VK_SUCCESS,
          "3D storage image/query compiler");
    zvk_shader_finish(&compute);
    CHECK(zvk_spirv(robust_vert, sizeof robust_vert / 4, "main", VK_SHADER_STAGE_VERTEX_BIT, NULL,
                    &compute) == VK_SUCCESS,
          "boolean vector reductions and robust vertex test compiler");
    zvk_shader_finish(&compute);
    CHECK(zvk_spirv_metal(robust_matrix_comp, sizeof robust_matrix_comp / 4, "main", NULL,
                          &compute) == VK_SUCCESS,
          "row-major non-square matrix loads and stores compiler");
    zvk_shader_finish(&compute);
    CHECK(zvk_spirv(ms_array_fetch_frag, sizeof ms_array_fetch_frag / 4, "main",
                    VK_SHADER_STAGE_FRAGMENT_BIT, NULL, &compute) == VK_SUCCESS,
          "multisample array fetch, image queries and view metadata compiler");
    zvk_shader_finish(&compute);
    CHECK(zvk_spirv(ms_input_frag, sizeof ms_input_frag / 4, "main", VK_SHADER_STAGE_FRAGMENT_BIT,
                    NULL, &compute) == VK_SUCCESS,
          "multisample color/depth subpass input compiler");
    zvk_shader_finish(&compute);
    CHECK(zvk_spirv(texture_operands_frag, sizeof texture_operands_frag / 4, "main",
                    VK_SHADER_STAGE_FRAGMENT_BIT, NULL, &compute) == VK_SUCCESS,
          "separate image/sampler pairs, texel offsets, gradients and LOD compiler");
    zvk_shader_finish(&compute);
    CHECK(zvk_spirv(ms_builtins_frag, sizeof ms_builtins_frag / 4, "main",
                    VK_SHADER_STAGE_FRAGMENT_BIT, NULL, &compute) == VK_SUCCESS,
          "fragment sample ID, sample position and sample mask compiler");
    zvk_shader_finish(&compute);
    CHECK(zvk_spirv_metal(capacity_comp, sizeof capacity_comp / 4, "main", NULL, &compute) ==
                  VK_SUCCESS &&
              compute.packed_buffers && compute.nbindings == 40,
          "40 descriptor packed Metal compiler");
    zvk_shader_finish(&compute);
    CHECK(zvk_spirv_metal(reconverge_comp,sizeof reconverge_comp/4,"main",NULL,&compute)==VK_SUCCESS,
          "structured compute loops reconverge divergent function calls before execution barriers");
    zvk_shader_finish(&compute);
    unsigned mutations = 0;
    CHECK(zvk_spirv_metal(functions_comp_spvasm, sizeof functions_comp_spvasm / 4, "main", NULL,
                          &compute) == VK_SUCCESS,
          "nonrecursive value function calls, buffer arrays and memory-only fences compiler");
    zvk_shader_finish(&compute);
    uint32_t recursive[sizeof functions_comp_spvasm / 4];
    memcpy(recursive, functions_comp_spvasm, sizeof recursive);
    uint32_t function = 0;
    for (size_t i = 5; i < sizeof recursive / 4; i += recursive[i] >> 16) {
        unsigned op = recursive[i] & 65535;
        if (op == SpvOpFunction)
            function = recursive[i + 2];
        if (op == SpvOpFunctionCall) {
            recursive[i + 3] = function;
            break;
        }
    }
    CHECK(zvk_spirv_metal(recursive, sizeof recursive / 4, "main", NULL, &compute) != VK_SUCCESS,
          "recursive call graph is rejected before GPU compilation");
    zvk_shader_finish(&compute);
    CHECK(compile(stencil_write_vert, sizeof stencil_write_vert / 4, "main",
                  VK_SHADER_STAGE_VERTEX_BIT),
          "vertex read-only storage-buffer descriptor compiler");
    CHECK(compile(stencil_fetch_ms_frag, sizeof stencil_fetch_ms_frag / 4, "main",
                  VK_SHADER_STAGE_FRAGMENT_BIT),
          "unsigned multisample snapshot address and size compiler");
    CHECK(zvk_spirv_metal(samplers48_comp, sizeof samplers48_comp / 4, "main", NULL, &compute) ==
              VK_SUCCESS,
          "sixteen compute multisample image descriptors compiler");
    zvk_shader_finish(&compute);
    const uint32_t *modules[] = {
        triangle_vert,         subpass_frag,          buffer_comp,
        shared_comp,           storage_comp,          texel_comp,
        storage_array_comp,    many_bindings_comp,    texel_bgra_comp,
        functions_comp_spvasm, dimensions_1d_comp,    dimensions_3d_comp,
        texel_sint_comp,       robust_matrix_comp,    ms_array_fetch_frag,
        ms_input_frag,         texture_operands_frag, ms_builtins_frag,
        capacity_comp,         stencil_write_vert,    stencil_fetch_ms_frag,
        stencil_input_ms_frag, samplers48_comp, dynamic_insert_frag_spvasm,
        early_keep_depth_frag,output_initializer_frag_spvasm,reconverge_comp};
    const size_t sizes[] = {
        sizeof triangle_vert,         sizeof subpass_frag,          sizeof buffer_comp,
        sizeof shared_comp,           sizeof storage_comp,          sizeof texel_comp,
        sizeof storage_array_comp,    sizeof many_bindings_comp,    sizeof texel_bgra_comp,
        sizeof functions_comp_spvasm, sizeof dimensions_1d_comp,    sizeof dimensions_3d_comp,
        sizeof texel_sint_comp,       sizeof robust_matrix_comp,    sizeof ms_array_fetch_frag,
        sizeof ms_input_frag,         sizeof texture_operands_frag, sizeof ms_builtins_frag,
        sizeof capacity_comp,         sizeof stencil_write_vert,    sizeof stencil_fetch_ms_frag,
        sizeof stencil_input_ms_frag, sizeof samplers48_comp, sizeof dynamic_insert_frag_spvasm,
        sizeof early_keep_depth_frag,sizeof output_initializer_frag_spvasm,sizeof reconverge_comp};
    for (unsigned m = 0; m < ZVK_ARRAY_SIZE(modules); m++) {
        uint32_t *code = malloc(sizes[m]);
        size_t words = sizes[m] / 4;
        const uint32_t *original = modules[m];
        VkShaderStageFlagBits stage = VK_SHADER_STAGE_COMPUTE_BIT;
        for (size_t i = 5; i < words; i += original[i] >> 16)
            if ((original[i] & 65535) == SpvOpEntryPoint) {
                stage = original[i + 1] == SpvExecutionModelVertex ? VK_SHADER_STAGE_VERTEX_BIT
                        : original[i + 1] == SpvExecutionModelFragment
                            ? VK_SHADER_STAGE_FRAGMENT_BIT
                            : VK_SHADER_STAGE_COMPUTE_BIT;
                break;
            }
        /* Malformed instruction lengths and IDs must fail safely even without
         * a validation layer or SPIRV-Tools, including input-image metadata. */
        for (size_t i = 5; i < words; i += original[i] >> 16) {
            for (unsigned n = 0; n < 4; n++) {
                memcpy(code, original, sizes[m]);
                code[i] = (n << 16) | (code[i] & 65535);
                if (stage != VK_SHADER_STAGE_COMPUTE_BIT)
                    (void)compile(code, words, "main", stage);
                else {
                    (void)zvk_spirv_metal(code, words, "main", NULL, &compute);
                    zvk_shader_finish(&compute);
                }
                mutations++;
            }
            for (unsigned j = 1; j < (original[i] >> 16); j++) {
                memcpy(code, original, sizes[m]);
                code[i + j] = UINT32_MAX;
                if (stage != VK_SHADER_STAGE_COMPUTE_BIT)
                    (void)compile(code, words, "main", stage);
                else {
                    (void)zvk_spirv_metal(code, words, "main", NULL, &compute);
                    zvk_shader_finish(&compute);
                }
                mutations++;
            }
        }
        free(code);
    }
    CHECK(mutations > 100, "malformed instruction and ID mutations completed");
    printf("SPIRV_RESULT %u passed %u failed (%u mutations)\n", passed, failed, mutations);
    return failed ? 1 : 0;
}
