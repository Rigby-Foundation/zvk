#version 450
layout(set=0,binding=0) uniform sampler2DMS textures[16];
void main() {
int sid=gl_VertexIndex%4;
uvec4 sum=uvec4(0);
sum+=uvec4(texelFetch(textures[0],ivec2(1,0),sid)*255.0+vec4(0.5))*1u;
sum+=uvec4(texelFetch(textures[1],ivec2(1,0),sid)*255.0+vec4(0.5))*2u;
sum+=uvec4(texelFetch(textures[2],ivec2(1,0),sid)*255.0+vec4(0.5))*3u;
sum+=uvec4(texelFetch(textures[3],ivec2(1,0),sid)*255.0+vec4(0.5))*4u;
sum+=uvec4(texelFetch(textures[4],ivec2(1,0),sid)*255.0+vec4(0.5))*5u;
sum+=uvec4(texelFetch(textures[5],ivec2(1,0),sid)*255.0+vec4(0.5))*6u;
sum+=uvec4(texelFetch(textures[6],ivec2(1,0),sid)*255.0+vec4(0.5))*7u;
sum+=uvec4(texelFetch(textures[7],ivec2(1,0),sid)*255.0+vec4(0.5))*8u;
sum+=uvec4(texelFetch(textures[8],ivec2(1,0),sid)*255.0+vec4(0.5))*9u;
sum+=uvec4(texelFetch(textures[9],ivec2(1,0),sid)*255.0+vec4(0.5))*10u;
sum+=uvec4(texelFetch(textures[10],ivec2(1,0),sid)*255.0+vec4(0.5))*11u;
sum+=uvec4(texelFetch(textures[11],ivec2(1,0),sid)*255.0+vec4(0.5))*12u;
sum+=uvec4(texelFetch(textures[12],ivec2(1,0),sid)*255.0+vec4(0.5))*13u;
sum+=uvec4(texelFetch(textures[13],ivec2(1,0),sid)*255.0+vec4(0.5))*14u;
sum+=uvec4(texelFetch(textures[14],ivec2(1,0),sid)*255.0+vec4(0.5))*15u;
sum+=uvec4(texelFetch(textures[15],ivec2(1,0),sid)*255.0+vec4(0.5))*16u;
bool good=all(equal(sum,uvec4(23120,19720,9520,34680))) && all(equal(textureSize(textures[0]),ivec2(2))) && textureSamples(textures[0])==4;
vec2 pos[3]=vec2[3](vec2(-1,-1),vec2(3,-1),vec2(-1,3));
gl_Position=good?vec4(pos[gl_VertexIndex],0.5,1):vec4(2,2,0.5,1);
}
