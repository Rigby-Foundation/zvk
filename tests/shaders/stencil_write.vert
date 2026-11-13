#version 450
layout(push_constant) uniform P { vec4 color; float depth; } p;
layout(set=0,binding=2,std430) readonly buffer Base { uint value[256]; } base;
void main() {
    vec2 pos[3]=vec2[3](vec2(-1,-1),vec2(3,-1),vec2(-1,3));
    gl_Position=vec4(pos[base.value[gl_VertexIndex]],p.depth,1);
}
