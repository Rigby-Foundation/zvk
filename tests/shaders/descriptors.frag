#version 450
layout(set=0,binding=0) uniform sampler2D images[2];
layout(location=0) out vec4 color;
void main() { color=textureLod(images[1],vec2(0.25),0); }
