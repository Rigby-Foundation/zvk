#version 450
layout(location=0) out vec4 color;
void main() {
    gl_SampleMask[0]=0;
    color=vec4(1,0,0,1);
}
