#version 450
layout(set=0,binding=0) uniform usampler3D image;
layout(location=0) out vec4 color;
void main() { color=vec4(texture(image,vec3(0.25,0.25,0.75))); }
