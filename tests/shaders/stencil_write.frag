#version 450
layout(push_constant) uniform P { uint bit; uint sid; } p;
layout(set=0,binding=2,std430) readonly buffer Base { uint value[256]; } base;
void main() {
    uint value=(base.value[uint(gl_FragCoord.y)*16u+uint(gl_FragCoord.x)]+37u*p.sid)&255u;
    if((value&p.bit)==0u) discard;
}
