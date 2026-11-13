#version 450
layout(input_attachment_index=0, set=0, binding=0) uniform subpassInput first;
layout(input_attachment_index=1, set=0, binding=1) uniform subpassInput second;
layout(location=0) out vec4 color;
void main() {
    color = vec4(vec3(subpassLoad(first).r) * 0.5 + subpassLoad(second).rgb * 0.25, 1.0);
}
