#version 450
layout(location=0,component=0) in float red;
layout(location=0,component=1) in float green;
layout(location=0,component=2) in vec2 blue_alpha;
layout(location=0,component=0) out float out_red;
layout(location=0,component=1) out float out_green;
layout(location=0,component=2) out vec2 out_blue_alpha;
void main() { out_red=red;out_green=green;out_blue_alpha=blue_alpha; }
