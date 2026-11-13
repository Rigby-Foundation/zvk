#version 450
layout(set=0,binding=0) uniform Data { vec4 value; } data[2];
layout(location=0) out vec4 color;
void main() { color=data[1].value; }
