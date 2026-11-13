/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Exercise the guest's actual packet encoder and private renderer shader
 * status/lifetime protocol with the generated Metal graphics stages. */
#include <virgl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static virgl_res *input_buffer(virgl *gpu,const void *data,unsigned size)
{
    virgl_res *result=virgl_res_create(gpu,PIPE_BUFFER,VIRGL_FORMAT_R8_UNORM,VIRGL_BIND_VERTEX_BUFFER,size+32,1,1,0,size+32);
    if(!result || !result->map)return NULL;
    memset(result->map,0x5b,result->size);memcpy((uint8_t *)result->map+16,data,size);
    if(virgl_res_to_host(gpu,result,0,0,0,0,result->size,1,1,0,0,0)){virgl_res_destroy(gpu,result);return NULL;}
    return result;
}
static int check_color(virgl *gpu,virgl_res *output,unsigned samples,const unsigned expected[4])
{
    if(virgl_res_from_host(gpu,output,0,0,0,0,output->size,1,1,0,0,0))return 0;
    const uint8_t *bytes=output->map;
    for(unsigned i=0;i<output->size;i++)if((i<16 || i>=16+32*32*samples*16)&&bytes[i]!=0xa7)return 0;
    const float *pixels=(const float *)(bytes+16);
    for(unsigned i=0;i<32*32*samples;i++)for(unsigned c=0;c<4;c++)
        if(!(pixels[i*4+c]>=0 && pixels[i*4+c]<=1) || (unsigned)(pixels[i*4+c]*255+.5f)!=expected[c]) {
            fprintf(stderr,"Metal buffer draw bad pixel %u channel %u: %g expected %u\n",i,c,pixels[i*4+c],expected[c]);return 0;
        }
    return 1;
}
static int input_unchanged(virgl *gpu,virgl_res *resource,const void *data,unsigned size)
{
    if(virgl_res_from_host(gpu,resource,0,0,0,0,resource->size,1,1,0,0,0))return 0;
    const uint8_t *bytes=resource->map;
    for(unsigned i=0;i<16;i++)if(bytes[i]!=0x5b || bytes[size+16+i]!=0x5b)return 0;
    return !memcmp(bytes+16,data,size);
}
static int check_depth(virgl *gpu,virgl_res *resource,unsigned samples,uint32_t expected)
{
    if(virgl_res_from_host(gpu,resource,0,0,0,0,resource->size,1,1,0,0,0))return 0;
    const uint8_t *bytes=resource->map;unsigned length=32*32*samples*4;
    for(unsigned i=0;i<resource->size;i++)if((i<16 || i>=16+length)&&bytes[i]!=0xc3)return 0;
    const uint32_t *values=(const uint32_t *)(bytes+16);
    for(unsigned i=0;i<32*32*samples;i++)if(values[i]!=expected) {
        fprintf(stderr,"Metal depth texel %u: %08x expected %08x\n",i,values[i],expected);return 0;
    }
    return 1;
}
int main(int argc,char **argv)
{
    if(argc!=2)return 2;
    virgl *gpu=virgl_open();if(!gpu)return 2;
    const struct virgl_caps_v2 *caps=virgl_caps(gpu);
    if(!caps || !(caps->capability_bits_v2&VIRGL_CAP_V2_ZVK_METAL)){virgl_close(gpu);return 2;}
    const char *files[]={"vertex","vertex_varying","vertex_attributes","fragment_uint","fragment_float",
        "fragment_varying","fragment_early","fragment_late","fragment_derivatives","fragment_structured",
        "fragment_selection_exit","fragment_texture","fragment_centroid","vertex_components","fragment_components","fragment_inputs","fragment_input_ms","vertex_blocks","fragment_blocks","fragment_query_lod","fragment_query_cube"};
    unsigned handles[sizeof files/sizeof *files];
    for(unsigned i=0;i<sizeof files/sizeof *files;i++) {
        char path[1024];int n=snprintf(path,sizeof path,"%s/%s.msl",argv[1],files[i]);
        if(n<0 || (size_t)n>=sizeof path){virgl_close(gpu);return 2;}
        FILE *file=fopen(path,"rb");if(!file){virgl_close(gpu);return 2;}
        if(fseek(file,0,SEEK_END)){fclose(file);virgl_close(gpu);return 2;}
        long bytes=ftell(file);
        if(bytes<=0 || bytes>=1024*1024 || fseek(file,0,SEEK_SET)){fclose(file);virgl_close(gpu);return 2;}
        char *source=malloc(bytes+1);
        if(!source || fread(source,1,bytes,file)!=(size_t)bytes){free(source);fclose(file);virgl_close(gpu);return 2;}
        fclose(file);source[bytes]=0;
        unsigned handle=virgl_handle(gpu);int failed=virgl_metal_shader(gpu,handle,source);
        free(source);
        if(failed){fprintf(stderr,"Metal stage transport %s failed\n",files[i]);virgl_close(gpu);return 1;}
        handles[i]=handle;
        printf("METAL_STAGE_TRANSPORT %s PASS\n",files[i]);
    }
    if(!(caps->capability_bits_v2&VIRGL_CAP_V2_ZVK_METAL_GRAPHICS)){virgl_close(gpu);return 2;}
    virgl_res *result=virgl_res_create(gpu,PIPE_BUFFER,VIRGL_FORMAT_R8_UNORM,
        VIRGL_BIND_VERTEX_BUFFER,32*32*4*16+32,1,1,0,32*32*4*16+32);
    if(!result || !result->map){virgl_close(gpu);return 2;}
    const uint32_t formats[4][2]={
        {ZVK_METAL_COLOR_R32_UINT,ZVK_METAL_COLOR_R32_UINT},
        {ZVK_METAL_COLOR_R32_UINT,ZVK_METAL_COLOR_RGBA8_UNORM},
        {ZVK_METAL_COLOR_RGBA8_UNORM,ZVK_METAL_COLOR_R32_UINT},
        {ZVK_METAL_COLOR_RGBA32_SFLOAT,ZVK_METAL_COLOR_RGBA32_UINT}};
    for(unsigned raw=0;raw<2;raw++)for(unsigned sample=1;sample<=4;sample*=2)for(unsigned test=0;test<4;test++) {
        struct zvk_metal_draw_info info={.vertex_shader=handles[0],.fragment_shader=handles[test<2?3:4],
            .format=formats[test][0],.base_format=formats[test][1],.width=32,.height=32,.samples=sample,
            .vertex_count=3,.instance_count=1};
        float depth=.25f;memcpy(info.push[0]+4,&depth,4);
        if(test<2)info.push[1][0]=0x12345678u;
        else {const float color[4]={.25f,.5f,.75f,1};memcpy(info.push[1],color,sizeof color);}
        info.flags=raw?ZVK_METAL_DRAW_RAW:0;
        unsigned pixel_words=raw?zvk_metal_color_bytes(info.base_format)/4:4;
        unsigned bytes=32*32*sample*pixel_words*4;
        memset(result->map,0xa7,result->size);
        if(virgl_res_to_host(gpu,result,0,0,0,0,result->size,1,1,0,0,0)){virgl_close(gpu);return 1;}
        struct virgl_metal_binding output={result->id,16,bytes,1};
        for(unsigned pass=0;pass<4;pass++) {
            if(pass==1) {
                info.flags=(raw?ZVK_METAL_DRAW_RAW:0)|ZVK_METAL_DRAW_LOAD|ZVK_METAL_DRAW_SCISSOR;
                info.scissor[0]=0;info.scissor[1]=0;info.scissor[2]=16;info.scissor[3]=32;
                if(test<2)info.push[1][0]=0x87654321u;
                else {const float color[4]={1,0,.5f,.5f};memcpy(info.push[1],color,sizeof color);}
            }
            if(pass==2) {info.scissor[2]=0;memset(info.push[1],0xff,sizeof info.push[1]);}
            if(pass==3) {
                info.flags=raw?ZVK_METAL_DRAW_RAW:0;info.vertex_count=0;
                if(test<2)info.clear[0]=0xdeadbeefu;
                else {const float color[4]={.25f,.5f,.75f,1};memcpy(info.clear,color,sizeof color);}
            }
            if(virgl_metal_draw(gpu,&info,&output) ||
               virgl_res_from_host(gpu,result,0,0,0,0,result->size,1,1,0,0,0)) {
                fprintf(stderr,"Metal draw transport failed: case %u samples %u pass %u\n",test,sample,pass);virgl_close(gpu);return 1;
            }
            const uint8_t *all=result->map;
            for(unsigned i=0;i<result->size;i++)if((i<16 || i>=16+bytes)&&all[i]!=0xa7) {
                fprintf(stderr,"Metal draw transport overwrote an output guard\n");virgl_close(gpu);return 1;
            }
            const uint32_t *values=(const uint32_t *)(all+16);
            for(unsigned i=0;i<32*32*sample;i++) {
                const uint32_t *pixel=values+pixel_words*i;int ok=1;
                int updated=(pass==1 || pass==2) && i%32<16;
                uint32_t bits=pass==3?0xdeadbeefu:updated?0x87654321u:0x12345678u;
                if(test==0)ok=pixel[0]==bits;
                if(test==1 && raw)ok=pixel[0]==bits;
                if(test==1 && !raw)for(unsigned c=0;c<4;c++) {
                    float value;memcpy(&value,pixel+c,4);
                    if(!(value>=0 && value<=1))ok=0;
                    else ok&=(unsigned)(value*255+.5f)==((bits>>(8*c))&255);
                }
                if(test==2)ok=pixel[0]==(updated?0x808000ffu:0xffbf8040u);
                if(test==3) {
                    const uint32_t original[4]={0x3e800000,0x3f000000,0x3f400000,0x3f800000};
                    const uint32_t changed[4]={0x3f800000,0,0x3f000000,0x3f000000};
                    ok=!memcmp(pixel,updated?changed:original,16);
                }
                if(!ok){fprintf(stderr,"Metal draw transport bad texel %u case %u samples %u pass %u: %08x\n",i,test,sample,pass,pixel[0]);virgl_close(gpu);return 1;}
            }
            printf("METAL_DRAW_TRANSPORT raw=%u case=%u samples=%u pass=%u PASS\n",raw,test,sample,pass);
        }
    }
    for(unsigned samples=1;samples<=4;samples*=2) {
        struct zvk_metal_draw_info info={.vertex_shader=handles[0],.fragment_shader=handles[3],
            .format=ZVK_METAL_COLOR_RGBA32_UINT,.base_format=ZVK_METAL_COLOR_RGBA32_SFLOAT,
            .width=32,.height=32,.samples=samples,.vertex_count=3,.instance_count=1,.flags=ZVK_METAL_DRAW_RAW};
        const uint32_t original[4]={0x7fc12345u,0x80000000u,0x7f800000u,0xff800000u};
        const uint32_t changed[4]={0x7fa23456u,0,0x7fffffffu,0xffffffffu};
        memcpy(info.push[1],original,16);
        memset(result->map,0xa7,result->size);
        struct virgl_metal_binding output={result->id,16,32*32*samples*16,1};
        if(virgl_res_to_host(gpu,result,0,0,0,0,result->size,1,1,0,0,0)){virgl_close(gpu);return 1;}
        for(unsigned pass=0;pass<4;pass++) {
            if(pass==1) {
                info.flags|=ZVK_METAL_DRAW_LOAD|ZVK_METAL_DRAW_SCISSOR;
                info.scissor[2]=16;info.scissor[3]=32;memcpy(info.push[1],changed,16);
            }
            if(pass==2)info.scissor[2]=0;
            if(pass==3) {info.flags=ZVK_METAL_DRAW_RAW;info.vertex_count=0;memcpy(info.clear,changed,16);}
            if(virgl_metal_draw(gpu,&info,&output) || virgl_res_from_host(gpu,result,0,0,0,0,result->size,1,1,0,0,0)){virgl_close(gpu);return 1;}
            const uint8_t *bytes=result->map;
            for(unsigned i=0;i<result->size;i++)if((i<16 || i>=16+output.length)&&bytes[i]!=0xa7){virgl_close(gpu);return 1;}
            const uint32_t *values=(const uint32_t *)(bytes+16);
            for(unsigned i=0;i<32*32*samples;i++) {
                const uint32_t *expected=(pass==3 || (pass && i%32<16))?changed:original;
                if(memcmp(values+4*i,expected,16)) {
                    fprintf(stderr,"Metal raw NaN payload texel %u samples %u pass %u failed\n",i,samples,pass);virgl_close(gpu);return 1;
                }
            }
            printf("METAL_RAW_BITS_TRANSPORT samples=%u pass=%u PASS\n",samples,pass);
        }
    }
    const float vertices[4][5]={{0,0,0,0,0},{-1,-1,1,0,1},{3,-1,1,0,1},{-1,3,1,0,1}};
    const float positions[3][2]={{-1,-1},{3,-1},{-1,3}};
    const float instances[2][3]={{1,0,0},{0,1,0}};
    uint32_t metadata[7016]={0}; /* Shared packed-image metadata byte ABI. */
    const uint32_t image[8]={32,32,1,0,4,(1u<<16)|(15u<<8),0x08080808u,0x18100800u};
    memcpy(metadata,image,sizeof image);memcpy(metadata+52*8,image,sizeof image);
    for(unsigned c=0;c<4;c++)metadata[52*14*8+12+c]=c;
    metadata[6912]=0;metadata[6913]=32*32*4;
    uint8_t texels[32*32*4];
    for(unsigned i=0;i<32*32;i++){texels[i*4]=64;texels[i*4+1]=128;texels[i*4+2]=191;texels[i*4+3]=255;}
    const void *inputs[]={vertices,positions,instances,metadata,texels};
    const unsigned sizes[]={sizeof vertices,sizeof positions,sizeof instances,sizeof metadata,sizeof texels};
    virgl_res *resources[5];
    for(unsigned i=0;i<5;i++)if(!(resources[i]=input_buffer(gpu,inputs[i],sizes[i]))){virgl_close(gpu);return 1;}
    for(unsigned samples=1;samples<=4;samples*=2)for(unsigned test=0;test<6;test++) {
        struct zvk_metal_draw_info info={.vertex_shader=handles[test<2?2:0],.fragment_shader=handles[test<2?5:11],
            .format=ZVK_METAL_COLOR_RGBA8_UNORM,.base_format=ZVK_METAL_COLOR_RGBA8_UNORM,
            .width=32,.height=32,.samples=samples,.vertex_count=3,.instance_count=1};
        struct zvk_metal_attribute attributes[2]={{0,2,1,0,20,0},{1,3,1,8,20,0}};
        struct zvk_metal_draw_binding bindings[2]={{0,1,resources[0]->id,16,sizes[0]}};
        unsigned attribute_count=2,buffer_count=1;
        unsigned expected[4]={255,0,255,255};
        if(test==0)info.first_vertex=1;
        if(test==1) {
            info.first_instance=1;
            attributes[0].stride=8;attributes[1]=(struct zvk_metal_attribute){1,3,2,0,12,1};
            bindings[0]=(struct zvk_metal_draw_binding){0,1,resources[1]->id,16,sizes[1]};
            bindings[1]=(struct zvk_metal_draw_binding){0,2,resources[2]->id,16,sizes[2]};
            buffer_count=2;expected[0]=0;expected[1]=255;expected[2]=0;
        }
        if(test==2) {
            attribute_count=0;buffer_count=2;
            bindings[0]=(struct zvk_metal_draw_binding){1,0,resources[4]->id,16,sizes[4]};
            bindings[1]=(struct zvk_metal_draw_binding){1,28,resources[3]->id,16,sizes[3]};
            expected[0]=64;expected[1]=128;expected[2]=191;
        }
        if(test==3) {
            info.vertex_shader=handles[13];info.fragment_shader=handles[14];
            attribute_count=buffer_count=0;expected[0]=64;expected[1]=128;expected[2]=191;
        }
        if(test==4) {
            info.vertex_shader=handles[2];info.fragment_shader=handles[5];
            attributes[0].stride=8;attributes[1]=(struct zvk_metal_attribute){1,3,2,0,0,0};
            bindings[0]=(struct zvk_metal_draw_binding){0,1,resources[1]->id,16,sizes[1]};
            bindings[1]=(struct zvk_metal_draw_binding){0,2,resources[2]->id,16,sizes[2]};
            attribute_count=buffer_count=2;expected[0]=255;expected[1]=expected[2]=0;
        }
        if(test==5) {
            info.vertex_shader=handles[17];info.fragment_shader=handles[18];
            attribute_count=buffer_count=0;expected[0]=64;expected[1]=128;expected[2]=191;
        }
        memset(result->map,0xa7,result->size);
        struct virgl_metal_binding output={result->id,16,32*32*samples*16,1};
        if(virgl_res_to_host(gpu,result,0,0,0,0,result->size,1,1,0,0,0) ||
            virgl_metal_draw_buffers(gpu,&info,&output,attribute_count,attributes,buffer_count,bindings) ||
            !check_color(gpu,result,samples,expected)) {
            fprintf(stderr,"Metal buffer transport case %u samples %u failed\n",test,samples);virgl_close(gpu);return 1;
        }
        printf("METAL_BUFFER_TRANSPORT case=%u samples=%u PASS\n",test,samples);
    }
    for(unsigned i=0;i<5;i++) {
        if(!input_unchanged(gpu,resources[i],inputs[i],sizes[i])){fprintf(stderr,"Metal draw changed an input buffer\n");virgl_close(gpu);return 1;}
        virgl_res_destroy(gpu,resources[i]);
    }
    /* Input attachment coordinates come from fragment position, not the
     * zero coordinate operand emitted by SPIR-V subpassLoad. */
    for(unsigned test=0;test<2;test++) {
        unsigned layers=test?4:2,bytes=32*32*4*layers;
        uint8_t pixels[32*32*4*4];uint32_t meta[7016]={0};
        for(unsigned layer=0;layer<layers;layer++)for(unsigned y=0;y<32;y++)for(unsigned x=0;x<32;x++) {
            uint8_t *p=pixels+((layer*32+y)*32+x)*4;
            p[0]=test?32+layer*16+2*x:layer?64:2*x;
            p[1]=test?64+2*y:layer?32:2*y;p[2]=test?128:layer?16:128;p[3]=255;
        }
        for(unsigned slot=0;slot<(test?1u:2u);slot++) {
            uint32_t image[8]={32,32,test?4:1,0,4,(1u<<16)|((test?4u:1u)<<8),0x08080808u,0x18100800u};
            memcpy(meta+slot*8,image,sizeof image);
            meta[6912+slot*2]=slot*32*32*4;meta[6913+slot*2]=test?bytes:32*32*4;
        }
        virgl_res *arena=input_buffer(gpu,pixels,bytes),*metadata=input_buffer(gpu,meta,sizeof meta);
        if(!arena || !metadata){virgl_close(gpu);return 1;}
        for(unsigned samples=1;samples<=4;samples*=2) {
            struct zvk_metal_draw_info info={.vertex_shader=handles[0],.fragment_shader=handles[test?16:15],
                .format=ZVK_METAL_COLOR_RGBA8_UNORM,.base_format=ZVK_METAL_COLOR_RGBA8_UNORM,
                .width=32,.height=32,.samples=samples,.vertex_count=3,.instance_count=1};
            info.push[1][0]=2;
            struct zvk_metal_draw_binding bindings[2]={{1,0,arena->id,16,bytes},{1,28,metadata->id,16,sizeof meta}};
            struct virgl_metal_binding output={result->id,16,32*32*samples*16,1};
            memset(result->map,0xa7,result->size);
            if(virgl_res_to_host(gpu,result,0,0,0,0,result->size,1,1,0,0,0) ||
                virgl_metal_draw_buffers(gpu,&info,&output,0,NULL,2,bindings) ||
                virgl_res_from_host(gpu,result,0,0,0,0,result->size,1,1,0,0,0)) {virgl_close(gpu);return 1;}
            const float *values=(const float *)((const uint8_t *)result->map+16);
            for(unsigned sample=0;sample<samples;sample++)for(unsigned y=0;y<32;y++)for(unsigned x=0;x<32;x++) {
                const unsigned expected[4]={test?64+2*x:80,test?64+2*y:y+8,test?128:x+4,255};
                for(unsigned c=0;c<4;c++) {
                    float value=values[((sample*32+y)*32+x)*4+c];
                    if(!(value>=0 && value<=1) || (unsigned)(value*255+.5f)!=expected[c]) {
                        fprintf(stderr,"Metal input attachment case %u (%u,%u) channel %u: %g expected %u\n",test,x,y,c,value,expected[c]);virgl_close(gpu);return 1;
                    }
                }
            }
            printf("METAL_INPUT_TRANSPORT case=%u samples=%u PASS\n",test,samples);
        }
        if(!input_unchanged(gpu,arena,pixels,bytes) || !input_unchanged(gpu,metadata,meta,sizeof meta)){virgl_close(gpu);return 1;}
        virgl_res_destroy(gpu,arena);virgl_res_destroy(gpu,metadata);
    }
    for(unsigned test=0;test<4;test++) {
        uint32_t meta[7016]={0};
        const uint32_t image[8]={32,32,test==2?6u:1u,0,4,5u<<16,0x08080808u,0x18100800u};
        memcpy(meta,image,sizeof image);
        unsigned sampler=52*14*8;meta[sampler]=test==0?0:1u<<22;
        const float lod[4]={test==3?4:.5f,1,3,0};memcpy(meta+sampler+4,lod,sizeof lod);
        virgl_res *metadata=input_buffer(gpu,meta,sizeof meta);
        if(!metadata){virgl_close(gpu);return 1;}
        for(unsigned samples=1;samples<=4;samples*=2) {
            struct zvk_metal_draw_info info={.vertex_shader=handles[0],.fragment_shader=handles[test==2?20:19],
                .format=ZVK_METAL_COLOR_RGBA32_SFLOAT,.base_format=ZVK_METAL_COLOR_RGBA32_UINT,
                .width=32,.height=32,.samples=samples,.vertex_count=3,.instance_count=1,.flags=ZVK_METAL_DRAW_RAW};
            struct zvk_metal_draw_binding bindings[1]={{1,28,metadata->id,16,sizeof meta}};
            struct virgl_metal_binding output={result->id,16,32*32*samples*16,1};
            memset(result->map,0xa7,result->size);
            if(virgl_res_to_host(gpu,result,0,0,0,0,result->size,1,1,0,0,0) ||
                virgl_metal_draw_buffers(gpu,&info,&output,0,NULL,1,bindings) ||
                virgl_res_from_host(gpu,result,0,0,0,0,result->size,1,1,0,0,0)){virgl_close(gpu);return 1;}
            const uint8_t *bytes=result->map;
            for(unsigned i=0;i<result->size;i++)if((i<16 || i>=16+output.length)&&bytes[i]!=0xa7){virgl_close(gpu);return 1;}
            const uint32_t expected[4]={test==2?0x3f800000u:test==1?0x40200000u:0x40400000u,
                test==2?0xff800000u:test==3?0x40c00000u:0x40200000u,0,0x3f800000u};
            const uint32_t *values=(const uint32_t *)(bytes+16);
            for(unsigned i=0;i<32*32*samples;i++)if(memcmp(values+4*i,expected,16)) {
                fprintf(stderr,"Metal query LOD case %u samples %u texel %u failed: %08x %08x\n",test,samples,i,values[i*4],values[i*4+1]);virgl_close(gpu);return 1;
            }
            printf("METAL_QUERY_TRANSPORT case=%u samples=%u PASS\n",test,samples);
        }
        if(!input_unchanged(gpu,metadata,meta,sizeof meta)){virgl_close(gpu);return 1;}
        virgl_res_destroy(gpu,metadata);
    }
    virgl_res *depth_plane=virgl_res_create(gpu,PIPE_BUFFER,VIRGL_FORMAT_R8_UNORM,VIRGL_BIND_VERTEX_BUFFER,
        32*32*4*4+32,1,1,0,32*32*4*4+32);
    if(!depth_plane || !depth_plane->map){virgl_close(gpu);return 1;}
    for(unsigned samples=1;samples<=4;samples*=2) {
        struct virgl_metal_binding output={result->id,16,32*32*samples*16,1};
        for(unsigned early=0;early<2;early++) {
            struct zvk_metal_draw_info info={.vertex_shader=handles[0],.fragment_shader=handles[early?6:7],
                .format=ZVK_METAL_COLOR_RGBA8_UNORM,.base_format=ZVK_METAL_COLOR_RGBA8_UNORM,
                .width=32,.height=32,.samples=samples,.vertex_count=3,.instance_count=1};
            info.clear[2]=info.clear[3]=0x3f800000u;info.push[1][0]=info.push[1][3]=0x3f800000u;
            struct zvk_metal_depth_info depth={depth_plane->id,16,32*32*samples*4,
                ZVK_METAL_DEPTH_TEST|ZVK_METAL_DEPTH_WRITE,1,0x3f800000u};
            memset(result->map,0xa7,result->size);memset(depth_plane->map,0xc3,depth_plane->size);
            if(virgl_res_to_host(gpu,result,0,0,0,0,result->size,1,1,0,0,0) ||
                virgl_res_to_host(gpu,depth_plane,0,0,0,0,depth_plane->size,1,1,0,0,0)){virgl_close(gpu);return 1;}
            for(unsigned pass=0;pass<2;pass++) {
                info.push[0][4]=pass?0x3f000000u:0x3e800000u;
                if(pass){info.fragment_shader=handles[4];info.flags=ZVK_METAL_DRAW_LOAD;depth.flags|=ZVK_METAL_DEPTH_LOAD;}
                const unsigned color[4]={pass&&!early?255:0,0,pass&&!early?0:255,255};
                uint32_t expected=early?0x3e800000u:pass?0x3f000000u:0x3f800000u;
                if(virgl_metal_draw_depth(gpu,&info,&output,0,NULL,0,NULL,&depth) ||
                    !check_color(gpu,result,samples,color) || !check_depth(gpu,depth_plane,samples,expected)) {
                    fprintf(stderr,"Metal depth discard early=%u samples=%u pass=%u failed\n",early,samples,pass);virgl_close(gpu);return 1;
                }
                printf("METAL_DEPTH_TRANSPORT early=%u samples=%u pass=%u PASS\n",early,samples,pass);
            }
        }
        for(unsigned test=0;test<11;test++) {
            struct zvk_metal_draw_info info={.vertex_shader=handles[0],.fragment_shader=handles[4],
                .format=ZVK_METAL_COLOR_RGBA8_UNORM,.base_format=ZVK_METAL_COLOR_RGBA8_UNORM,
                .width=32,.height=32,.samples=samples,.vertex_count=test==10?0:3,.instance_count=1};
            info.clear[2]=info.clear[3]=0x3f800000u;info.push[1][0]=info.push[1][3]=0x3f800000u;
            info.push[0][4]=0x3f000000u;
            struct zvk_metal_depth_info depth={depth_plane->id,16,32*32*samples*4,
                ZVK_METAL_DEPTH_TEST|ZVK_METAL_DEPTH_WRITE,test<8?test:1,test==10?0x3f400000u:0x3f800000u};
            if(test==8)depth.flags=ZVK_METAL_DEPTH_WRITE;
            if(test==9)depth.flags=ZVK_METAL_DEPTH_TEST;
            memset(result->map,0xa7,result->size);memset(depth_plane->map,0xc3,depth_plane->size);
            if(virgl_res_to_host(gpu,result,0,0,0,0,result->size,1,1,0,0,0) ||
                virgl_res_to_host(gpu,depth_plane,0,0,0,0,depth_plane->size,1,1,0,0,0)){virgl_close(gpu);return 1;}
            int drawn=test<8?(test&1):test<10;
            const unsigned color[4]={drawn?255:0,0,drawn?0:255,255};
            uint32_t expected=test==10?0x3f400000u:drawn&&test<8?0x3f000000u:0x3f800000u;
            if(virgl_metal_draw_depth(gpu,&info,&output,0,NULL,0,NULL,&depth) ||
                !check_color(gpu,result,samples,color) || !check_depth(gpu,depth_plane,samples,expected)) {
                fprintf(stderr,"Metal depth compare/state test %u samples %u failed\n",test,samples);virgl_close(gpu);return 1;
            }
            printf("METAL_DEPTH_STATE_TRANSPORT case=%u samples=%u PASS\n",test,samples);
        }
    }
    for(unsigned samples=1;samples<=4;samples*=2) {
        memset(result->map,0xa7,result->size);memset(depth_plane->map,0xc3,depth_plane->size);
        uint32_t *initial=(uint32_t *)((uint8_t *)depth_plane->map+16);
        for(unsigned sample=0;sample<samples;sample++)for(unsigned y=0;y<32;y++)for(unsigned x=0;x<32;x++) {
            float value=.25f+(((sample+x)&1)? .5f:0)+(x+2*y)/1024.0f;
            memcpy(initial+(sample*32+y)*32+x,&value,4);
        }
        if(virgl_res_to_host(gpu,result,0,0,0,0,result->size,1,1,0,0,0) ||
            virgl_res_to_host(gpu,depth_plane,0,0,0,0,depth_plane->size,1,1,0,0,0)){virgl_close(gpu);return 1;}
        struct virgl_metal_binding output={result->id,16,32*32*samples*16,1};
        for(unsigned pass=0;pass<5;pass++) {
            struct zvk_metal_draw_info info={.vertex_shader=handles[0],.fragment_shader=handles[4],
                .format=ZVK_METAL_COLOR_RGBA8_UNORM,.base_format=ZVK_METAL_COLOR_RGBA8_UNORM,
                .width=32,.height=32,.samples=samples,.vertex_count=pass==1?0:3,.instance_count=1,
                .flags=pass?ZVK_METAL_DRAW_LOAD:0};
            info.clear[2]=info.clear[3]=0x3f800000u;
            info.push[0][4]=pass==4?0x3d800000u:pass==3?0x3e000000u:0x3f000000u;
            info.push[1][pass==4?2:pass==3?1:0]=0x3f800000u;info.push[1][3]=0x3f800000u;
            if(pass>=3) {
                info.flags|=ZVK_METAL_DRAW_SCISSOR;
                info.scissor[0]=info.scissor[1]=8;info.scissor[2]=pass==4?0:8;info.scissor[3]=8;
            }
            struct zvk_metal_depth_info depth={depth_plane->id,16,32*32*samples*4,
                ZVK_METAL_DEPTH_LOAD|ZVK_METAL_DEPTH_TEST|(pass==2?0:ZVK_METAL_DEPTH_WRITE),pass==2?7:1,0xffffffffu};
            if(virgl_metal_draw_depth(gpu,&info,&output,0,NULL,0,NULL,&depth) ||
                virgl_res_from_host(gpu,result,0,0,0,0,result->size,1,1,0,0,0) ||
                virgl_res_from_host(gpu,depth_plane,0,0,0,0,depth_plane->size,1,1,0,0,0)){virgl_close(gpu);return 1;}
            const uint8_t *color_bytes=result->map,*depth_bytes=depth_plane->map;
            for(unsigned i=0;i<result->size;i++)if((i<16 || i>=16+output.length)&&color_bytes[i]!=0xa7){virgl_close(gpu);return 1;}
            for(unsigned i=0;i<depth_plane->size;i++)if((i<16 || i>=16+depth.length)&&depth_bytes[i]!=0xc3){virgl_close(gpu);return 1;}
            const uint32_t *colors=(const uint32_t *)(color_bytes+16),*depths=(const uint32_t *)(depth_bytes+16);
            for(unsigned sample=0;sample<samples;sample++)for(unsigned y=0;y<32;y++)for(unsigned x=0;x<32;x++) {
                int accepted=(sample+x)&1,in_rect=pass>=3&&x>=8&&x<16&&y>=8&&y<16;
                float z=accepted?.5f:.25f+(x+2*y)/1024.0f;
                if(in_rect)z=.125f;
                uint32_t zbits;memcpy(&zbits,&z,4);
                unsigned channel=in_rect?1:pass>=2||accepted?0:2;
                uint32_t color[4]={0,0,0,0x3f800000u};color[channel]=0x3f800000u;
                unsigned i=(sample*32+y)*32+x;
                if(depths[i]!=zbits || memcmp(colors+4*i,color,16)) {
                    fprintf(stderr,"Metal per-sample depth case %u sample %u (%u,%u): %08x expected %08x\n",pass,sample,x,y,depths[i],zbits);virgl_close(gpu);return 1;
                }
            }
            printf("METAL_DEPTH_SAMPLES_TRANSPORT samples=%u pass=%u PASS\n",samples,pass);
        }
    }
    virgl_res_destroy(gpu,depth_plane);
    virgl_res_destroy(gpu,result);
    for(unsigned i=0;i<sizeof files/sizeof *files;i++)virgl_metal_destroy(gpu,handles[i]);
    if(virgl_flush_fenced(gpu)){virgl_close(gpu);return 1;}
    virgl_close(gpu);return 0;
}
