#version 450
layout(set=0,binding=0) uniform isampler2DMSArray source;
layout(set=0,binding=1) uniform sampler2DMSArray depth_source;
layout(push_constant) uniform P { int sampleIndex; int layer; int samples; int swizzle; } p;
layout(location=0) out vec4 color;
void main() {
    ivec4 value=texelFetch(source,ivec3(ivec2(gl_FragCoord.xy),p.layer),p.sampleIndex);
    color=vec4(value)/255.0;
    color.rb+=vec2(128.0/255.0);
    color.g=texelFetch(depth_source,ivec3(ivec2(gl_FragCoord.xy),p.layer),p.sampleIndex).r;
    color.a=1;
    if(textureSize(source)!=ivec3(32,32,2) || textureSamples(source)!=p.samples ||
       value.a!=(p.swizzle!=0?1:-1))color=vec4(1,0,1,1);
}
