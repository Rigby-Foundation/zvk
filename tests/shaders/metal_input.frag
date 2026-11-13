#version 450
layout(input_attachment_index=0,set=0,binding=0) uniform subpassInputMS source;
layout(push_constant) uniform P { int sample_index; } p;
layout(location=0) out vec4 color;
void main() { color=subpassLoad(source,p.sample_index); }
