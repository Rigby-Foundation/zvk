#version 450
layout(push_constant) uniform P { ivec4 value; } p;
layout(location=0) out ivec4 color;
void main() { color=p.value; }
