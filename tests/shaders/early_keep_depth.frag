#version 450
layout(early_fragment_tests) in;
layout(location=0) out vec4 color;
void main() { color=vec4(0,1,0,1);gl_FragDepth=1.0; }
