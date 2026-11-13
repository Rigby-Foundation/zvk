/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Specialization folding only: shader instructions execute on the GPU. */
static inline uint32_t zvk_quantize_f16_bits(uint32_t value)
{
    uint32_t sign=value&0x80000000u, magnitude=value&0x7fffffffu;
    if (magnitude>=0x7f800000u)return value;
    magnitude=(magnitude+0xfffu+((magnitude>>13u)&1u))&0xffffe000u;
    if (magnitude<0x38800000u)magnitude=0;
    if (magnitude>=0x47800000u)magnitude=0x7f800000u;
    return sign|magnitude;
}
