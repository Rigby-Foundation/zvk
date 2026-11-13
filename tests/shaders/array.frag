#version 450
layout(set=0,binding=0) uniform sampler2DArray image;
layout(location=0) out vec4 color;
void main() { color=textureLod(image,vec3(0.25,0.25,2),1); }
