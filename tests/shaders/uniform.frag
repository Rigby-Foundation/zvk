#version 450
layout(set=0,binding=0,std140) uniform Data { layout(offset=32) vec4 color; } data;
layout(location=0) out vec4 color;
void main() { color=data.color; }
