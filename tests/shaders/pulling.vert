#version 450
layout(location=0) in vec4 attributes[16];
layout(location=0) out vec3 color;
void main() {
    bool good=true;
    for(int i=0;i<16;i++) {
        float expected=float(i*100+((i&1)==0?gl_VertexIndex:gl_InstanceIndex));
        good=good&&all(equal(attributes[i],vec4(expected)));
    }
    color=good?vec3(0,1,0):vec3(1,0,0);
    vec2 positions[3]=vec2[3](vec2(-1,-1),vec2(3,-1),vec2(-1,3));
    gl_Position=vec4(positions[gl_VertexIndex%3],0,1);
}
