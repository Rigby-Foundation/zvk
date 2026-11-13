/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Preserve SPIR-V structured regions in native GPU control flow. Phi copies
 * occur on incoming edges with parallel temporaries; merge instructions
 * execute after reconvergence. Inlined function returns leave a one-iteration
 * wrapper, unwinding nested loops with an explicit return flag. */
struct flow_block {
    const uint32_t *start,*end,*term;
    uint32_t label,merge,continuation;
    int loop;
};
struct flow_loop {
    uint32_t header,merge,continuation;
    const struct flow_loop *outer;
};
struct flow_stop {
    uint32_t label;
    const struct flow_stop *outer;
};
struct flow_graph {
    struct compiler *c;
    struct flow_block *blocks;
    uint32_t *map,*phi_temp;
    uint32_t function;
    unsigned count;
    int returned,escape,result,call_depth;
    const struct flow_stop *stops;
};
static struct flow_block *flow_block(struct flow_graph *g,uint32_t label)
{
    struct id *n=get(g->c,label);
    uint32_t dense=zvk_spirv_id(&g->c->map,label);
    if (n->function!=g->function || opcode(n->ins)!=SpvOpLabel || g->map[dense]>=g->count)
        fail(g->c,"branch leaves its function");
    return g->blocks+g->map[dense];
}
static void flow_region(struct flow_graph *g,uint32_t label,uint32_t stop,
                         const struct flow_loop *loop,int continuation,unsigned nesting);
/* Lower phi nodes on incoming edges. All sources are saved before assigning
 * any destination, including mutually dependent loop-carried phi values. */
static void flow_phi_edge(struct flow_graph *g,uint32_t source,uint32_t target)
{
    struct compiler *c=g->c;
    struct flow_block *b=flow_block(g,target);
    const uint32_t *phis[MAX_MEMBERS];int scratch[MAX_MEMBERS],count=0;
    for (const uint32_t *p=b->start;p<b->end;p+=wc(p)) {
        if (opcode(p)!=SpvOpPhi)continue;
        if (count==MAX_MEMBERS || (wc(p)-3)%2)fail(c,"invalid phi block");
        struct id *r=get(c,p[2]);
        if (r->reg<0)fail(c,"pointer phi is not supported");
        uint32_t dense=zvk_spirv_id(&c->map,p[2]);
        if (!g->phi_temp[dense])g->phi_temp[dense]=temporary(c,registers(c,r->type));
        phis[count]=p;scratch[count]=g->phi_temp[dense];
        int found=0;
        for (int i=3;i<wc(p);i+=2) {
            flow_block(g,p[i+1]);
            if (p[i+1]!=source)continue;
            copy(c,scratch[count],get(c,p[i])->reg,r->type);found=1;break;
        }
        if (!found)fail(c,"phi has no incoming value for branch");
        count++;
    }
    for (int i=0;i<count;i++)copy(c,get(c,phis[i][2])->reg,scratch[i],get(c,phis[i][2])->type);
}
/* Return true when an edge has emitted a loop transfer or reached a merge. */
static int flow_transfer(struct flow_graph *g,uint32_t target,uint32_t stop,
                           const struct flow_loop *loop,int continuation,unsigned nesting)
{
    if (loop) {
        if (target==loop->header) {emit(g->c,"CONT");return 1;}
        if (target==loop->merge) {emit(g->c,"BRK");return 1;}
        if (target==loop->continuation && !continuation) {
            flow_region(g,target,loop->header,loop,1,nesting+1);
            emit(g->c,"CONT");return 1;
        }
        for (const struct flow_loop *outer=loop->outer;outer;outer=outer->outer)
            if (target==outer->header || target==outer->merge || target==outer->continuation)
                fail(g->c,"branch exits multiple loop constructs");
    }
    if (target==stop)return 1;
    for (const struct flow_stop *s=g->stops;s;s=s->outer)
        if (s->label && target==s->label) {
            emit(g->c,"MOV TEMP[%d].x, IMM[%d]",g->escape,scalar_imm(g->c,target));
            return 1;
        }
    return 0;
}
static void flow_edge(struct flow_graph *g,uint32_t source,uint32_t target,uint32_t stop,
                       const struct flow_loop *loop,int continuation,unsigned nesting)
{
    flow_phi_edge(g,source,target);
    if (!flow_transfer(g,target,stop,loop,continuation,nesting))
        flow_region(g,target,stop,loop,continuation,nesting+1);
}
static void flow_instructions(struct flow_graph *g,const struct flow_block *b)
{
    for (const uint32_t *p=b->start;p<b->term;p+=wc(p))
        instruction_emit(g->c,p,g->call_depth);
}
static void flow_terminator(struct flow_graph *g,const struct flow_block *b,uint32_t stop,
                              const struct flow_loop *loop,int continuation,unsigned nesting)
{
    struct compiler *c=g->c;const uint32_t *p=b->term;SpvOp op=opcode(p);
    if (op==SpvOpBranchConditional) {
        char condition[64];source(c,p[1],0,condition);
        emit(c,"UIF %s",condition);flow_edge(g,b->label,p[2],stop,loop,continuation,nesting);
        emit(c,"ELSE");flow_edge(g,b->label,p[3],stop,loop,continuation,nesting);emit(c,"ENDIF");
    } else if (op==SpvOpSwitch) {
        if ((wc(p)-3)%2)fail(c,"switch operand count");
        char selector[64];source(c,p[1],0,selector);
        int matched=temporary(c,1),condition=temporary(c,1),equal=temporary(c,1);
        emit(c,"MOV TEMP[%d].x, IMM[%d]",matched,scalar_imm(c,0));
        for (int i=3;i<wc(p);i+=2) {
            /* Several literals can share one case body. Emit that region
             * once, including its phi transfers and inlined calls. Literals
             * targeting the default are covered by the final unmatched arm. */
            if (p[i+1]==p[2])continue;
            int seen=0;
            for (int j=3;j<i;j+=2)if (p[j+1]==p[i+1])seen=1;
            if (seen)continue;
            emit(c,"USEQ TEMP[%d].x, %s, IMM[%d]",condition,selector,scalar_imm(c,p[i]));
            for (int j=i+2;j<wc(p);j+=2)if (p[j+1]==p[i+1]) {
                emit(c,"USEQ TEMP[%d].x, %s, IMM[%d]",equal,selector,scalar_imm(c,p[j]));
                emit(c,"OR TEMP[%d].x, TEMP[%d].xxxx, TEMP[%d].xxxx",condition,condition,equal);
            }
            emit(c,"UIF TEMP[%d].xxxx",condition);
            emit(c,"MOV TEMP[%d].x, IMM[%d]",matched,scalar_imm(c,1));
            flow_edge(g,b->label,p[i+1],stop,loop,continuation,nesting);emit(c,"ENDIF");
        }
        emit(c,"USEQ TEMP[%d].x, TEMP[%d].xxxx, IMM[%d]",condition,matched,scalar_imm(c,0));
        emit(c,"UIF TEMP[%d].xxxx",condition);flow_edge(g,b->label,p[2],stop,loop,continuation,nesting);emit(c,"ENDIF");
    } else if (op==SpvOpReturn || op==SpvOpReturnValue || op==SpvOpUnreachable || op==SpvOpKill) {
        if (op==SpvOpReturnValue) {
            if (g->result<0)fail(c,"unexpected return value");
            copy(c,g->result,get(c,p[1])->reg,get(c,p[1])->type);
        }
        if (op==SpvOpKill) {
            if (c->stage!=VK_SHADER_STAGE_FRAGMENT_BIT)fail(c,"kill outside fragment stage");
            if(c->shader->early_fragment_tests)fail(c,"early tests with discard are not supported on the GL path");
            emit(c,"KILL");
        }
        emit(c,"MOV TEMP[%d].x, IMM[%d]",g->returned,scalar_imm(c,1));emit(c,"BRK");
    } else if (op==SpvOpBranch)flow_edge(g,b->label,p[1],stop,loop,continuation,nesting);
    else fail(c,"invalid block terminator %s",SpvOpToString(op));
}
static void flow_region(struct flow_graph *g,uint32_t label,uint32_t stop,
                         const struct flow_loop *loop,int continuation,unsigned nesting)
{
    if (nesting>256)fail(g->c,"structured control-flow nesting limit");
    struct flow_stop scope={stop,g->stops};
    g->stops=&scope;
    unsigned steps=0,guards=0;
    while (label!=stop && label) {
        if (++steps>g->count)fail(g->c,"unstructured control-flow cycle");
        struct flow_block *b=flow_block(g,label);
        if (getenv("ZVK_DEBUG_FLOW"))
            fprintf(stderr,"zvk flow function=%u block=%u stop=%u temps=%d nesting=%u\n",
                    g->function,label,stop,g->c->temps,nesting);
        if (b->loop) {
            struct flow_loop inner={b->label,b->merge,b->continuation,loop};
            emit(g->c,"BGNLOOP");flow_instructions(g,b);
            flow_terminator(g,b,b->merge,&inner,0,nesting+1);emit(g->c,"ENDLOOP");
            emit(g->c,"UIF TEMP[%d].xxxx",g->returned);emit(g->c,"BRK");emit(g->c,"ENDIF");
            label=b->merge;continue;
        }
        flow_instructions(g,b);
        if (opcode(b->term)==SpvOpBranch) {
            uint32_t target=b->term[1];flow_phi_edge(g,label,target);
            if (flow_transfer(g,target,stop,loop,continuation,nesting))goto done;
            label=target;continue;
        }
        uint32_t merge=b->merge;
        flow_terminator(g,b,merge?merge:stop,loop,continuation,nesting);
        if (!merge)goto done;
        /* A branch can bypass this selection's merge and rejoin an outer
         * selection. Retain its target while skipping the remainder of this
         * region. The owning selection clears it at its reconvergence point. */
        int condition=temporary(g->c,1);
        emit(g->c,"USEQ TEMP[%d].x, TEMP[%d].xxxx, IMM[%d]",condition,g->escape,scalar_imm(g->c,merge));
        emit(g->c,"UIF TEMP[%d].xxxx",condition);
        emit(g->c,"MOV TEMP[%d].x, IMM[%d]",g->escape,scalar_imm(g->c,0));emit(g->c,"ENDIF");
        emit(g->c,"USEQ TEMP[%d].x, TEMP[%d].xxxx, IMM[%d]",condition,g->escape,scalar_imm(g->c,0));
        emit(g->c,"UIF TEMP[%d].xxxx",condition);guards++;
        label=merge;
    }
done:
    while (guards--)emit(g->c,"ENDIF");
    g->stops=scope.outer;
}
static void function_emit(struct compiler *c,uint32_t function,int return_reg,int depth)
{
    struct id *fn=get(c,function);
    if (depth>16 || fn->processing || opcode(fn->ins)!=SpvOpFunction)
        fail(c,"invalid or recursive function call");
    fn->processing=1;
    c->shader->structured_flow=1;
    const uint32_t *start=fn->ins+wc(fn->ins),*end=start;
    unsigned blocks=0;
    while (end<c->code+c->words && opcode(end)!=SpvOpFunctionEnd) {
        if (opcode(end)==SpvOpLabel)blocks++;
        end+=wc(end);
    }
    if (!blocks || end>=c->code+c->words)fail(c,"unterminated or empty function");
    size_t bytes=blocks*sizeof(struct flow_block)+2*c->map.count*sizeof(uint32_t);
    void *memory=calloc(1,bytes);
    if (!memory)fail(c,"out of memory for control flow");
    c->flow_alloc[depth]=memory;
    struct flow_graph graph={.c=c,.blocks=memory,.map=(void *)((struct flow_block *)memory+blocks),
        .function=function,.count=blocks,.returned=temporary(c,1),.escape=temporary(c,1),.result=return_reg,.call_depth=depth};
    graph.phi_temp=graph.map+c->map.count;
    memset(graph.map,255,c->map.count*sizeof(uint32_t));
    unsigned index=0;struct flow_block *b=NULL;
    for (const uint32_t *p=start;p<end;p+=wc(p)) {
        SpvOp op=opcode(p);
        if (op==SpvOpFunctionParameter || op==SpvOpLine || op==SpvOpNoLine)continue;
        if (op==SpvOpLabel) {
            if (b)b->end=p;
            b=graph.blocks+index;graph.map[zvk_spirv_id(&c->map,p[1])]=index++;
            b->label=p[1];b->start=p+wc(p);b->end=end;
        } else if (!b)fail(c,"instruction before first block");
        else if (op==SpvOpLoopMerge || op==SpvOpSelectionMerge) {
            b->merge=p[1];b->loop=op==SpvOpLoopMerge;b->continuation=b->loop?p[2]:0;
        } else if (op==SpvOpBranch || op==SpvOpBranchConditional || op==SpvOpSwitch ||
                   op==SpvOpReturn || op==SpvOpReturnValue || op==SpvOpKill || op==SpvOpUnreachable)
            b->term=p;
    }
    for (unsigned i=0;i<blocks;i++)if (!graph.blocks[i].term)fail(c,"unterminated block");
    emit(c,"MOV TEMP[%d].x, IMM[%d]",graph.returned,scalar_imm(c,0));
    emit(c,"MOV TEMP[%d].x, IMM[%d]",graph.escape,scalar_imm(c,0));
    emit(c,"BGNLOOP");flow_region(&graph,graph.blocks[0].label,0,NULL,0,0);emit(c,"BRK");emit(c,"ENDLOOP");
    fn->processing=0;free(memory);c->flow_alloc[depth]=NULL;
}
