#version 450
layout(set=0,binding=0) uniform usampler2D st;
layout(set=0,binding=1) uniform sampler2D dep;
layout(location=0) out vec4 color;
void main() {
    ivec2 p=ivec2(gl_FragCoord.xy);
    color=vec4(float(texelFetch(st,p,0).r)/255.0,texelFetch(dep,p,0).r,
               all(equal(textureSize(st,0),ivec2(16)))?1.0:0.0,1.0);
}
