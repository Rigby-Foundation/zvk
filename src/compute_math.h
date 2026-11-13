/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* GLSL.std.450 scalar/vector lowering to native Metal GPU operations. */
static void extended(struct cc *c, const uint32_t *p)
{
    const uint32_t *import = node(c, p[3])->p;
    if (cop(import) != SpvOpExtInstImport || !memchr(import + 2, 0, (cnw(import) - 2) * 4) ||
        strcmp((const char *)(import + 2), "GLSL.std.450"))
        cfail(c, "unsupported extended instruction set");
    unsigned op = p[4], result = p[2], n = components(c, node(c, result)->type);
    if (op == GLSLstd450Sinh || op == GLSLstd450Cosh) {
        if (!n || cnw(p)!=6 || components(c,node(c,p[5])->type)!=n)
            cfail(c,"hyperbolic operand shape");
        for (unsigned k=0;k<n;k++)
            emit(c,"{float a=as_type<float>(v%u[%u]);v%u[%u]=as_type<uint>((exp(a)%sexp(-a))*0.5f);}\n",
                 p[5],k,result,k,op == GLSLstd450Sinh ? "-" : "+");
        if (n==1) emit(c,"v%u=uint4(v%u.x);\n",result,result);
        return;
    }
    if (op == GLSLstd450Radians || op == GLSLstd450Degrees) {
        if (!n || cnw(p) != 6 || components(c,node(c,p[5])->type) != n)
            cfail(c,"angle conversion shape");
        for (unsigned k=0;k<n;k++)
            emit(c,"v%u[%u]=as_type<uint>(as_type<float>(v%u[%u])*%s);\n",result,k,p[5],k,
                 op == GLSLstd450Radians ? "0x1.1df46ap-6f" : "0x1.ca5dc2p+5f");
        if (n==1)
            emit(c,"v%u=uint4(v%u.x);\n",result,result);
        return;
    }
    if (op == GLSLstd450Modf || op == GLSLstd450ModfStruct ||
        op == GLSLstd450Frexp || op == GLSLstd450FrexpStruct) {
        int structure = op == GLSLstd450ModfStruct || op == GLSLstd450FrexpStruct;
        int exponent = op == GLSLstd450Frexp || op == GLSLstd450FrexpStruct;
        unsigned width = components(c, node(c, p[5])->type);
        if (!width || cnw(p) != (structure ? 6u : 7u))
            cfail(c, "decomposition operand shape");
        uint32_t output = structure ? 0 : p[6];
        if (structure) {
            const uint32_t *t = node(c, node(c, result)->type)->p;
            if (cop(t) != SpvOpTypeStruct || cnw(t) != 4 ||
                components(c, t[2]) != width || components(c, t[3]) != width)
                cfail(c, "decomposition result shape");
        } else {
            node(c, output);
            if (n != width || components(c, node(c, output)->pointee) != width)
                cfail(c, "decomposition pointer shape");
        }
        for (unsigned k = 0; k < width; k++) {
            emit(c, "{%s whole;float fraction=%s(as_type<float>(v%u[%u]),whole);",
                 exponent ? "int" : "float", exponent ? "frexp" : "modf", p[5],k);
            if (structure)
                emit(c, "v%u[0][%u]=as_type<uint>(fraction);v%u[1][%u]=as_type<uint>(whole);", result,k,result,k);
            else {
                char name[32], limit[32];
                address(c, output, name, sizeof name, limit, sizeof limit);
                emit(c, "v%u[%u]=as_type<uint>(fraction);if(o%u<=%s && %uu<=%s-o%u)%s[o%u/4u+%uu]=as_type<uint>(whole);",
                     result,k,output,limit,4*(k+1),limit,output,name,output,k);
            }
            emit(c, "}\n");
        }
        if (!structure && width == 1)
            emit(c, "v%u=uint4(v%u.x);\n",result,result);
        return;
    }
    if (op == GLSLstd450Determinant || op == GLSLstd450MatrixInverse) {
        unsigned cols, rows;
        if (cnw(p) != 6)
            cfail(c, "matrix intrinsic operand count");
        matrix_shape(c, node(c, p[5])->type, &cols, &rows);
        if (cols != rows)
            cfail(c, "matrix intrinsic requires square matrix");
        unsigned mask = (1u << cols) - 1;
        if (op == GLSLstd450Determinant) {
            if (n != 1)
                cfail(c, "determinant result type");
            emit(c, "v%u=uint4(as_type<uint>(",result);
            determinant_expr(c,p[5],mask,mask);
            emit(c, "));\n");
        } else {
            if (node(c,result)->type != node(c,p[5])->type)
                cfail(c,"inverse result type");
            emit(c, "{float determinant=");
            determinant_expr(c,p[5],mask,mask);
            emit(c,";\n");
            for (unsigned col=0;col<cols;col++)
                for (unsigned row=0;row<rows;row++) {
                    emit(c,"v%u[%u][%u]=as_type<uint>(%s",result,col,row,(col+row)&1?"-":"");
                    determinant_expr(c,p[5],mask&~(1u<<row),mask&~(1u<<col));
                    emit(c,"/determinant);\n");
                }
            emit(c,"}\n");
        }
        return;
    }
    const char *type = "float", *fn = NULL;
    unsigned args = 1;
    switch (op) {
    case GLSLstd450Round: fn = "round"; break;
    case GLSLstd450RoundEven: fn = "rint"; break;
    case GLSLstd450Trunc: fn = "trunc"; break;
    case GLSLstd450FAbs: fn = "fabs"; break;
    case GLSLstd450SAbs: fn = "abs"; type = "int"; break;
    case GLSLstd450FSign: fn = "sign"; break;
    case GLSLstd450Floor: fn = "floor"; break;
    case GLSLstd450Ceil: fn = "ceil"; break;
    case GLSLstd450Fract: fn = "fract"; break;
    case GLSLstd450Radians: fn = "radians"; break;
    case GLSLstd450Degrees: fn = "degrees"; break;
    case GLSLstd450Sin: fn = "sin"; break;
    case GLSLstd450Cos: fn = "cos"; break;
    case GLSLstd450Tan: fn = "tan"; break;
    case GLSLstd450Asin: fn = "asin"; break;
    case GLSLstd450Acos: fn = "acos"; break;
    case GLSLstd450Atan: fn = "atan"; break;
    case GLSLstd450Sinh: fn = "sinh"; break;
    case GLSLstd450Cosh: fn = "cosh"; break;
    case GLSLstd450Tanh: fn = "tanh"; break;
    case GLSLstd450Asinh: fn = "asinh"; break;
    case GLSLstd450Acosh: fn = "acosh"; break;
    case GLSLstd450Atanh: fn = "atanh"; break;
    case GLSLstd450Atan2: fn = "atan2"; args = 2; break;
    case GLSLstd450Pow: fn = "pow"; args = 2; break;
    case GLSLstd450Exp: fn = "exp"; break;
    case GLSLstd450Log: fn = "log"; break;
    case GLSLstd450Exp2: fn = "exp2"; break;
    case GLSLstd450Log2: fn = "log2"; break;
    case GLSLstd450Sqrt: fn = "sqrt"; break;
    case GLSLstd450InverseSqrt: fn = "rsqrt"; break;
    case GLSLstd450UMin: type = "uint"; fn = "min"; args = 2; break;
    case GLSLstd450UMax: type = "uint"; fn = "max"; args = 2; break;
    case GLSLstd450SMin: type = "int"; fn = "min"; args = 2; break;
    case GLSLstd450SMax: type = "int"; fn = "max"; args = 2; break;
    case GLSLstd450FMin:
    case GLSLstd450NMin: fn = "fmin"; args = 2; break;
    case GLSLstd450FMax:
    case GLSLstd450NMax: fn = "fmax"; args = 2; break;
    case GLSLstd450UClamp: type = "uint"; args = 3; break;
    case GLSLstd450SClamp: type = "int"; args = 3; break;
    case GLSLstd450FClamp:
    case GLSLstd450NClamp: args = 3; break;
    case GLSLstd450FMix: fn = "mix"; args = 3; break;
    case GLSLstd450Step: fn = "step"; args = 2; break;
    case GLSLstd450SmoothStep: fn = "smoothstep"; args = 3; break;
    case GLSLstd450Fma: fn = "fma"; args = 3; break;
    case GLSLstd450Ldexp: fn = "ldexp"; args = 2; break;
    default: break;
    }
    if (fn || op == GLSLstd450UClamp || op == GLSLstd450SClamp ||
        op == GLSLstd450FClamp || op == GLSLstd450NClamp) {
        if (!n || cnw(p) != 5 + args)
            cfail(c, "extended instruction operand count");
        for (unsigned a = 0; a < args; a++) {
            unsigned width = components(c, node(c, p[5 + a])->type);
            if (width != n && width != 1)
                cfail(c, "extended instruction operand shape");
        }
        for (unsigned k = 0; k < n; k++) {
            emit(c, "v%u[%u]=as_type<uint>(", result, k);
            if (!fn) {
                const char *prefix = !strcmp(type, "float") ? "f" : "";
                unsigned a = components(c, node(c, p[5])->type) == 1 ? 0 : k;
                unsigned b = components(c, node(c, p[6])->type) == 1 ? 0 : k;
                unsigned d = components(c, node(c, p[7])->type) == 1 ? 0 : k;
                emit(c, "%smin(%smax(as_type<%s>(v%u[%u]),as_type<%s>(v%u[%u])),as_type<%s>(v%u[%u]))",
                     prefix, prefix, type, p[5], a, type, p[6], b, type, p[7], d);
            } else {
                emit(c, "%s(", fn);
                for (unsigned a = 0; a < args; a++)
                    emit(c, "%sas_type<%s>(v%u[%u])", a ? "," : "",
                         op == GLSLstd450Ldexp && a == 1 ? "int" : type, p[5 + a],
                         components(c, node(c, p[5 + a])->type) == 1 ? 0 : k);
                emit(c, ")");
            }
            emit(c, ");\n");
        }
        if (n == 1)
            emit(c, "v%u=uint4(v%u.x);\n", result, result);
        return;
    }
    if (op == GLSLstd450SSign || op == GLSLstd450FindILsb ||
        op == GLSLstd450FindSMsb || op == GLSLstd450FindUMsb) {
        if (!n || cnw(p) != 6 || components(c, node(c, p[5])->type) != n)
            cfail(c, "integer intrinsic shape");
        for (unsigned k = 0; k < n; k++) {
            if (op == GLSLstd450SSign)
                emit(c, "v%u[%u]=uint(as_type<int>(v%u[%u])>0?1:as_type<int>(v%u[%u])<0?-1:0);\n",
                     result, k, p[5], k, p[5], k);
            else {
                emit(c, "{uint a=v%u[%u];", p[5], k);
                if (op == GLSLstd450FindSMsb)
                    emit(c, "if(as_type<int>(a)<0)a=~a;");
                emit(c, "v%u[%u]=a?%s:0xffffffffu;}\n", result, k,
                     op == GLSLstd450FindILsb ? "ctz(a)" : "31u-clz(a)");
            }
        }
        if (n == 1)
            emit(c, "v%u=uint4(v%u.x);\n", result, result);
        return;
    }
    if (op >= GLSLstd450Length && op <= GLSLstd450Refract) {
        unsigned width = components(c, node(c, p[5])->type);
        if (!width)
            cfail(c, "geometric intrinsic operand");
        const char *mask = width == 1 ? "x" : width == 2 ? "xy" : width == 3 ? "xyz" : "xyzw";
        char a[80], b[80], d[80];
        if (width == 1)
            snprintf(a, sizeof a, "as_type<float>(v%u.x)", p[5]);
        else
            snprintf(a, sizeof a, "as_type<float%u>(v%u.%s)", width, p[5], mask);
        args = op == GLSLstd450Length || op == GLSLstd450Normalize ? 1
                   : op == GLSLstd450FaceForward || op == GLSLstd450Refract ? 3 : 2;
        if (cnw(p) != 5 + args)
            cfail(c, "geometric intrinsic operand count");
        if (args > 1) {
            if (components(c, node(c, p[6])->type) != width)
                cfail(c, "geometric second operand shape");
            if (width == 1)
                snprintf(b, sizeof b, "as_type<float>(v%u.x)", p[6]);
            else
                snprintf(b, sizeof b, "as_type<float%u>(v%u.%s)", width, p[6], mask);
        }
        if (args > 2) {
            unsigned third = op == GLSLstd450Refract ? 1 : width;
            if (components(c, node(c, p[7])->type) != third)
                cfail(c, "geometric third operand shape");
            if (third == 1)
                snprintf(d, sizeof d, "as_type<float>(v%u.x)", p[7]);
            else
                snprintf(d, sizeof d, "as_type<float%u>(v%u.%s)", width, p[7], mask);
        }
        emit(c, "v%u=uint4(0);", result);
        if (op == GLSLstd450Length || op == GLSLstd450Distance) {
            emit(c, "v%u=as_type<uint4>(float4(%s(%s", result, width == 1 ? "fabs" : "length", a);
            if (op == GLSLstd450Distance) emit(c, "-%s", b);
            emit(c, ")));\n");
            return;
        }
        emit(c, "v%u.%s=as_type<%s>(", result, mask,
             width == 1 ? "uint" : width == 2 ? "uint2" : width == 3 ? "uint3" : "uint4");
        if (op == GLSLstd450Normalize) {
            if (width==1)
                emit(c,"sign(%s)",a);
            else
                emit(c,"(%s*rsqrt(dot(%s,%s)))",a,a,a);
        }
        else if (op == GLSLstd450Cross) {
            if (width != 3) cfail(c, "cross product width");
            emit(c, "cross(%s,%s)", a, b);
        } else if (op == GLSLstd450FaceForward) {
            if (width == 1)
                emit(c, "(%s*%s<0.0f?%s:-%s)", d, b, a, a);
            else
                emit(c, "(dot(%s,%s)<0.0f?%s:-%s)", d, b, a, a);
        } else if (op == GLSLstd450Reflect) {
            if (width == 1)
                emit(c, "(%s-2.0f*(%s*%s)*%s)", a, b, a, b);
            else
                emit(c, "reflect(%s,%s)", a, b);
        } else if (width == 1)
            emit(c, "refract(float2(%s,0),float2(%s,0),%s).x", a, b, d);
        else
            emit(c, "refract(%s,%s,%s)", a, b, d);
        emit(c, ");\n");
        if (width == 1)
            emit(c, "v%u=uint4(v%u.x);\n", result, result);
        return;
    }
    if (op == GLSLstd450PackHalf2x16 || op == GLSLstd450UnpackHalf2x16) {
        unsigned input = op == GLSLstd450PackHalf2x16 ? 2 : 1;
        if (cnw(p) != 6 || components(c, node(c, p[5])->type) != input ||
            n != (input == 2 ? 1u : 2u))
            cfail(c, "half packing shape");
        if (input == 2)
            emit(c, "v%u=uint4(as_type<uint>(half2(as_type<float2>(v%u.xy))));\n", result, p[5]);
        else
            emit(c, "v%u=as_type<uint4>(float4(float2(as_type<half2>(v%u.x)),0.0f,0.0f));\n", result, p[5]);
        return;
    }
    if (op == GLSLstd450PackSnorm4x8 || op == GLSLstd450PackUnorm4x8 ||
        op == GLSLstd450PackSnorm2x16 || op == GLSLstd450PackUnorm2x16) {
        unsigned count = op == GLSLstd450PackSnorm4x8 || op == GLSLstd450PackUnorm4x8 ? 4 : 2;
        unsigned bits = count == 4 ? 8 : 16;
        int snorm = op == GLSLstd450PackSnorm4x8 || op == GLSLstd450PackSnorm2x16;
        unsigned maximum = snorm ? (1u << (bits - 1)) - 1 : (1u << bits) - 1;
        if (cnw(p) != 6 || n != 1 || components(c, node(c, p[5])->type) != count)
            cfail(c, "normalized packing shape");
        emit(c, "{uint packed=0;\n");
        for (unsigned k = 0; k < count; k++)
            emit(c, "packed|=(uint(int(rint(clamp(as_type<float>(v%u[%u]),%s,1.0f)*%u.0f)))&%uu)<<%uu;\n",
                 p[5], k, snorm ? "-1.0f" : "0.0f", maximum, (1u << bits) - 1, k * bits);
        emit(c, "v%u=uint4(packed);}\n", result);
        return;
    }
    if (op == GLSLstd450UnpackSnorm2x16 || op == GLSLstd450UnpackUnorm2x16 ||
        op == GLSLstd450UnpackSnorm4x8 || op == GLSLstd450UnpackUnorm4x8) {
        unsigned count = op == GLSLstd450UnpackSnorm4x8 || op == GLSLstd450UnpackUnorm4x8 ? 4 : 2;
        unsigned bits = count == 4 ? 8 : 16;
        int snorm = op == GLSLstd450UnpackSnorm2x16 || op == GLSLstd450UnpackSnorm4x8;
        unsigned maximum = snorm ? (1u << (bits - 1)) - 1 : (1u << bits) - 1;
        if (cnw(p) != 6 || n != count || components(c, node(c, p[5])->type) != 1)
            cfail(c, "normalized unpacking shape");
        for (unsigned k = 0; k < count; k++) {
            emit(c, "{uint bits=(v%u.x>>%uu)&%uu;", p[5], k * bits, (1u << bits) - 1);
            if (snorm)
                emit(c, "int signed_bits=as_type<int>(bits<<%uu)>>%uu;v%u[%u]=as_type<uint>(fmax(float(signed_bits)/%u.0f,-1.0f));}\n",
                     32 - bits, 32 - bits, result, k, maximum);
            else
                emit(c, "v%u[%u]=as_type<uint>(float(bits)/%u.0f);}\n", result, k, maximum);
        }
        return;
    }
    cfail(c, "unsupported GLSL extended instruction %u", op);
}
