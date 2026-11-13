#version 450
layout(push_constant) uniform Push { vec4 values; } pc;
layout(location=0) out vec4 color;
void main() {
    float n=uintBitsToFloat(floatBitsToUint(pc.values.x)|0x7fc00000u);
    color=vec4(n!=0.0?1.0:0.0,isnan(n)?1.0:0.0,n==n?1.0:0.0,1.0);
}
