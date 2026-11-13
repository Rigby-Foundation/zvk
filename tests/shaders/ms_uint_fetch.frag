#version 450
layout(set=0,binding=0) uniform usampler2DMS source;
layout(set=0,binding=1) uniform sampler2DMS depth_source;
layout(push_constant) uniform P { int sampleIndex; int layer; int samples; int swizzle; } p;
layout(location=0) out vec4 color;
void main() {
    uvec4 value=texelFetch(source,ivec2(gl_FragCoord.xy),p.sampleIndex);
    color=vec4(value)/255.0;

    color.g=texelFetch(depth_source,ivec2(gl_FragCoord.xy),p.sampleIndex).r;
    color.a=1;
    if(textureSize(source)!=ivec2(32) || textureSamples(source)!=p.samples ||
       value.a!=(p.swizzle!=0?1u:255u))color=vec4(1,0,1,1);
}
