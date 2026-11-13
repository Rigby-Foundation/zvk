#version 450
layout(location=0,component=0) out float red;
layout(location=0,component=1) out float green;
layout(location=0,component=2) out vec2 blue_alpha;
void main() {
    vec2 p[3]=vec2[3](vec2(-1,-1),vec2(3,-1),vec2(-1,3));
    gl_Position=vec4(p[gl_VertexIndex],.5,1);
    red=.25;green=.5;blue_alpha=vec2(.75,1);
}
