#version 450
layout(push_constant) uniform Params { float depth; float slope; } params;
void main() {
    vec2 p=gl_VertexIndex==0?vec2(-1,-1):gl_VertexIndex==1?vec2(3,-1):vec2(-1,3);
    gl_Position=vec4(p,params.depth+params.slope*p.x,1);
}
