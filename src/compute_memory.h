/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Keep logical aggregate registers independent from decorated memory layouts. */
static void memory_value(struct cc *c, uint32_t type, uint32_t ptr, uint32_t value,
                         unsigned reg, int aggregate, uint32_t offset, unsigned stride,
                         unsigned component_stride, int row_major, const char *name,
                         const char *limit, int store, unsigned depth)
{
    if (depth > 24)
        cfail(c, "recursive memory value");
    const uint32_t *t = node(c, type)->p;
    unsigned width = components(c, type);
    if (width) {
        for (unsigned k = 0; k < width; k++) {
            uint64_t add = (uint64_t)offset + k * (component_stride ? component_stride : 4);
            if (add > UINT32_MAX - 4)
                cfail(c, "memory value offset overflow");
            char lane[64];
            if (aggregate)
                snprintf(lane, sizeof lane, "v%u[%u][%u]", value, reg, k);
            else
                snprintf(lane, sizeof lane, "v%u[%u]", value, k);
            emit(c, "if(o%u<=%s && %uu<=%s-o%u) ", ptr, limit, (unsigned)add + 4, limit, ptr);
            if (store)
                emit(c, "%s[o%u/4u+%uu]=%s;\n", name, ptr, (unsigned)add / 4, lane);
            else
                emit(c, "%s=%s[o%u/4u+%uu];else %s=0;\n", lane, name, ptr, (unsigned)add / 4, lane);
        }
        return;
    }
    unsigned parts = cop(t) == SpvOpTypeStruct ? cnw(t) - 2
                     : cop(t) == SpvOpTypeMatrix ? t[3]
                     : cop(t) == SpvOpTypeArray ? literal(c, t[3]) : 0;
    if ((!parts && cop(t)!=SpvOpTypeStruct) || parts>1024)
        cfail(c, "memory aggregate shape");
    for (unsigned i = 0; i < parts; i++) {
        uint32_t child = cop(t) == SpvOpTypeStruct ? t[2 + i] : t[2];
        unsigned ms = stride, cs = component_stride, rm = row_major;
        uint64_t add = offset;
        if (cop(t) == SpvOpTypeStruct) {
            struct cn *n = node(c, type);
            add += member_offset(c, type, i, depth);
            ms = n->matrix_strides[i];
            cs = 4;
            rm = (n->row_major_mask >> i) & 1;
        } else if (cop(t) == SpvOpTypeMatrix) {
            if (!ms)
                ms = bytes(c, child, depth + 1);
            add += (uint64_t)i * (rm ? 4 : ms);
            cs = rm ? ms : 4;
        } else
            add += (uint64_t)i * (node(c, type)->stride ? (unsigned)node(c, type)->stride
                                                        : bytes(c, child, depth + 1));
        if (add > UINT32_MAX)
            cfail(c, "aggregate memory offset overflow");
        memory_value(c, child, ptr, value, reg, 1, add, ms, cs, rm, name, limit, store, depth + 1);
        reg += value_registers(c, child, 0);
    }
}
static void load_store(struct cc *c, const uint32_t *p, int store)
{
    uint32_t ptrid = p[store ? 1 : 3], value = p[2];
    struct cn *ptr = node(c, ptrid), *root = node(c, ptr->root), *v = node(c, value);
    if (v->type==ptr->pointee && cop(node(c,v->type)->p)==SpvOpTypeStruct && cnw(node(c,v->type)->p)==2)
        return;
    if (v->type != ptr->pointee || !value_registers(c, v->type, 0))
        cfail(c, "load/store value type");
    if (store && ((root->slot >= 0 &&
                   c->out->bindings[root->slot].type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) ||
                  storage(c, root) == SpvStorageClassPushConstant || storage(c, root) == SpvStorageClassInput))
        cfail(c, "write to read-only memory");
    if (storage(c, root) == SpvStorageClassPushConstant) {
        /* The complete declared block is a safe bound for dynamic aggregate accesses. */
        uint32_t size = bytes(c, root->pointee, 0);
        if (size > 128)
            cfail(c, "push constant range");
        if (size > c->out->push_bytes)
            c->out->push_bytes = size;
    }
    char name[32], limit[32];
    address(c, ptrid, name, sizeof name, limit, sizeof limit);
    memory_value(c, v->type, ptrid, value, 0, !components(c, v->type), 0,
                 ptr->matrix_stride, ptr->component_stride, ptr->row_major,
                 name, limit, store, 0);
    if (!store && components(c, v->type) == 1)
        emit(c, "v%u=uint4(v%u.x);\n", value, value);
}
static void copy_memory(struct cc *c,const uint32_t *p)
{
    struct cn *dst=node(c,p[1]),*src=node(c,p[2]);
    struct cn *root=node(c,dst->root);
    if (dst->pointee!=src->pointee)cfail(c,"memory copy pointee type");
    if (storage(c,root)==SpvStorageClassInput || storage(c,root)==SpvStorageClassPushConstant ||
        (root->slot>=0 && c->out->bindings[root->slot].type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER))
        cfail(c,"memory copy to read-only memory");
    unsigned regs=value_registers(c,src->pointee,0);
    if (!regs)return;
    uint32_t value=c->bound+(uint32_t)(p-c->code);
    int aggregate=!components(c,src->pointee);
    if (aggregate)emit(c,"{uint4 v%u[%u]={};\n",value,regs);
    else emit(c,"{uint4 v%u=uint4(0);\n",value);
    char name[32],limit[32];
    address(c,p[2],name,sizeof name,limit,sizeof limit);
    memory_value(c,src->pointee,p[2],value,0,aggregate,0,src->matrix_stride,
                  src->component_stride,src->row_major,name,limit,0,0);
    address(c,p[1],name,sizeof name,limit,sizeof limit);
    memory_value(c,dst->pointee,p[1],value,0,aggregate,0,dst->matrix_stride,
                  dst->component_stride,dst->row_major,name,limit,1,0);
    emit(c,"}\n");
}
