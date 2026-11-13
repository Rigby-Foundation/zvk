/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Logical SSA aggregates use one uint4 per scalar/vector constituent. Memory
 * strides remain independent and are applied only by loads/stores. */
static unsigned value_registers(struct cc *c, uint32_t type, unsigned depth)
{
    if (depth > 24)
        cfail(c, "recursive value type");
    if (components(c, type))
        return 1;
    const uint32_t *p = node(c, type)->p;
    uint64_t count = 0;
    if (cop(p) == SpvOpTypeMatrix)
        count = (uint64_t)p[3] * value_registers(c, p[2], depth + 1);
    else if (cop(p) == SpvOpTypeArray)
        count = (uint64_t)literal(c, p[3]) * value_registers(c, p[2], depth + 1);
    else if (cop(p) == SpvOpTypeStruct)
        for (unsigned i = 2; i < cnw(p); i++)
            count += value_registers(c, p[i], depth + 1);
    if (count > 1024)
        cfail(c, "aggregate value too large");
    return (unsigned)count;
}
static void value_copy(struct cc *c, uint32_t dst, uint32_t src, uint32_t type)
{
    if (components(c, type))
        emit(c, "v%u=v%u;\n", dst, src);
    else
        for (unsigned i = 0; i < value_registers(c, type, 0); i++)
            emit(c, "v%u[%u]=v%u[%u];\n", dst, i, src, i);
}
static void value_constant(struct cc *c, uint32_t dst, uint32_t src, unsigned offset,
                           int aggregate, unsigned depth)
{
    if (depth > 24)
        cfail(c, "recursive constant value");
    struct cn *n = node(c, src);
    unsigned width = components(c, n->type);
    if (width) {

        for (unsigned k = 0; k < width; k++) {
            uint32_t word = constant_word(c,src,k,0);
            if (aggregate)
                emit(c, "v%u[%u][%u]=%uu;\n", dst, offset, k, word);
            else
                emit(c, "v%u[%u]=%uu;\n", dst, k, word);
        }
        if (!aggregate && width == 1)
            emit(c, "v%u=uint4(v%u.x);\n", dst, dst);
        return;
    }
    unsigned regs=value_registers(c,n->type,0);
    for (unsigned i=0;i<regs;i++)
        for (unsigned k=0;k<4;k++)
            emit(c,"v%u[%u][%u]=%uu;\n",dst,offset+i,k,aggregate_word(c,src,i,k,0));

}
static uint32_t value_part(struct cc *c, uint32_t type, const uint32_t *indices, unsigned count,
                           unsigned *offset, unsigned *lane)
{
    *offset = *lane = 0;
    for (unsigned i = 0; i < count; i++) {
        const uint32_t *p = node(c, type)->p;
        unsigned index = indices[i];
        if (cop(p) == SpvOpTypeStruct) {
            if (index >= cnw(p) - 2)
                cfail(c, "struct extraction index");
            for (unsigned j = 0; j < index; j++)
                *offset += value_registers(c, p[2 + j], 0);
            type = p[2 + index];
        } else if (cop(p) == SpvOpTypeMatrix || cop(p) == SpvOpTypeArray) {
            unsigned length = cop(p) == SpvOpTypeMatrix ? p[3] : literal(c, p[3]);
            if (index >= length)
                cfail(c, "aggregate extraction index");
            *offset += index * value_registers(c, p[2], 0);
            type = p[2];
        } else if (cop(p) == SpvOpTypeVector) {
            if (index >= p[3] || i + 1 != count)
                cfail(c, "vector extraction index");
            *lane = index;
            type = p[2];
        } else
            cfail(c, "composite extraction type");
    }
    return type;
}
