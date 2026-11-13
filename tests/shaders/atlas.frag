#version 450
layout(location=0) out uvec4 color;
layout(set=0,binding=0) uniform U {uvec4 value;} ubos[12];
// The test remaps this binding in SPIR-V to exceed glslang's syntax limit.
layout(std430,set=0,binding=1) readonly buffer S {uint values[];} ssbos[8];
layout(set=0,binding=2) uniform utexture2D images[16];
layout(set=0,binding=3,r32ui) readonly uniform uimage2D storage_images[4];
layout(set=0,binding=4,input_attachment_index=0) uniform usubpassInput inputs[8];
layout(set=0,binding=5) uniform sampler samplers[16];
void main() {
    color=uvec4(0);
    uint index=uint(gl_FragCoord.x)%2u==0u?0u:24575u;
#define U(i) color.x+=ubos[i].value.x;
    U(0) U(1) U(2) U(3) U(4) U(5) U(6) U(7) U(8) U(9) U(10) U(11)
#define S(i) color.y+=ssbos[i].values[index]+uint(ssbos[i].values.length()==24576)*100u;
    S(0) S(1) S(2) S(3) S(4) S(5) S(6) S(7)
#define T(i,j) color.z+=textureLod(usampler2D(images[i],samplers[i]),vec2(0.5),0).x+textureLod(usampler2D(images[i],samplers[j]),vec2(0.5),0).x;
    T(0,1) T(1,2) T(2,3) T(3,4) T(4,5) T(5,6) T(6,7) T(7,8)
    T(8,9) T(9,10) T(10,11) T(11,12) T(12,13) T(13,14) T(14,15) T(15,0)
#define I(i) color.w+=imageLoad(storage_images[i],ivec2(0)).x;
    I(0) I(1) I(2) I(3)
#define A(i) color.w+=subpassLoad(inputs[i]).x;
    A(0) A(1) A(2) A(3) A(4) A(5) A(6) A(7)
}
