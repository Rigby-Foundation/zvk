#version 450
layout(set=0,binding=0) uniform sampler2DMS source;
layout(set=0,binding=1) uniform sampler2DMS depth_source;
layout(push_constant) uniform P { int sampleIndex; int layer; int samples; } p;
layout(location=0) out vec4 color;
void main() {
    color=texelFetch(source,ivec2(gl_FragCoord.xy),p.sampleIndex);
    color.g=texelFetch(depth_source,ivec2(gl_FragCoord.xy),p.sampleIndex).r;
    if(textureSize(source)!=ivec2(32) || textureSamples(source)!=p.samples)color=vec4(1,0,1,1);
}
