/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Evaluate specialization expressions only; shader execution remains on GPU. */
static uint32_t aggregate_word(struct cc *c,uint32_t id,unsigned reg,unsigned lane,unsigned depth);
static uint32_t constant_word(struct cc *c, uint32_t id, unsigned lane, unsigned depth)
{
    if (depth>64) cfail(c,"recursive specialization expression");
    struct cn *n=node(c,id);
    const uint32_t *p=n->p;
    unsigned width=components(c,n->type);
    if (!width || lane>=width) cfail(c,"specialization constituent shape");
    if (n->known) return n->literal;
    if (cop(p)==SpvOpConstantComposite || cop(p)==SpvOpSpecConstantComposite) {
        if (cnw(p)!=3+width) cfail(c,"constant vector shape");
        return constant_word(c,p[3+lane],0,depth+1);
    }
    if (cop(p)==SpvOpUndef)return 0;
    if (cop(p)!=SpvOpSpecConstantOp || cnw(p)<5)
        cfail(c,"id %u is not a constant expression",id);
    unsigned op=p[3];
    if (op==SpvOpCompositeExtract || op==SpvOpCompositeInsert)
        return aggregate_word(c,id,0,lane,depth+1);
    if (op==SpvOpVectorShuffle) {
        if (cnw(p)!=6+width) cfail(c,"specialization shuffle shape");
        unsigned k=p[6+lane], a=components(c,node(c,p[4])->type);
        if (k==UINT32_MAX) return 0;
        return constant_word(c,k<a?p[4]:p[5],k<a?k:k-a,depth+1);
    }
    uint32_t a=constant_word(c,p[4],components(c,node(c,p[4])->type)==1?0:lane,depth+1);
    if (op==SpvOpSelect) {
        if (cnw(p)!=7) cfail(c,"specialization select operands");
        return constant_word(c,p[a?5:6],lane,depth+1);
    }
    if (op==SpvOpNot || op==SpvOpLogicalNot) return ~a;
    if (op==SpvOpSNegate) return 0u-a;
    if (op==SpvOpQuantizeToF16) return zvk_quantize_f16_bits(a);
    if (op==SpvOpUConvert || op==SpvOpSConvert || op==SpvOpBitcast) return a;
    if (cnw(p)!=6) cfail(c,"specialization binary operands");
    uint32_t b=constant_word(c,p[5],components(c,node(c,p[5])->type)==1?0:lane,depth+1);
    int32_t sa=(int32_t)a,sb=(int32_t)b;
    switch (op) {
    case SpvOpIAdd:return a+b;
    case SpvOpISub:return a-b;
    case SpvOpIMul:return a*b;
    case SpvOpUDiv:return b?a/b:0;
    case SpvOpUMod:return b?a%b:0;
    case SpvOpSDiv:return !b || (a==0x80000000u && b==UINT32_MAX)?0:(uint32_t)(sa/sb);
    case SpvOpSRem:
    case SpvOpSMod: {
        if (!b || (a==0x80000000u && b==UINT32_MAX)) return 0;
        uint32_t v=(uint32_t)(sa%sb);
        return op==SpvOpSMod && v && ((v^b)&0x80000000u)?v+b:v;
    }
    case SpvOpShiftLeftLogical:return b<32?a<<b:0;
    case SpvOpShiftRightLogical:return b<32?a>>b:0;
    case SpvOpShiftRightArithmetic:return b<32?(uint32_t)(sa>>b):0;
    case SpvOpBitwiseAnd:
    case SpvOpLogicalAnd:return a&b;
    case SpvOpBitwiseOr:
    case SpvOpLogicalOr:return a|b;
    case SpvOpBitwiseXor:
    case SpvOpLogicalNotEqual:return a^b;
    case SpvOpIEqual:
    case SpvOpLogicalEqual:return a==b?UINT32_MAX:0;
    case SpvOpINotEqual:return a!=b?UINT32_MAX:0;
    case SpvOpULessThan:return a<b?UINT32_MAX:0;
    case SpvOpUGreaterThan:return a>b?UINT32_MAX:0;
    case SpvOpULessThanEqual:return a<=b?UINT32_MAX:0;
    case SpvOpUGreaterThanEqual:return a>=b?UINT32_MAX:0;
    case SpvOpSLessThan:return sa<sb?UINT32_MAX:0;
    case SpvOpSGreaterThan:return sa>sb?UINT32_MAX:0;
    case SpvOpSLessThanEqual:return sa<=sb?UINT32_MAX:0;
    case SpvOpSGreaterThanEqual:return sa>=sb?UINT32_MAX:0;
    default:cfail(c,"unsupported specialization operation %u",op);
    }
    return 0;
}

static uint32_t aggregate_word(struct cc *c,uint32_t id,unsigned reg,unsigned lane,unsigned depth)
{
    if (depth>64)cfail(c,"recursive aggregate specialization");
    struct cn *n=node(c,id);
    const uint32_t *p=n->p,*t=node(c,n->type)->p;
    unsigned width=components(c,n->type);
    if (n->known || cop(p)==SpvOpUndef)return n->literal;
    if (cop(p)==SpvOpSpecConstantOp && (p[3]==SpvOpCompositeExtract || p[3]==SpvOpCompositeInsert)) {
        int insert=p[3]==SpvOpCompositeInsert;
        if (cnw(p)<(insert?7u:6u))cfail(c,"specialization composite operands");
        unsigned offset,selected_lane;
        uint32_t selected=value_part(c,node(c,p[insert?5:4])->type,p+(insert?6:5),cnw(p)-(insert?6:5),&offset,&selected_lane);
        unsigned sw=components(c,selected);
        if (!insert)return aggregate_word(c,p[4],offset+reg,sw==1?selected_lane:lane,depth+1);
        if (sw==1 && reg==offset && lane==selected_lane)return aggregate_word(c,p[4],0,0,depth+1);
        if (sw>1 && reg==offset)return aggregate_word(c,p[4],0,lane,depth+1);
        if (!sw && reg>=offset && reg-offset<value_registers(c,selected,0))
            return aggregate_word(c,p[4],reg-offset,lane,depth+1);
        return aggregate_word(c,p[5],reg,lane,depth+1);
    }
    if (width) {
        if (reg)cfail(c,"constant value register index");
        return lane<width?constant_word(c,id,lane,depth+1):0;
    }
    if (cop(p)!=SpvOpConstantComposite && cop(p)!=SpvOpSpecConstantComposite)
        cfail(c,"aggregate constant instruction");
    unsigned parts=cop(t)==SpvOpTypeStruct?cnw(t)-2:cop(t)==SpvOpTypeMatrix?t[3]:
                   cop(t)==SpvOpTypeArray?literal(c,t[3]):0;
    if (cnw(p)!=parts+3)cfail(c,"aggregate constant constituents");
    for (unsigned i=0;i<parts;i++) {
        uint32_t child=cop(t)==SpvOpTypeStruct?t[2+i]:t[2];
        unsigned size=value_registers(c,child,0);
        if (node(c,p[3+i])->type!=child)cfail(c,"constant constituent type");
        if (reg<size)return aggregate_word(c,p[3+i],reg,lane,depth+1);
        reg-=size;
    }
    cfail(c,"aggregate constant register index");
    return 0;
}
