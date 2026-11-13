#version 450
layout(set=0,binding=0) uniform sampler2DMSArray source;
layout(set=0,binding=1) uniform sampler2DMSArray depth_source;
layout(push_constant) uniform P { int sampleIndex; int layer; int samples; } p;
layout(location=0) out vec4 color;
void main() {
    ivec3 pos=ivec3(ivec2(gl_FragCoord.xy),p.layer);
    color=texelFetch(source,pos,p.sampleIndex);
    color.g=texelFetch(depth_source,pos,p.sampleIndex).r;
    if(textureSize(source)!=ivec3(32,32,2) || textureSamples(source)!=p.samples)color=vec4(1,0,1,1);
}
