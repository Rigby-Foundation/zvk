#version 450
layout(location=0) in ivec2 position;
layout(location=1) in uvec3 color;
layout(location=0) out vec3 varying_color;
void main() { gl_Position=vec4(vec2(position)/10.0,0.5,1.0); varying_color=vec3(color)/255.0; }
