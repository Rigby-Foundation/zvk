#version 450
layout(location=0) in vec2 position;
layout(location=1) in vec3 color;
layout(location=0) out vec3 varying_color;
layout(push_constant) uniform Animation { vec4 transform; } animation;
void main() {
    float c=animation.transform.x, s=animation.transform.y;
    vec2 p=vec2(position.x*c-position.y*s,position.x*s+position.y*c);
    p.x*=animation.transform.z;
    p.y*=animation.transform.w;
    gl_Position=vec4(p,0.5,1.0);
    varying_color=color;
}
