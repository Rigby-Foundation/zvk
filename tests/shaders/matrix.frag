#version 450
layout(set=0,binding=0,std140,row_major) uniform Data { mat3 transform; vec4 colors[2]; } data;
layout(location=0) out vec4 color;
void main() { color=vec4(data.transform * data.colors[1].xyz,1.0); }
