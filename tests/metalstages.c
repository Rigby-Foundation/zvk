/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Emit actual SPIR-V fixture translations for the independent Metal GPU test.
 * This does not enable Metal graphics in the public Vulkan driver. */
#include "zvk_internal.h"
#include "shaders.h"
struct stage_case {const char *name;const uint32_t *code;size_t bytes;VkShaderStageFlagBits stage;};
int main(int argc,char **argv)
{
    if(argc<2 || argc>3 || (argc==3 && strcmp(argv[2],"--fuzz")))return 2;
    const struct stage_case cases[]={
        {"vertex",ms_write_vert,sizeof ms_write_vert,VK_SHADER_STAGE_VERTEX_BIT},
        {"vertex_varying",builtin_vert,sizeof builtin_vert,VK_SHADER_STAGE_VERTEX_BIT},
        {"vertex_attributes",triangle_vert,sizeof triangle_vert,VK_SHADER_STAGE_VERTEX_BIT},
        {"fragment_uint",ms_uint_write_frag,sizeof ms_uint_write_frag,VK_SHADER_STAGE_FRAGMENT_BIT},
        {"fragment_float",push_frag,sizeof push_frag,VK_SHADER_STAGE_FRAGMENT_BIT},
        {"fragment_varying",triangle_frag,sizeof triangle_frag,VK_SHADER_STAGE_FRAGMENT_BIT},
        {"fragment_early",metal_early_frag,sizeof metal_early_frag,VK_SHADER_STAGE_FRAGMENT_BIT},
        {"fragment_late",metal_late_frag,sizeof metal_late_frag,VK_SHADER_STAGE_FRAGMENT_BIT},
        {"fragment_derivatives",metal_derivatives_frag,sizeof metal_derivatives_frag,VK_SHADER_STAGE_FRAGMENT_BIT},
        {"fragment_structured",structured_frag,sizeof structured_frag,VK_SHADER_STAGE_FRAGMENT_BIT},
        {"fragment_selection_exit",selection_exit_frag_spvasm,sizeof selection_exit_frag_spvasm,VK_SHADER_STAGE_FRAGMENT_BIT},
        {"fragment_texture",texture_frag,sizeof texture_frag,VK_SHADER_STAGE_FRAGMENT_BIT},
        {"fragment_centroid",metal_centroid_frag,sizeof metal_centroid_frag,VK_SHADER_STAGE_FRAGMENT_BIT},
        {"vertex_components",metal_component_vert,sizeof metal_component_vert,VK_SHADER_STAGE_VERTEX_BIT},
        {"fragment_components",metal_component_frag,sizeof metal_component_frag,VK_SHADER_STAGE_FRAGMENT_BIT},
        {"fragment_inputs",subpass_frag,sizeof subpass_frag,VK_SHADER_STAGE_FRAGMENT_BIT},
        {"fragment_input_ms",metal_input_frag,sizeof metal_input_frag,VK_SHADER_STAGE_FRAGMENT_BIT},
        {"vertex_blocks",metal_block_vert,sizeof metal_block_vert,VK_SHADER_STAGE_VERTEX_BIT},
        {"fragment_blocks",metal_block_frag,sizeof metal_block_frag,VK_SHADER_STAGE_FRAGMENT_BIT},
        {"fragment_query_lod",metal_query_lod_frag,sizeof metal_query_lod_frag,VK_SHADER_STAGE_FRAGMENT_BIT},
        {"fragment_query_cube",metal_query_cube_frag,sizeof metal_query_cube_frag,VK_SHADER_STAGE_FRAGMENT_BIT},
    };
    for(unsigned i=0;i<ZVK_ARRAY_SIZE(cases);i++) {
        zvk_shader shader;
        VkResult r=zvk_spirv_metal_graphics(cases[i].code,cases[i].bytes/4,"main",NULL,cases[i].stage,&shader);
        if(r!=VK_SUCCESS){fprintf(stderr,"%s: %s\n",cases[i].name,zvkGetLastError());return 1;}
        char path[1024];int n=snprintf(path,sizeof path,"%s/%s.msl",argv[1],cases[i].name);
        if(n<0 || (size_t)n>=sizeof path){zvk_shader_finish(&shader);return 2;}
        FILE *file=fopen(path,"w");
        if(!file){zvk_shader_finish(&shader);return 2;}
        int failed=fputs(shader.metal,file)<0;
        failed|=fclose(file)!=0;zvk_shader_finish(&shader);if(failed)return 2;
        printf("METAL_STAGE_SOURCE %s PASS\n",cases[i].name);
    }
    if(argc==3) {
        zvk_shader shader;
        unsigned mutations=0;
        for(unsigned i=0;i<ZVK_ARRAY_SIZE(cases);i++) {
            size_t words=cases[i].bytes/4;
            uint32_t *copy=malloc(cases[i].bytes);
            if(!copy)return 2;
            memcpy(copy,cases[i].code,cases[i].bytes);
            for(size_t word=0;word<words;word++) {
                uint32_t original=copy[word];
                for(unsigned variant=0;variant<2;variant++) {
                    copy[word]=variant?UINT32_MAX:0;
                    (void)zvk_spirv_metal_graphics(copy,words,"main",NULL,cases[i].stage,&shader);
                    zvk_shader_finish(&shader);mutations++;
                }
                copy[word]=original;
            }
            free(copy);
        }
        printf("METAL_GRAPHICS_MALFORMED_MUTATIONS %u PASS\n",mutations);
    }
    return 0;
}
