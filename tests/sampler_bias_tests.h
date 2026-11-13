/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Colored mips distinguish sampler bias from shader LOD and combined bias. */
static void sampler_bias_tests(VkRenderPass pass,VkFramebuffer fb,VkDescriptorSetLayout set_layout,
                               VkDescriptorSet set,struct image output,struct buffer result)
{
    struct image source=image_levels(8,8,VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT,3,1,0);
    const float colors[3][4]={{1,0,0,1},{0,1,0,1},{0,0,1,1}};
    begin();
    dimension_barrier(source,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,3);
    for(unsigned mip=0;mip<3;mip++) {
        VkClearColorValue value;memcpy(value.float32,colors[mip],sizeof value.float32);
        VkImageSubresourceRange range={VK_IMAGE_ASPECT_COLOR_BIT,mip,1,0,1};
        vkCmdClearColorImage(command,source.handle,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&value,1,&range);
    }
    dimension_barrier(source,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,1,3);
    submit();
    VkPushConstantRange push={VK_SHADER_STAGE_FRAGMENT_BIT,0,4};
    VkPipelineLayoutCreateInfo li={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount=1,.pSetLayouts=&set_layout,.pushConstantRangeCount=1,.pPushConstantRanges=&push};
    VkPipelineLayout layout;VK(vkCreatePipelineLayout(device,&li,NULL,&layout));
    VkPipeline render=pipeline_shaders(builtin_vert,sizeof builtin_vert,NULL,pass,layout,
                                       sampler_bias_frag,sizeof sampler_bias_frag,0,0,0);
    VkSamplerCreateInfo sci={.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter=VK_FILTER_NEAREST,.minFilter=VK_FILTER_NEAREST,.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU=VK_SAMPLER_ADDRESS_MODE_REPEAT,.addressModeV=VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeW=VK_SAMPLER_ADDRESS_MODE_REPEAT,.maxLod=2};
    const float biases[5]={2,-1,2,16,-16};
    const unsigned selected[5]={2,1,2,0,2};
    for(uint32_t mode=0;mode<5;mode++) {
        sci.mipLodBias=biases[mode];VkSampler sampler;VK(vkCreateSampler(device,&sci,NULL,&sampler));
        begin();vkCmdPushConstants(command,layout,VK_SHADER_STAGE_FRAGMENT_BIT,0,4,&mode);
        sample_format_image(source.view,pass,fb,render,layout,set,sampler,output,result);
        int good=1;const float *values=result.map;
        for(unsigned y=12;y<20;y++)for(unsigned x=12;x<20;x++)for(unsigned c=0;c<4;c++)
            if(!(fabsf(values[(y*32+x)*4+c]-colors[selected[mode]][c])<=0.000001f))good=0;
        EXPECT(good,mode<3?"GPU explicit LOD and gradients retain positive and negative sampler bias":
                          "GPU clamps the combined shader and sampler bias before adding base LOD");
        vkDestroySampler(device,sampler,NULL);
    }
    vkDestroyPipeline(device,render,NULL);vkDestroyPipelineLayout(device,layout,NULL);
    if(properties.limits.maxPerStageDescriptorStorageImages) {
        const VkDescriptorType types[2]={VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
        struct compute_case compute=compute_case(sampler_bias_comp,sizeof sampler_bias_comp,2,types);
        struct buffer values=buffer(64,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        VkDescriptorBufferInfo buffer_info={values.handle,0,32};
        for(unsigned test=0;test<2;test++) {
            sci.mipLodBias=test?-1:2;VkSampler sampler;VK(vkCreateSampler(device,&sci,NULL,&sampler));
            VkDescriptorImageInfo image_info={sampler,source.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            VkWriteDescriptorSet writes[2]={
                {.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=compute.set,.dstBinding=0,
                 .descriptorCount=1,.descriptorType=types[0],.pImageInfo=&image_info},
                {.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=compute.set,.dstBinding=1,
                 .descriptorCount=1,.descriptorType=types[1],.pBufferInfo=&buffer_info}};
            vkUpdateDescriptorSets(device,2,writes,0,NULL);memset(values.map,0xa5,64);
            begin();bind_compute_case(compute);vkCmdDispatch(command,1,1,1);
            compute_visibility(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,
                               VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_HOST_READ_BIT);submit();
            int good=1;const float *actual=values.map;
            for(unsigned vector=0;vector<2;vector++)for(unsigned c=0;c<4;c++)
                if(actual[vector*4+c]!=colors[test?(vector?0:1):2][c])good=0;
            for(unsigned i=32;i<64;i++)if(((uint8_t *)values.map)[i]!=0xa5)good=0;
            EXPECT(good,"Metal compute applies sampler bias to explicit LOD and gradients without changing output guards");
            vkDestroySampler(device,sampler,NULL);
        }
        free_compute_case(compute);free_buffer(values);
    }
    free_image(source);
}
