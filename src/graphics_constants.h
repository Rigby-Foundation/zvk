/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Fold specialization expressions, including nested aggregate inserts/extracts.
 * This evaluates constants only; shader instructions remain native GPU work. */
static uint32_t graphics_constant_word(struct compiler *c,uint32_t id,unsigned reg,unsigned lane,unsigned depth);
static uint32_t graphics_constant_part(struct compiler *c,uint32_t type,const uint32_t *indices,
                                      unsigned count,unsigned *reg,unsigned *lane)
{
    if (!count || count>32)fail(c,"specialization composite index count");
    *reg=*lane=0;
    for (unsigned i=0;i<count;i++) {
        const uint32_t *t=get(c,type)->ins;unsigned index=indices[i];
        if (opcode(t)==SpvOpTypeVector) {
            if (index>=t[3] || i+1!=count)fail(c,"specialization vector index");
            *lane=index;type=t[2];
        } else if (opcode(t)==SpvOpTypeStruct) {
            *reg+=field_regs(c,type,index);type=t[2+index];
        } else if (opcode(t)==SpvOpTypeArray || opcode(t)==SpvOpTypeMatrix) {
            unsigned length=opcode(t)==SpvOpTypeArray?constant(c,t[3]):t[3];
            if (index>=length)fail(c,"specialization aggregate index");
            *reg+=index*registers(c,t[2]);type=t[2];
        } else fail(c,"index into scalar specialization");
    }
    return type;
}
static uint32_t graphics_constant_word(struct compiler *c,uint32_t id,unsigned reg,unsigned lane,unsigned depth)
{
    if (depth>64)fail(c,"recursive specialization expression");
    struct id *n=get(c,id);const uint32_t *p=n->ins,*t=get(c,n->type)->ins;
    SpvOp kind=opcode(p),type=opcode(t);
    if (kind==SpvOpUndef || kind==SpvOpConstantNull)return 0;
    if (n->literal_valid)return n->literal;
    if (kind==SpvOpConstantComposite || kind==SpvOpSpecConstantComposite) {
        if(type==SpvOpTypeVector) {
            if(wc(p)!=(int)(3+t[3]) || reg)fail(c,"specialization vector constituents");
            return lane<t[3]?graphics_constant_word(c,p[3+lane],0,0,depth+1):0;
        }
        unsigned count=type==SpvOpTypeStruct?wc(t)-2:type==SpvOpTypeArray?constant(c,t[3]):type==SpvOpTypeMatrix?t[3]:0;
        if(wc(p)!=(int)(3+count))fail(c,"specialization aggregate constituents");
        for(unsigned i=0;i<count;i++) {
            unsigned child=type==SpvOpTypeStruct?t[2+i]:t[2],size=registers(c,child);
            if(get(c,p[3+i])->type!=child)fail(c,"specialization constituent type");
            if(reg<size)return graphics_constant_word(c,p[3+i],reg,lane,depth+1);
            reg-=size;
        }
        fail(c,"specialization register index");
    }
    if(kind!=SpvOpSpecConstantOp)fail(c,"id %u is not a specialization expression",id);
    minimum(c,p,5);SpvOp op=(SpvOp)p[3];
    if(op==SpvOpCompositeExtract || op==SpvOpCompositeInsert) {
        int insert=op==SpvOpCompositeInsert;minimum(c,p,insert?7:6);
        unsigned offset,selected_lane;
        uint32_t selected=graphics_constant_part(c,get(c,p[insert?5:4])->type,p+(insert?6:5),wc(p)-(insert?6:5),&offset,&selected_lane);
        SpvOp selected_type=opcode(get(c,selected)->ins);
        int scalar=selected_type==SpvOpTypeBool || selected_type==SpvOpTypeInt || selected_type==SpvOpTypeFloat;
        if(!insert)return graphics_constant_word(c,p[4],offset+reg,scalar?selected_lane:lane,depth+1);
        if(scalar && reg==offset && lane==selected_lane)return graphics_constant_word(c,p[4],0,0,depth+1);
        if(!scalar && reg>=offset && reg-offset<(unsigned)registers(c,selected))
            return graphics_constant_word(c,p[4],reg-offset,lane,depth+1);
        return graphics_constant_word(c,p[5],reg,lane,depth+1);
    }
    if(type==SpvOpTypeVector && lane>=t[3])return 0;
    if(reg)fail(c,"specialization scalar/vector register");
    if(op==SpvOpVectorShuffle) {
        minimum(c,p,6+width(c,n->type));unsigned index=p[6+lane],left=width(c,get(c,p[4])->type);
        if(index==UINT32_MAX)return 0;
        unsigned source=index<left?p[4]:p[5],selected=index<left?index:index-left;
        if(selected>=(unsigned)width(c,get(c,source)->type))fail(c,"specialization shuffle index");
        return graphics_constant_word(c,source,0,selected,depth+1);
    }
    uint32_t a=graphics_constant_word(c,p[4],0,width(c,get(c,p[4])->type)==1?0:lane,depth+1),b=0,value=0;
    if (op == SpvOpSelect) {
        minimum(c, p, 7);
        value = graphics_constant_word(c,a?p[5]:p[6],0,lane,depth+1);
    } else if (op == SpvOpNot || op == SpvOpLogicalNot)
        value = ~a;
    else if (op == SpvOpSNegate)
        value = 0u - a;
    else if (op == SpvOpQuantizeToF16)
        value = zvk_quantize_f16_bits(a);
    else if (op == SpvOpBitcast || op == SpvOpUConvert || op == SpvOpSConvert)
        value = a;
    else {
        minimum(c, p, 6);
        b = graphics_constant_word(c,p[5],0,width(c,get(c,p[5])->type)==1?0:lane,depth+1);
        switch (op) {
        case SpvOpIAdd:
            value = a + b;
            break;
        case SpvOpISub:
            value = a - b;
            break;
        case SpvOpIMul:
            value = a * b;
            break;
        case SpvOpUDiv:
        case SpvOpUMod:
            value = !b ? 0 : op == SpvOpUDiv ? a / b : a % b;
            break;
        case SpvOpSDiv:
        case SpvOpSRem:
        case SpvOpSMod:
            if (!b || (a == 0x80000000u && b == UINT32_MAX)) {
                value=0;
                break;
            }
            value = op == SpvOpSDiv ? (uint32_t)((int32_t)a / (int32_t)b)
                                    : (uint32_t)((int32_t)a % (int32_t)b);
            if (op == SpvOpSMod && value && ((value ^ b) & 0x80000000u))
                value += b;
            break;
        case SpvOpShiftLeftLogical:
        case SpvOpShiftRightLogical:
        case SpvOpShiftRightArithmetic:
            value = b >= 32 ? 0 : op == SpvOpShiftLeftLogical    ? a << b
                    : op == SpvOpShiftRightLogical ? a >> b
                                                   : (uint32_t)((int32_t)a >> b);
            break;
        case SpvOpBitwiseAnd:
        case SpvOpLogicalAnd:
            value = a & b;
            break;
        case SpvOpBitwiseOr:
        case SpvOpLogicalOr:
            value = a | b;
            break;
        case SpvOpBitwiseXor:
        case SpvOpLogicalNotEqual:
            value = a ^ b;
            break;
        case SpvOpIEqual:
        case SpvOpLogicalEqual:
            value = a == b ? UINT32_MAX : 0;
            break;
        case SpvOpINotEqual:
            value = a != b ? UINT32_MAX : 0;
            break;
        case SpvOpULessThan:
            value = a < b ? UINT32_MAX : 0;
            break;
        case SpvOpUGreaterThan:
            value = a > b ? UINT32_MAX : 0;
            break;
        case SpvOpULessThanEqual:
            value = a <= b ? UINT32_MAX : 0;
            break;
        case SpvOpUGreaterThanEqual:
            value = a >= b ? UINT32_MAX : 0;
            break;
        case SpvOpSLessThan:
            value = (int32_t)a < (int32_t)b ? UINT32_MAX : 0;
            break;
        case SpvOpSGreaterThan:
            value = (int32_t)a > (int32_t)b ? UINT32_MAX : 0;
            break;
        case SpvOpSLessThanEqual:
            value = (int32_t)a <= (int32_t)b ? UINT32_MAX : 0;
            break;
        case SpvOpSGreaterThanEqual:
            value = (int32_t)a >= (int32_t)b ? UINT32_MAX : 0;
            break;
        default:
            fail(c, "unsupported specialization opcode %s", SpvOpToString(op));
        }
    }
    return value;
}
static void fold_specialization(struct compiler *c,const uint32_t *p)
{
    struct id *n=get(c,p[2]);SpvOp type=opcode(get(c,n->type)->ins);
    if(type==SpvOpTypeBool || type==SpvOpTypeInt || type==SpvOpTypeFloat) {
        n->literal=graphics_constant_word(c,p[2],0,0,0);n->literal_valid=1;
    }
}
