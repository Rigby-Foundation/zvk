#version 450
layout(location=0) centroid in vec3 value;
layout(location=0) out vec4 color;
void main() { color=vec4(value,1); }
