#version 450
layout(location=0) out vec4 color;
float calls(int seed) {
    float result=0;
    for (int i=0;i<4;i++) {
        switch ((seed+i)&3) {
        case 0: result+=1; continue;
        case 1: result+=1; // Fall through to the nested loop.
        case 2:
            for (int j=0;j<3;j++) {
                if (j==1) continue;
                result+=0.5;
            }
            break;
        default: return result+3;
        }
    }
    return result;
}
void main() {
    float a[10];
    for (int i=0;i<10;i++)
        for (int j=0;j<1;j++)
            for (int k=0;k<1;k++) a[i]=2;
    int x=int(gl_FragCoord.x);
    float value=gl_FragCoord.x;
    if ((x&1)==0) {
        for (int i=0;i<4;i++) value+=float(i);
    } else value+=6;
    float derivative=dFdx(value);
    const float expected[4]=float[4](7,6,4,3);
    bool good=a[x%10]==2 && calls(x&3)==expected[x&3] &&
              abs(derivative-1)<0.001 && abs(dFdy(gl_FragCoord.y)-1)<0.001;
    color=good?vec4(0.25,0.5,0.75,1):vec4(1,0,0,1);
}
