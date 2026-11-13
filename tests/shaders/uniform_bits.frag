#version 450
layout(set=0,binding=0) uniform Bits { uvec4 values[64]; } bits;
layout(location=0) out vec4 color;
void main() {
    uint value=(uint(gl_FragCoord.y)*32u+uint(gl_FragCoord.x))^0x9e3779b9u;
    for (uint i=0;i<4;i++) {
        uvec4 v=bits.values[value%64u];
        value=v[value&3u];
    }
    color=vec4(vec3(uvec3(value,value>>8u,value>>16u)&uvec3(255u))/255.0,1);
}
