/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "zvk_internal.h"
#include <spirv/unified1/spirv.h>

VkResult zvk_spirv_groups(const uint32_t *code,size_t words,uint32_t **expanded,size_t *count)
{
    *expanded=NULL;*count=words;
    uint64_t extra=0;
    for (size_t at=5;at<words;at+=code[at]>>16) {
        const uint32_t *p=code+at;
        unsigned op=p[0]&65535,n=p[0]>>16;
        if (op!=SpvOpGroupDecorate && op!=SpvOpGroupMemberDecorate)continue;
        if (n<3 || (op==SpvOpGroupMemberDecorate && (n-2)%2))goto invalid;
        unsigned targets=op==SpvOpGroupDecorate?n-2:(n-2)/2;
        int found=0;
        for (size_t q=5;q<words;q+=code[q]>>16)
            if ((code[q]&65535)==SpvOpDecorationGroup && code[q+1]==p[1])found=1;
        if (!found)goto invalid;
        for (size_t q=5;q<words;q+=code[q]>>16)
            if ((code[q]&65535)==SpvOpDecorate && code[q+1]==p[1])
                extra+=(uint64_t)targets*((code[q]>>16)+(op==SpvOpGroupMemberDecorate));
        if (extra>16*1024*1024)goto invalid;
    }
    if (!extra)return VK_SUCCESS;
    if (words>SIZE_MAX/4-extra)return VK_ERROR_OUT_OF_HOST_MEMORY;
    uint32_t *out=malloc((words+extra)*4);
    if (!out)return VK_ERROR_OUT_OF_HOST_MEMORY;
    size_t used=0;
    memcpy(out,code,20);used=5;
    for (size_t at=5;at<words;at+=code[at]>>16) {
        const uint32_t *p=code+at;
        unsigned op=p[0]&65535,n=p[0]>>16;
        memcpy(out+used,p,n*4);used+=n;
        if (op!=SpvOpGroupDecorate && op!=SpvOpGroupMemberDecorate)continue;
        for (unsigned target=2;target<n;target+=op==SpvOpGroupDecorate?1:2)
            for (size_t q=5;q<words;q+=code[q]>>16) {
                const uint32_t *d=code+q;unsigned dn=d[0]>>16;
                if ((d[0]&65535)!=SpvOpDecorate || d[1]!=p[1])continue;
                int member=op==SpvOpGroupMemberDecorate;
                out[used++]=((dn+member)<<16)|(member?SpvOpMemberDecorate:SpvOpDecorate);
                out[used++]=p[target];
                if (member)out[used++]=p[target+1];
                memcpy(out+used,d+2,(dn-2)*4);used+=dn-2;
            }
    }
    *expanded=out;*count=used;
    return VK_SUCCESS;
invalid:
    zvk_error("SPIR-V: malformed or excessively expanded decoration group");
    return VK_ERROR_INVALID_SHADER_NV;
}
