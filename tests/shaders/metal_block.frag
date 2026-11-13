#version 450
in Packed {
    layout(location=0,component=0) float red;
    layout(location=0,component=1) float green;
    layout(location=0,component=2) vec2 blue_alpha;
} packed;
layout(location=1,component=0) in vec2 low[2];
layout(location=1,component=2) in vec2 high[2];
layout(location=0) out vec4 color;
void main() { color=vec4(packed.red,packed.green,packed.blue_alpha)+vec4(low[0]+low[1],high[0]+high[1]); }
