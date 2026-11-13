#version 450
layout(set=0,binding=0) uniform usampler1D image;
layout(location=0) out vec4 color;
void main() { color=vec4(texture(image,0.25)); }
