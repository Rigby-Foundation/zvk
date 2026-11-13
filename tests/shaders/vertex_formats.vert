#version 450
layout(location=0) in ivec4 position;
layout(location=1) in uvec4 color;
layout(location=0) out vec3 varying_color;
void main() {
    gl_Position=vec4(vec2(position.xy)/10.0,0.5,1.0);
    varying_color=(position.z==0 && position.w==1 && color.w==1)?vec3(color.xyz)/255.0:vec3(0,1,0);
}
