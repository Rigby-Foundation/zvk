#version 450
layout(set=0,binding=0) uniform usampler2DMS st;
layout(set=0,binding=1) uniform sampler2DMS dep;
layout(location=0) out vec4 color;
void main() {
    ivec2 p=ivec2(gl_FragCoord.xy); int sample_id=p.x/16; p.x%=16;
    color=vec4(float(texelFetch(st,p,sample_id).r)/255.0,texelFetch(dep,p,sample_id).r,
               all(equal(textureSize(st),ivec2(16))) && textureSamples(st)>sample_id?1.0:0.0,1.0);
}
