#version 450
layout(location=0) out vec4 color;
void main() {
    gl_SampleMask[0]=1<<gl_SampleID;
    color=vec4(float(gl_SampleID)/4.0,gl_SamplePosition,1);
}
