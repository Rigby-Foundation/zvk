#version 450
layout(push_constant) uniform Push { vec4 color; } pc;
layout(location=0) out vec4 color;
float adjust(float x) { return x*0.5; }
void main() {
    vec3 value=vec3(0.0);
    for (int i=0; i<3; ++i) {
        if (i==1) value[i]=pc.color[i];
        else value[i]=adjust(pc.color[i]);
    }
    color=vec4(value,1.0);
}
