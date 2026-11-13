#version 450
layout(push_constant) uniform Values { vec4 value; } p;
layout(location=0) out vec4 color;
void main() {
    mat2 m = mat2(p.value.y, 0.0, 0.0, p.value.z);
    float whole;
    float fraction = modf(p.value.w, whole);
    color = vec4(asin(p.value.x) / 3.141592653589793,
                 pow(p.value.y, p.value.z),
                 determinant(inverse(m)) * fraction, whole / 4.0);
}
