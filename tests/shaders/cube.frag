#version 450
layout(set=0,binding=0) uniform samplerCube image;
layout(location=0) out vec4 color;
void main() { color=textureLod(image,vec3(1,0,0),2); }
