#version 450
layout(set=0,binding=0) uniform samplerCube source;
layout(location=0) out vec4 color;
void main(){float length=gl_FragCoord.x; color=vec4(textureQueryLod(source,vec3(length,length*.25,length*.5)),0,1);}
