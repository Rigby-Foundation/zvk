#version 450
layout(set=0,binding=0) uniform sampler2D source;
layout(push_constant) uniform P { uint mode; } p;
layout(location=0) out vec4 color;
void main() {
    if(p.mode==0) color=textureLod(source,vec2(0.5),0.0);
    else if(p.mode==1) color=textureLod(source,vec2(0.5),2.0);
    else if(p.mode==2) color=textureGrad(source,vec2(0.5),vec2(0.125,0),vec2(0,0.125));
    else if(p.mode==3) color=texture(source,gl_FragCoord.xy*exp2(-25.0),16.0);
    else color=texture(source,gl_FragCoord.xy*exp2(19.0),-16.0);
}
