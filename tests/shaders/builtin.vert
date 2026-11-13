#version 450
layout(location=0) out vec3 varying_color;
void main() {
    vec2 positions[3]=vec2[3](vec2(-.8,-.8),vec2(.8,-.8),vec2(0,.8));
    gl_Position=vec4(positions[gl_VertexIndex],0.5,1.0);
    varying_color=gl_InstanceIndex==1?vec3(0,1,0):vec3(1,0,0);
}
