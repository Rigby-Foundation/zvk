/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Native GPU acceptance of zVk's generated vertex/fragment MSL. */
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#include <stdint.h>
#include <stdio.h>

static int complete(id<MTLCommandBuffer> command)
{
    [command commit];[command waitUntilCompleted];
    if(command.status==MTLCommandBufferStatusCompleted)return 1;
    fprintf(stderr,"Metal command: %s\n",command.error.description.UTF8String);return 0;
}
static id<MTLLibrary> compile(id<MTLDevice> device,NSString *directory,NSString *name)
{
    NSError *error=nil;
    NSString *path=[directory stringByAppendingPathComponent:[name stringByAppendingString:@".msl"]];
    NSString *source=[NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:&error];
    MTLCompileOptions *options=[MTLCompileOptions new];options.fastMathEnabled=NO;
    id<MTLLibrary> library=source?[device newLibraryWithSource:source options:options error:&error]:nil;
    if(!library)fprintf(stderr,"%s: %s\n",name.UTF8String,error.description.UTF8String);
    return library;
}
static id<MTLRenderPipelineState> pipeline(id<MTLDevice> device,id<MTLLibrary> vertex,
    id<MTLLibrary> fragment,MTLPixelFormat format,unsigned samples,int depth,int attributes)
{
    MTLRenderPipelineDescriptor *p=[MTLRenderPipelineDescriptor new];
    p.vertexFunction=[vertex newFunctionWithName:@"zvk_vertex"];
    p.fragmentFunction=[fragment newFunctionWithName:@"zvk_fragment"];
    p.colorAttachments[0].pixelFormat=format;p.rasterSampleCount=samples;
    if(depth)p.depthAttachmentPixelFormat=MTLPixelFormatDepth32Float;
    if(attributes) {
        MTLVertexDescriptor *v=[MTLVertexDescriptor vertexDescriptor];
        v.attributes[0].format=MTLVertexFormatFloat2;v.attributes[0].bufferIndex=1;
        v.attributes[1].format=MTLVertexFormatFloat3;v.attributes[1].bufferIndex=1;v.attributes[1].offset=8;
        v.layouts[1].stride=20;p.vertexDescriptor=v;
    }
    NSError *error=nil;
    id<MTLRenderPipelineState> result=[device newRenderPipelineStateWithDescriptor:p error:&error];
    if(!result)fprintf(stderr,"Metal pipeline: %s\n",error.description.UTF8String);return result;
}
static int draw(id<MTLDevice> device,id<MTLCommandQueue> queue,id<MTLRenderPipelineState> pipeline,
    id<MTLTexture> color,id<MTLTexture> depth,int clear,const void *vp,const void *fp,
    id<MTLBuffer> vertices,unsigned first_instance,NSDictionary<NSNumber *,id<MTLBuffer>> *fragment_buffers)
{
    MTLRenderPassDescriptor *pass=[MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture=color;pass.colorAttachments[0].clearColor=MTLClearColorMake(0,0,1,1);
    pass.colorAttachments[0].loadAction=clear?MTLLoadActionClear:MTLLoadActionLoad;
    pass.colorAttachments[0].storeAction=MTLStoreActionStore;
    if(depth) {
        pass.depthAttachment.texture=depth;pass.depthAttachment.clearDepth=1;
        pass.depthAttachment.loadAction=clear?MTLLoadActionClear:MTLLoadActionLoad;
        pass.depthAttachment.storeAction=MTLStoreActionStore;
    }
    id<MTLCommandBuffer> command=[queue commandBuffer];
    id<MTLRenderCommandEncoder> encoder=[command renderCommandEncoderWithDescriptor:pass];
    [encoder setRenderPipelineState:pipeline];
    if(depth) {
        MTLDepthStencilDescriptor *d=[MTLDepthStencilDescriptor new];
        d.depthCompareFunction=MTLCompareFunctionLess;d.depthWriteEnabled=YES;
        [encoder setDepthStencilState:[device newDepthStencilStateWithDescriptor:d]];
    }
    if(vertices)[encoder setVertexBuffer:vertices offset:0 atIndex:1];
    for(NSNumber *index in fragment_buffers)
        [encoder setFragmentBuffer:fragment_buffers[index] offset:0 atIndex:index.unsignedIntegerValue];
    [encoder setVertexBytes:vp length:128 atIndex:30];[encoder setFragmentBytes:fp length:128 atIndex:30];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3 instanceCount:1 baseInstance:first_instance];
    [encoder endEncoding];return complete(command);
}
static int pixels(id<MTLTexture> image,unsigned r,unsigned g,unsigned b,int all)
{
    uint8_t bytes[4096];
    [image getBytes:bytes bytesPerRow:128 fromRegion:MTLRegionMake2D(0,0,32,32) mipmapLevel:0];
    for(unsigned i=0;i<1024;i++) {
        if(!all && i!=16*32+16)continue;
        if(bytes[4*i]!=r || bytes[4*i+1]!=g || bytes[4*i+2]!=b || bytes[4*i+3]!=255) {
            fprintf(stderr,"Metal pixel %u: %u %u %u %u expected %u %u %u 255\n",i,
                bytes[4*i],bytes[4*i+1],bytes[4*i+2],bytes[4*i+3],r,g,b);return 0;
        }
    }
    return 1;
}
int main(int argc,char **argv)
{
    @autoreleasepool {
        if(argc!=2)return 2;
        id<MTLDevice> device=MTLCreateSystemDefaultDevice();
        if(!device || ![device supportsTextureSampleCount:4])return 2;
        id<MTLCommandQueue> queue=[device newCommandQueue];
        NSString *directory=[NSString stringWithUTF8String:argv[1]];
        NSArray<NSString *> *names=@[@"vertex",@"vertex_varying",@"vertex_attributes",@"fragment_uint",
            @"fragment_float",@"fragment_varying",@"fragment_early",@"fragment_late",
            @"fragment_derivatives",@"fragment_structured",@"fragment_selection_exit",@"fragment_texture",@"fragment_centroid",@"vertex_components",@"fragment_components",@"fragment_inputs",@"fragment_input_ms",@"vertex_blocks",@"fragment_blocks",@"fragment_query_lod",@"fragment_query_cube"];
        NSMutableDictionary<NSString *,id<MTLLibrary>> *libraries=[NSMutableDictionary new];
        for(NSString *name in names) {
            id<MTLLibrary> lib=compile(device,directory,name);if(!lib)return 1;libraries[name]=lib;
            printf("METAL_STAGE_COMPILE %s PASS\n",name.UTF8String);
        }
        MTLTextureDescriptor *td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
            width:32 height:32 mipmapped:NO];
        td.textureType=MTLTextureType2DMultisample;td.sampleCount=4;td.storageMode=MTLStorageModePrivate;
        td.usage=MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead|MTLTextureUsagePixelFormatView;
        id<MTLTexture> base=[device newTextureWithDescriptor:td];
        id<MTLTexture> integer=[base newTextureViewWithPixelFormat:MTLPixelFormatR32Uint];
        if(!integer){fprintf(stderr,"Metal compatible integer multisample view unavailable\n");return 1;}
        id<MTLRenderPipelineState> p=pipeline(device,libraries[@"vertex"],libraries[@"fragment_uint"],MTLPixelFormatR32Uint,4,0,0);
        float vertex_push[32]={0};vertex_push[4]=.25f;
        uint32_t integer_push[32]={0x12345678,0x9abcdef0,0,1};
        if(!p || !draw(device,queue,p,integer,nil,1,vertex_push,integer_push,nil,0,nil))return 1;
        NSString *source=@"#include <metal_stdlib>\nusing namespace metal;\n"
            "kernel void read_uint(texture2d_ms<uint,access::read> t [[texture(0)]],device uint *o [[buffer(0)]],uint3 p [[thread_position_in_grid]]){o[(p.z*32+p.y)*32+p.x]=t.read(p.xy,p.z).x;}\n"
            "kernel void read_color(texture2d_ms<float,access::read> t [[texture(0)]],device uint4 *o [[buffer(0)]],uint3 p [[thread_position_in_grid]]){o[(p.z*32+p.y)*32+p.x]=uint4(round(t.read(p.xy,p.z)*255.0f));}\n";
        NSError *error=nil;id<MTLLibrary> readback=[device newLibraryWithSource:source options:nil error:&error];
        if(!readback){fprintf(stderr,"%s\n",error.description.UTF8String);return 1;}
        id<MTLBuffer> out=[device newBufferWithLength:4096*16 options:MTLResourceStorageModeShared];
        for(NSString *name in @[@"read_uint",@"read_color"]) {
            id<MTLComputePipelineState> cs=[device newComputePipelineStateWithFunction:[readback newFunctionWithName:name] error:&error];
            if(!cs)return 1;
            id<MTLCommandBuffer> command=[queue commandBuffer];id<MTLComputeCommandEncoder> encoder=[command computeCommandEncoder];
            [encoder setComputePipelineState:cs];[encoder setTexture:[name isEqualToString:@"read_uint"]?integer:base atIndex:0];
            [encoder setBuffer:out offset:0 atIndex:0];
            [encoder dispatchThreadgroups:MTLSizeMake(4,4,4) threadsPerThreadgroup:MTLSizeMake(8,8,1)];
            [encoder endEncoding];if(!complete(command))return 1;
            uint32_t *values=out.contents;
            for(unsigned i=0;i<4096;i++) {
                if([name isEqualToString:@"read_uint"]){if(values[i]!=0x12345678)return 1;}
                else if(values[i*4]!=0x78 || values[i*4+1]!=0x56 || values[i*4+2]!=0x34 || values[i*4+3]!=0x12)return 1;
            }
        }
        puts("METAL_SPIRV_INTEGER_MS4_MUTABLE_BITS_PASS");
        td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:32 height:32 mipmapped:NO];
        td.usage=MTLTextureUsageRenderTarget;td.storageMode=MTLStorageModeShared;
        id<MTLTexture> color=[device newTextureWithDescriptor:td];
        td.pixelFormat=MTLPixelFormatDepth32Float;td.storageMode=MTLStorageModePrivate;
        id<MTLTexture> depth=[device newTextureWithDescriptor:td];
        float fragment_push[32]={1,0,0,1};
        for(unsigned early=0;early<2;early++) {
            for(unsigned pass=0;pass<2;pass++) {
                NSString *fs=pass?@"fragment_float":early?@"fragment_early":@"fragment_late";
                p=pipeline(device,libraries[@"vertex"],libraries[fs],MTLPixelFormatRGBA8Unorm,1,1,0);
                vertex_push[4]=pass?.5f:.25f;
                if(!p || !draw(device,queue,p,color,depth,!pass,vertex_push,fragment_push,nil,0,nil))return 1;
            }
            if(!pixels(color,early?0:255,0,early?255:0,1))return 1;
            printf("METAL_SPIRV_%s_DEPTH_DISCARD_PASS\n",early?"EARLY":"LATE");
        }
        p=pipeline(device,libraries[@"vertex_varying"],libraries[@"fragment_varying"],MTLPixelFormatRGBA8Unorm,1,0,0);
        for(unsigned instance=0;instance<2;instance++) {
            if(!p || !draw(device,queue,p,color,nil,1,vertex_push,fragment_push,nil,instance,nil) ||
                !pixels(color,instance?0:255,instance?255:0,0,0))return 1;
        }
        puts("METAL_SPIRV_VARYING_FIRST_INSTANCE_PASS");
        p=pipeline(device,libraries[@"vertex_varying"],libraries[@"fragment_centroid"],MTLPixelFormatRGBA8Unorm,1,0,0);
        if(!p || !draw(device,queue,p,color,nil,1,vertex_push,fragment_push,nil,1,nil) || !pixels(color,0,255,0,0))return 1;
        puts("METAL_SPIRV_CENTROID_INTERFACE_PASS");
        const float data[3][5]={{-1,-1,1,0,1},{3,-1,1,0,1},{-1,3,1,0,1}};
        id<MTLBuffer> vertices=[device newBufferWithBytes:data length:sizeof data options:MTLResourceStorageModeShared];
        p=pipeline(device,libraries[@"vertex_attributes"],libraries[@"fragment_varying"],MTLPixelFormatRGBA8Unorm,1,0,1);
        if(!p || !draw(device,queue,p,color,nil,1,vertex_push,fragment_push,vertices,0,nil) || !pixels(color,255,0,255,1))return 1;
        puts("METAL_SPIRV_VERTEX_ATTRIBUTES_PASS");
        for(NSString *fs in @[@"fragment_structured",@"fragment_selection_exit"]) {
            p=pipeline(device,libraries[@"vertex"],libraries[fs],MTLPixelFormatRGBA8Unorm,1,0,0);
            if(!p || !draw(device,queue,p,color,nil,1,vertex_push,fragment_push,nil,0,nil) || !pixels(color,64,128,191,1))return 1;
            printf("METAL_SPIRV_%s_RECONVERGENCE_PASS\n",fs.UTF8String);
        }
        td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float width:32 height:32 mipmapped:NO];
        td.usage=MTLTextureUsageRenderTarget;td.storageMode=MTLStorageModeShared;
        id<MTLTexture> derivatives=[device newTextureWithDescriptor:td];
        p=pipeline(device,libraries[@"vertex"],libraries[@"fragment_derivatives"],MTLPixelFormatRGBA32Float,1,0,0);
        if(!p || !draw(device,queue,p,derivatives,nil,1,vertex_push,fragment_push,nil,0,nil))return 1;
        float values[32*32*4];[derivatives getBytes:values bytesPerRow:32*16 fromRegion:MTLRegionMake2D(0,0,32,32) mipmapLevel:0];
        for(unsigned y=0;y<32;y++)for(unsigned x=0;x<32;x++) {
            const float expected[4]={y+.5f,x+.5f,(y&~1u)+.5f,(x&~1u)+.5f};
            for(unsigned c=0;c<4;c++)if(values[(y*32+x)*4+c]!=expected[c]) {
                fprintf(stderr,"Metal derivative (%u,%u) channel %u: %g expected %g\n",x,y,c,values[(y*32+x)*4+c],expected[c]);return 1;
            }
        }
        puts("METAL_SPIRV_FINE_COARSE_DERIVATIVES_PASS");
        p=pipeline(device,libraries[@"vertex_components"],libraries[@"fragment_components"],MTLPixelFormatRGBA8Unorm,1,0,0);
        if(!p || !draw(device,queue,p,color,nil,1,vertex_push,fragment_push,nil,0,nil) || !pixels(color,64,128,191,1))return 1;
        puts("METAL_SPIRV_COMPONENT_PACKED_INTERFACE_PASS");
        p=pipeline(device,libraries[@"vertex_blocks"],libraries[@"fragment_blocks"],MTLPixelFormatRGBA8Unorm,1,0,0);
        if(!p || !draw(device,queue,p,color,nil,1,vertex_push,fragment_push,nil,0,nil) || !pixels(color,64,128,191,1))return 1;
        puts("METAL_SPIRV_COMPONENT_PACKED_BLOCK_ARRAY_PASS");
        uint32_t metadata[7016]={0}; /* compute_metadata byte ABI, 28064 bytes */
        uint32_t image[8]={32,32,1,0,4,(1u<<16)|(15u<<8),0x08080808u,0x18100800u};
        memcpy(metadata,image,sizeof image);memcpy(metadata+52*8,image,sizeof image);
        unsigned sampler=52*14*8;
        for(unsigned c=0;c<4;c++)metadata[sampler+12+c]=c;
        metadata[6912]=0;metadata[6913]=32*32*4;
        uint8_t texels[32*32*4];
        for(unsigned i=0;i<32*32;i++){texels[i*4]=64;texels[i*4+1]=128;texels[i*4+2]=191;texels[i*4+3]=255;}
        id<MTLBuffer> arena=[device newBufferWithBytes:texels length:sizeof texels options:MTLResourceStorageModeShared];
        id<MTLBuffer> info=[device newBufferWithBytes:metadata length:sizeof metadata options:MTLResourceStorageModeShared];
        p=pipeline(device,libraries[@"vertex"],libraries[@"fragment_texture"],MTLPixelFormatRGBA8Unorm,1,0,0);
        if(!p || !draw(device,queue,p,color,nil,1,vertex_push,fragment_push,nil,0,@{@0:arena,@28:info}) || !pixels(color,64,128,191,1))return 1;
        puts("METAL_SPIRV_PACKED_TEXTURE_IMPLICIT_LOD_PASS");
        for(unsigned test=0;test<4;test++) {
            memset(metadata,0,sizeof metadata);
            const uint32_t query_image[8]={32,32,test==2?6u:1u,0,4,5u<<16,0x08080808u,0x18100800u};
            memcpy(metadata,query_image,sizeof query_image);
            metadata[sampler]=test==0?0:1u<<22;
            const float lod_state[4]={test==3?4:.5f,1,3,0};
            memcpy(metadata+sampler+4,lod_state,sizeof lod_state);
            info=[device newBufferWithBytes:metadata length:sizeof metadata options:MTLResourceStorageModeShared];
            p=pipeline(device,libraries[@"vertex"],libraries[test==2?@"fragment_query_cube":@"fragment_query_lod"],MTLPixelFormatRGBA32Float,1,0,0);
            if(!p || !draw(device,queue,p,derivatives,nil,1,vertex_push,fragment_push,nil,0,@{@0:arena,@28:info}))return 1;
            [derivatives getBytes:values bytesPerRow:32*16 fromRegion:MTLRegionMake2D(0,0,32,32) mipmapLevel:0];
            uint32_t expected[4]={test==2?0x3f800000u:test==1?0x40200000u:0x40400000u,
                test==2?0xff800000u:test==3?0x40c00000u:0x40200000u,0,0x3f800000u};
            for(unsigned i=0;i<1024;i++)if(memcmp(values+i*4,expected,16)) {
                fprintf(stderr,"Metal query LOD case %u pixel %u: %g %g\n",test,i,values[i*4],values[i*4+1]);return 1;
            }
            printf("METAL_SPIRV_QUERY_LOD case=%u PASS\n",test);
        }

        return 0;
    }
}
