#version 450
layout(constant_id=0) const float red=0.25;
layout(constant_id=1) const bool selected=false;
layout(location=0) out vec4 color;
void main() { color=vec4(red,selected?0.5:0.0,1.0,1.0); }
