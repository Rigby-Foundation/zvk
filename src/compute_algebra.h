/* SPDX-License-Identifier: LGPL-2.1-or-later */
static void matrix_shape(struct cc *c, uint32_t type, unsigned *cols, unsigned *rows)
{
    const uint32_t *t = node(c, type)->p;
    if (cop(t) != SpvOpTypeMatrix || t[3] < 2 || t[3] > 4 ||
        cop(node(c, t[2])->p) != SpvOpTypeVector)
        cfail(c, "matrix operand shape");
    *cols = t[3];
    *rows = components(c, t[2]);
}
static void determinant_expr(struct cc *c, uint32_t value, unsigned cols, unsigned rows)
{
    unsigned first = 0;
    while (!(rows & (1u << first)))
        first++;
    if (!(rows & (rows - 1))) {
        unsigned col = 0;
        while (!(cols & (1u << col)))
            col++;
        emit(c, "as_type<float>(v%u[%u][%u])", value, col, first);
        return;
    }
    emit(c, "(");
    unsigned term = 0;
    for (unsigned col = 0; col < 4; col++)
        if (cols & (1u << col)) {
            emit(c, "%sas_type<float>(v%u[%u][%u])*", !term ? "" : term & 1 ? "-" : "+", value, col, first);
            determinant_expr(c, value, cols & ~(1u << col), rows & ~(1u << first));
            term++;
        }
    emit(c, ")");
}
static void algebra(struct cc *c, const uint32_t *p)
{
    unsigned op = cop(p), id = p[2], a = p[3], b = cnw(p) > 4 ? p[4] : 0;
    unsigned ac = 1, ar = components(c, node(c, a)->type), bc = 1, br = 0;
    if (!ar)
        matrix_shape(c, node(c, a)->type, &ac, &ar);
    if (b) {
        br = components(c, node(c, b)->type);
        if (!br)
            matrix_shape(c, node(c, b)->type, &bc, &br);
    }
    if (op == SpvOpTranspose) {
        unsigned dc, dr;
        matrix_shape(c, node(c, id)->type, &dc, &dr);
        if (dc != ar || dr != ac)
            cfail(c, "transpose result shape");
        for (unsigned col = 0; col < ac; col++)
            for (unsigned row = 0; row < ar; row++)
                emit(c, "v%u[%u][%u]=v%u[%u][%u];\n", id, row, col, a, col, row);
    } else if (op == SpvOpMatrixTimesScalar) {
        if (br != 1 || node(c, id)->type != node(c, a)->type)
            cfail(c, "matrix scale shape");
        for (unsigned col = 0; col < ac; col++)
            emit(c, "v%u[%u]=as_type<uint4>(as_type<float4>(v%u[%u])*as_type<float>(v%u.x));\n", id, col, a, col, b);
    } else if (op == SpvOpOuterProduct) {
        unsigned dc, dr;
        matrix_shape(c, node(c, id)->type, &dc, &dr);
        if (dc != br || dr != ar)
            cfail(c, "outer product result shape");
        for (unsigned col = 0; col < dc; col++)
            emit(c, "v%u[%u]=as_type<uint4>(as_type<float4>(v%u)*as_type<float>(v%u[%u]));\n", id,col,a,b,col);
    } else if (op == SpvOpDot) {
        if (ar != br || components(c, node(c, id)->type) != 1)
            cfail(c, "dot product shape");
        emit(c, "v%u=uint4(as_type<uint>(", id);
        for (unsigned k = 0; k < ar; k++)
            emit(c, "%sas_type<float>(v%u[%u])*as_type<float>(v%u[%u])", k ? "+" : "", a,k,b,k);
        emit(c, "));\n");
    } else {
        unsigned rows = op == SpvOpVectorTimesMatrix ? bc : ar;
        unsigned cols = op == SpvOpMatrixTimesMatrix ? bc : 1;
        unsigned count = op == SpvOpVectorTimesMatrix ? ar : ac;
        if ((op == SpvOpVectorTimesMatrix && ar != br) ||
            (op != SpvOpVectorTimesMatrix && ac != br))
            cfail(c, "matrix multiplication operand shape");
        if (op == SpvOpMatrixTimesMatrix) {
            unsigned dc, dr;
            matrix_shape(c, node(c, id)->type, &dc, &dr);
            if (dc != cols || dr != rows)
                cfail(c, "matrix multiplication result shape");
        } else if (components(c, node(c, id)->type) != rows)
            cfail(c, "matrix/vector multiplication result shape");
        for (unsigned col = 0; col < cols; col++)
            for (unsigned row = 0; row < rows; row++) {
                if (cols > 1)
                    emit(c, "v%u[%u][%u]=as_type<uint>(", id, col, row);
                else
                    emit(c, "v%u[%u]=as_type<uint>(", id, row);
                for (unsigned k = 0; k < count; k++) {
                    emit(c, "%s", k ? "+" : "");
                    if (op == SpvOpVectorTimesMatrix)
                        emit(c, "as_type<float>(v%u[%u])*as_type<float>(v%u[%u][%u])", a,k,b,row,k);
                    else if (op == SpvOpMatrixTimesVector)
                        emit(c, "as_type<float>(v%u[%u][%u])*as_type<float>(v%u[%u])", a,k,row,b,k);
                    else
                        emit(c, "as_type<float>(v%u[%u][%u])*as_type<float>(v%u[%u][%u])", a,k,row,b,col,k);
                }
                emit(c, ");\n");
            }
    }
}
