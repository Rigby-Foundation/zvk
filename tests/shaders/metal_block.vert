#version 450
out Packed {
    layout(location=0,component=0) float red;
    layout(location=0,component=1) float green;
    layout(location=0,component=2) vec2 blue_alpha;
} packed;
layout(location=1,component=0) out vec2 low[2];
layout(location=1,component=2) out vec2 high[2];
void main() {
    vec2 p[3]=vec2[3](vec2(-1,-1),vec2(3,-1),vec2(-1,3));
    gl_Position=vec4(p[gl_VertexIndex],.5,1);
    packed.red=.125;packed.green=.25;packed.blue_alpha=vec2(.375,.5);
    low[0]=vec2(.0625,.125);high[0]=vec2(.1875,.25);
    low[1]=low[0];high[1]=high[0];
}
