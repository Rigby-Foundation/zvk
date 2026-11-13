#version 450
layout(push_constant) uniform P { uvec4 value; } p;
layout(location=0) out uvec4 color;
void main() { color=p.value; }
