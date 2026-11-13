#version 450
layout(input_attachment_index=0,set=0,binding=0) uniform usubpassInput st;
layout(input_attachment_index=1,set=0,binding=1) uniform subpassInput dep;
layout(location=0) out vec4 color;
void main() { color=vec4(float(subpassLoad(st).r)/255.0,subpassLoad(dep).r,1,1); }
