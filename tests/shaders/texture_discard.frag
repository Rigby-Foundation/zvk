#version 450
layout(set=0,binding=0) uniform sampler2D image;
layout(location=0) out vec4 color;
void main() {
    if(texture(image,gl_FragCoord.xy/32.0).r<0.5) discard;
    color=vec4(0,1,0,1);
}
