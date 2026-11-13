#version 450
layout(location=0) in vec3 varying_color;
layout(location=0) out vec4 color;
layout(set=0,binding=0,std140) uniform U { vec4 values[4]; } u;
layout(push_constant) uniform P { int index; } p;
void main() {
    vec4 result=vec4(0);
    for(int i=0;i<2;i++)result=u.values[p.index+i];
    color=result;
}
