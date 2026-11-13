/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "zvk_internal.h"
#include <stdbool.h>
#define SPV_ENABLE_UTILITY_CODE
#define inline static inline
#include <spirv/unified1/spirv.h>
#undef inline

static int id_order(const void *a,const void *b)
{
    uint32_t x=*(const uint32_t *)a,y=*(const uint32_t *)b;
    return (x>y)-(x<y);
}
VkResult zvk_spirv_ids(const uint32_t *code,size_t words,zvk_id_map *map)
{
    memset(map,0,sizeof *map);
    uint32_t count=0;
    for (size_t at=5;at<words;at+=code[at]>>16) {
        bool result,type;
        SpvHasResultAndType((SpvOp)(code[at]&65535),&result,&type);
        count+=result;
    }
    map->ids=malloc((size_t)count*4);
    if (!map->ids && count)return VK_ERROR_OUT_OF_HOST_MEMORY;
    for (size_t at=5;at<words;at+=code[at]>>16) {
        bool result,type;
        SpvHasResultAndType((SpvOp)(code[at]&65535),&result,&type);
        if (result) {
            uint32_t id=code[at+(type?2:1)];
            if (!id || id>=code[3])goto invalid;
            map->ids[map->count++]=id;
        }
    }
    qsort(map->ids,map->count,4,id_order);
    for (unsigned i=1;i<map->count;i++)
        if (map->ids[i]==map->ids[i-1])goto invalid;
    return VK_SUCCESS;
invalid:
    free(map->ids);memset(map,0,sizeof *map);
    zvk_error("SPIR-V: duplicate or out-of-range result ID");
    return VK_ERROR_INVALID_SHADER_NV;
}
uint32_t zvk_spirv_id(const zvk_id_map *map,uint32_t id)
{
    uint32_t first=0,last=map->count;
    while(first<last) {
        uint32_t middle=first+(last-first)/2,value=map->ids[middle];
        if (value==id)return middle;
        if (value<id)first=middle+1;else last=middle;
    }
    return UINT32_MAX;
}
