#version 450
layout(location=0) out vec4 color;
void main() {
    float value=gl_FragCoord.x*gl_FragCoord.y;
    color=vec4(dFdxFine(value),dFdyFine(value),dFdxCoarse(value),dFdyCoarse(value));
}
