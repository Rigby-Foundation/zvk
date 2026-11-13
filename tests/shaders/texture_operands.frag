#version 450
layout(set=0,binding=0) uniform texture2D source;
layout(set=0,binding=1) uniform sampler repeat_sampler;
layout(set=0,binding=2) uniform sampler clamp_sampler;
layout(push_constant) uniform P { int mode; } p;
layout(location=0) out vec4 color;
void main() {
    vec2 uv=vec2(8.5/16.0);
    if(p.mode==0)color=texelFetchOffset(sampler2D(source,repeat_sampler),ivec2(8),0,ivec2(-8));
    else if(p.mode==1)color=textureLodOffset(sampler2D(source,repeat_sampler),uv,0,ivec2(7));
    else if(p.mode==2)color=textureOffset(sampler2D(source,repeat_sampler),uv,ivec2(-8,7),0.0);
    else if(p.mode==3)color=textureGradOffset(sampler2D(source,repeat_sampler),uv,vec2(1.0/16.0,0),vec2(0,1.0/16.0),ivec2(7,-8));
    else if(p.mode==4)color=texture(sampler2D(source,repeat_sampler),vec2(1.2,.1));
    else color=texture(sampler2D(source,clamp_sampler),vec2(1.2,.1));
    if(textureSize(sampler2D(source,repeat_sampler),0)!=ivec2(16) || textureQueryLevels(sampler2D(source,repeat_sampler))!=1)
        color=vec4(1,0,1,1);
}
