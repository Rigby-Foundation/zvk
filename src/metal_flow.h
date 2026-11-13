/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Structured MSL preserves fragment quad and compute workgroup reconvergence.
 * Compute modules without execution barriers may use the dispatcher. */
struct mf_block {const uint32_t *start,*term;unsigned label,merge,continuation;int loop;};
struct mf_loop {unsigned header,merge,continuation;const struct mf_loop *outer;};
struct mf_stop {unsigned label;const struct mf_stop *outer;};
struct mf_graph {
    struct cc *c;struct mf_block *blocks;unsigned *map;
    unsigned function,count,serial,result;const struct mf_stop *stops;
};
static struct mf_block *mf_block(struct mf_graph *g,unsigned label)
{
    struct cn *n=node(g->c,label);unsigned dense=zvk_spirv_id(&g->c->map,label);
    if(cop(n->p)!=SpvOpLabel || n->function!=g->function || g->map[dense]>=g->count)
        cfail(g->c,"graphics branch leaves its function");
    return g->blocks+g->map[dense];
}
static void mf_region(struct mf_graph *g,unsigned label,unsigned stop,const struct mf_loop *loop,int continuation,unsigned nesting);
static void mf_phi(struct mf_graph *g,unsigned source,unsigned target)
{
    struct cc *c=g->c;struct mf_block *block=mf_block(g,target);
    emit(c,"{\n");
    for(const uint32_t *p=block->start;p<block->term;p+=cnw(p)) {
        if(cop(p)!=SpvOpPhi)continue;
        if(cnw(p)<5 || (cnw(p)-3)%2)cfail(c,"graphics phi operand count");
        unsigned incoming=0;
        for(unsigned i=3;i<cnw(p);i+=2) {
            mf_block(g,p[i+1]);if(p[i+1]==source)incoming=p[i];
        }
        if(!incoming || node(c,incoming)->type!=p[1])cfail(c,"graphics phi incoming value");
        if(opaque(c,p[1]))emit(c,"uint2 phi%u=h%u;\n",p[2],incoming);
        else {
            unsigned regs=value_registers(c,p[1],0);
            if(!regs)cfail(c,"graphics pointer phi requires variable pointers");
            if(components(c,p[1]))emit(c,"uint4 phi%u=v%u;\n",p[2],incoming);
            else {
                emit(c,"uint4 phi%u[%u];\n",p[2],regs);
                for(unsigned i=0;i<regs;i++)emit(c,"phi%u[%u]=v%u[%u];\n",p[2],i,incoming,i);
            }
        }
    }
    for(const uint32_t *p=block->start;p<block->term;p+=cnw(p))if(cop(p)==SpvOpPhi) {
        if(opaque(c,p[1]))emit(c,"h%u=phi%u;\n",p[2],p[2]);
        else if(components(c,p[1]))emit(c,"v%u=phi%u;\n",p[2],p[2]);
        else for(unsigned i=0;i<value_registers(c,p[1],0);i++)emit(c,"v%u[%u]=phi%u[%u];\n",p[2],i,p[2],i);
    }
    emit(c,"}\n");
}
static int mf_transfer(struct mf_graph *g,unsigned target,unsigned stop,const struct mf_loop *loop,int continuation,unsigned nesting)
{
    if(loop) {
        if(target==loop->header){emit(g->c,"continue;\n");return 1;}
        if(target==loop->merge){emit(g->c,"break;\n");return 1;}
        if(target==loop->continuation&&!continuation) {
            mf_region(g,target,loop->header,loop,1,nesting+1);emit(g->c,"continue;\n");return 1;
        }
        for(const struct mf_loop *outer=loop->outer;outer;outer=outer->outer)
            if(target==outer->header || target==outer->merge || target==outer->continuation)
                cfail(g->c,"graphics branch exits multiple loop constructs");
    }
    if(target==stop)return 1;
    for(const struct mf_stop *s=g->stops;s;s=s->outer)
        if(s->label && target==s->label){emit(g->c,"escape%u=%uu;\n",g->serial,target);return 1;}
    return 0;
}
static void mf_edge(struct mf_graph *g,unsigned from,unsigned to,unsigned stop,const struct mf_loop *loop,int continuation,unsigned nesting)
{
    mf_phi(g,from,to);
    if(!mf_transfer(g,to,stop,loop,continuation,nesting))mf_region(g,to,stop,loop,continuation,nesting+1);
}
static void mf_instructions(struct mf_graph *g,const struct mf_block *b)
{
    for(const uint32_t *p=b->start;p<b->term;p+=cnw(p)) {
        if(cop(p)==SpvOpPhi)continue;
        instruction(g->c,p,b->label);
        if(cop(p)==SpvOpFunctionCall)
            emit(g->c,"if(discarded){returned%u=true;break;}\n",g->serial);
    }
}
static void mf_terminator(struct mf_graph *g,const struct mf_block *b,unsigned stop,const struct mf_loop *loop,int continuation,unsigned nesting)
{
    struct cc *c=g->c;const uint32_t *p=b->term;unsigned op=cop(p);
    if(op==SpvOpBranch)mf_edge(g,b->label,p[1],stop,loop,continuation,nesting);
    else if(op==SpvOpBranchConditional) {
        emit(c,"if(v%u.x){\n",p[1]);mf_edge(g,b->label,p[2],stop,loop,continuation,nesting);
        emit(c,"}else{\n");mf_edge(g,b->label,p[3],stop,loop,continuation,nesting);emit(c,"}\n");
    } else if(op==SpvOpSwitch) {
        if(cnw(p)<3 || (cnw(p)-3)%2)cfail(c,"graphics switch operand count");
        emit(c,"{bool matched=false;\n");
        for(unsigned i=3;i<cnw(p);i+=2) {
            if(p[i+1]==p[2])continue;
            int seen=0;for(unsigned j=3;j<i;j+=2)if(p[j+1]==p[i+1])seen=1;
            if(seen)continue;
            emit(c,"if(v%u.x==%uu",p[1],p[i]);
            for(unsigned j=i+2;j<cnw(p);j+=2)if(p[j+1]==p[i+1])emit(c,"||v%u.x==%uu",p[1],p[j]);
            emit(c,"){matched=true;\n");mf_edge(g,b->label,p[i+1],stop,loop,continuation,nesting);emit(c,"}\n");
        }
        emit(c,"if(!matched){\n");mf_edge(g,b->label,p[2],stop,loop,continuation,nesting);emit(c,"}}\n");
    } else if(op==SpvOpReturn || op==SpvOpReturnValue || op==SpvOpKill || op==SpvOpUnreachable) {
        if(op==SpvOpReturnValue) {
            if(!g->result || node(c,p[1])->type!=node(c,g->function)->p[1])cfail(c,"graphics function return type");
            if(opaque(c,node(c,p[1])->type))emit(c,"h%u=h%u;\n",g->result,p[1]);
            else value_copy(c,g->result,p[1],node(c,p[1])->type);
        }
        if(op==SpvOpKill) {
            if(c->stage!=VK_SHADER_STAGE_FRAGMENT_BIT)cfail(c,"graphics discard stage");
            emit(c,"discarded=true;discard_fragment();\n");
        }
        emit(c,"returned%u=true;break;\n",g->serial);
    } else cfail(c,"graphics block terminator");
}
static void mf_region(struct mf_graph *g,unsigned label,unsigned stop,const struct mf_loop *loop,int continuation,unsigned nesting)
{
    if(nesting>256)cfail(g->c,"graphics structured nesting limit");
    struct mf_stop scope={stop,g->stops};g->stops=&scope;
    unsigned steps=0,guards=0;
    while(label && label!=stop) {
        if(++steps>g->count)cfail(g->c,"unstructured graphics control-flow cycle");
        struct mf_block *b=mf_block(g,label);
        if(b->loop) {
            struct mf_loop inner={b->label,b->merge,b->continuation,loop};
            emit(g->c,"while(true){\n");mf_instructions(g,b);
            mf_terminator(g,b,b->merge,&inner,0,nesting+1);emit(g->c,"}\nif(returned%u)break;\n",g->serial);
            label=b->merge;continue;
        }
        mf_instructions(g,b);
        if(cop(b->term)==SpvOpBranch) {
            unsigned target=b->term[1];mf_phi(g,label,target);
            if(mf_transfer(g,target,stop,loop,continuation,nesting))goto done;
            label=target;continue;
        }
        unsigned merge=b->merge;
        mf_terminator(g,b,merge?merge:stop,loop,continuation,nesting);
        if(!merge)goto done;
        emit(g->c,"if(escape%u==%uu)escape%u=0;\nif(!returned%u && !escape%u){\n",g->serial,merge,g->serial,g->serial,g->serial);
        guards++;label=merge;
    }
done:
    while(guards--)emit(g->c,"}\n");g->stops=scope.outer;
}
static void metal_function_emit(struct cc *c,unsigned function,unsigned result)
{
    if(c->metal_depth>=32 || node(c,function)->visiting)cfail(c,"recursive graphics call");
    node(c,function)->visiting=1;
    unsigned old_function=c->current_function,depth=c->metal_depth++;
    c->current_function=function;
    const uint32_t *start=function_body(c,function),*end=start;
    unsigned blocks=0;
    while(end<c->code+c->words && cop(end)!=SpvOpFunctionEnd){if(cop(end)==SpvOpLabel)blocks++;end+=cnw(end);}
    if(!blocks || end>=c->code+c->words)cfail(c,"graphics function missing blocks");
    size_t bytes=blocks*sizeof(struct mf_block)+c->map.count*sizeof(unsigned);
    void *memory=calloc(1,bytes);if(!memory)cfail(c,"out of memory for graphics flow");
    c->flow_alloc[depth]=memory;
    struct mf_graph g={.c=c,.blocks=memory,.map=(void *)((struct mf_block *)memory+blocks),
        .function=function,.count=blocks,.serial=c->flow_serial++,.result=result};
    memset(g.map,255,c->map.count*sizeof(unsigned));
    struct mf_block *b=NULL;unsigned index=0;
    for(const uint32_t *p=start;p<end;p+=cnw(p)) {
        unsigned op=cop(p);
        if(op==SpvOpLabel){b=g.blocks+index;g.map[zvk_spirv_id(&c->map,p[1])]=index++;b->label=p[1];b->start=p+cnw(p);}
        else if(!b)cfail(c,"graphics instruction precedes first block");
        else if(op==SpvOpLoopMerge || op==SpvOpSelectionMerge){b->merge=p[1];b->loop=op==SpvOpLoopMerge;b->continuation=b->loop?p[2]:0;}
        else if(op==SpvOpBranch || op==SpvOpBranchConditional || op==SpvOpSwitch || op==SpvOpReturn ||
                op==SpvOpReturnValue || op==SpvOpKill || op==SpvOpUnreachable)b->term=p;
    }
    for(unsigned i=0;i<blocks;i++)if(!g.blocks[i].term)cfail(c,"graphics block missing terminator");
    emit(c,"{bool returned%u=false;uint escape%u=0;do{\n",g.serial,g.serial);
    mf_region(&g,g.blocks[0].label,0,NULL,0,0);emit(c,"break;}while(false);}\n");
    free(memory);c->flow_alloc[depth]=NULL;c->metal_depth--;c->current_function=old_function;node(c,function)->visiting=0;
}
