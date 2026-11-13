#version 450
layout(location=0) in vec4 value;
layout(location=0) out vec3 varying_color;
layout(push_constant) uniform P { int first; int mode; } p;
void main() {
    vec2 positions[3]=vec2[3](vec2(-.8,-.8),vec2(.8,-.8),vec2(0,.8));
    gl_Position=vec4(positions[gl_VertexIndex-p.first],.5,1);
    vec4 expected=(p.mode==1 || (p.mode==2 && gl_VertexIndex==0))?vec4(1):vec4(0);
    varying_color=all(equal(value,expected))?vec3(0,1,0):vec3(1,0,0);
}
