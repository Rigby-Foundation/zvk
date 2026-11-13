#version 450
layout(set=0,binding=0) uniform usampler1DArray image;
layout(location=0) out vec4 color;
void main() { color=vec4(texture(image,vec2(0.25,2.0))); }
