#version 450
layout(input_attachment_index=0,set=0,binding=0) uniform isubpassInputMS source;
layout(input_attachment_index=1,set=0,binding=1) uniform subpassInputMS depth_source;
layout(push_constant) uniform P { int sampleIndex; } p;
layout(location=0) out vec4 color;
void main() {
    color=vec4(subpassLoad(source,p.sampleIndex))/255.0;
    color.rb+=vec2(128.0/255.0);
    color.g=subpassLoad(depth_source,p.sampleIndex).r;
    color.a=1;
}
