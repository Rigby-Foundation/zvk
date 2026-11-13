#version 450
layout(set=0,binding=0) uniform Data { vec4 value; } data[12];
layout(location=0) out vec4 color;
void main() {
    color = (data[0].value + data[1].value + data[2].value + data[3].value +
             data[4].value + data[5].value + data[6].value + data[7].value +
             data[8].value + data[9].value + data[10].value + data[11].value) / 12.0;
}
