#version 450
layout(input_attachment_index=0,set=0,binding=0) uniform usubpassInputMS st;
layout(input_attachment_index=1,set=0,binding=1) uniform subpassInputMS dep;
layout(push_constant) uniform P { uint count; } pc;
layout(location=0) out vec4 color;
void main() {
    uint sum=0;
    for(uint sid=0;sid<pc.count;sid++) sum+=subpassLoad(st,int(sid)).r;
    color=vec4(float(sum&255u)/255.0,subpassLoad(dep,0).r,1,1);
}
