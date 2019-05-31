/*
 * Copyright (c) 2016, Alliance for Open Media. All rights reserved
 *
 * This source code is subject to the terms of the BSD 2 Clause License and
 * the Alliance for Open Media Patent License 1.0. If the BSD 2 Clause License
 * was not distributed with this source code in the LICENSE file, you can
 * obtain it at www.aomedia.org/license/software. If the Alliance for Open
 * Media Patent License 1.0 was not distributed with this source code in the
 * PATENTS file, you can obtain it at www.aomedia.org/license/patent.
 */

#include "EbDefinitions.h"
#include <immintrin.h>
#include "aom_dsp_rtcd.h"
#include "EbVariance_SSE2.h"

 // Various blending functions and macros.
 // See also the aom_blend_* functions in aom_dsp_rtcd.h

 // Alpha blending with alpha values from the range [0, 64], where 64
 // means use the first input and 0 means use the second input.

#define AOM_BLEND_A64_ROUND_BITS 6
#define AOM_BLEND_A64_MAX_ALPHA (1 << AOM_BLEND_A64_ROUND_BITS)  // 64

#define AOM_BLEND_A64(a, v0, v1)                                          \
  ROUND_POWER_OF_TWO((a) * (v0) + (AOM_BLEND_A64_MAX_ALPHA - (a)) * (v1), \
                     AOM_BLEND_A64_ROUND_BITS)

// Alpha blending with alpha values from the range [0, 256], where 256
// means use the first input and 0 means use the second input.
#define AOM_BLEND_A256_ROUND_BITS 8
#define AOM_BLEND_A256_MAX_ALPHA (1 << AOM_BLEND_A256_ROUND_BITS)  // 256

#define AOM_BLEND_A256(a, v0, v1)                                          \
  ROUND_POWER_OF_TWO((a) * (v0) + (AOM_BLEND_A256_MAX_ALPHA - (a)) * (v1), \
                     AOM_BLEND_A256_ROUND_BITS)

static INLINE __m128i mm256_add_hi_lo_epi16(const __m256i val) {
    return _mm_add_epi16(_mm256_castsi256_si128(val),
        _mm256_extractf128_si256(val, 1));
}

static INLINE __m128i mm256_add_hi_lo_epi32(const __m256i val) {
    return _mm_add_epi32(_mm256_castsi256_si128(val),
        _mm256_extractf128_si256(val, 1));
}

static INLINE void variance_kernel_no_sum_avx2(const __m256i src, const __m256i ref,
    __m256i *const sse) {
    const __m256i adj_sub = _mm256_set1_epi16((short)0xff01);  // (1,-1)

    // unpack into pairs of source and reference values
    const __m256i src_ref0 = _mm256_unpacklo_epi8(src, ref);
    const __m256i src_ref1 = _mm256_unpackhi_epi8(src, ref);

    // subtract adjacent elements using src*1 + ref*-1
    const __m256i diff0 = _mm256_maddubs_epi16(src_ref0, adj_sub);
    const __m256i diff1 = _mm256_maddubs_epi16(src_ref1, adj_sub);
    const __m256i madd0 = _mm256_madd_epi16(diff0, diff0);
    const __m256i madd1 = _mm256_madd_epi16(diff1, diff1);

    // add to the running totals
    *sse = _mm256_add_epi32(*sse, _mm256_add_epi32(madd0, madd1));
}

static INLINE void variance_final_from_32bit_no_sum_avx2(__m256i vsse,
    uint32_t *const sse) {
    // extract the low lane and add it to the high lane
    const __m128i sse_reg_128 = mm256_add_hi_lo_epi32(vsse);
    const __m128i zero = _mm_setzero_si128();

    // unpack sse and sum registers and add
    const __m128i sse_sum_lo = _mm_unpacklo_epi32(sse_reg_128, zero);
    const __m128i sse_sum_hi = _mm_unpackhi_epi32(sse_reg_128, zero);
    const __m128i sse_sum = _mm_add_epi32(sse_sum_lo, sse_sum_hi);

    // perform the final summation and extract the results
    const __m128i res = _mm_add_epi32(sse_sum, _mm_srli_si128(sse_sum, 8));
    *((int32_t *)sse) = _mm_cvtsi128_si32(res);
}

// handle pixels (<= 512)
static INLINE void variance_final_512_no_sum_avx2(__m256i vsse,
    uint32_t *const sse) {
    // extract the low lane and add it to the high lane
    variance_final_from_32bit_no_sum_avx2(vsse, sse);
}

// handle 1024 pixels (32x32, 16x64, 64x16)
static INLINE void variance_final_1024_no_sum_avx2(__m256i vsse,
    uint32_t *const sse) {
    // extract the low lane and add it to the high lane
    variance_final_from_32bit_no_sum_avx2(vsse, sse);
}

static INLINE __m256i sum_to_32bit_avx2(const __m256i sum) {
    const __m256i sum_lo = _mm256_cvtepi16_epi32(_mm256_castsi256_si128(sum));
    const __m256i sum_hi =
        _mm256_cvtepi16_epi32(_mm256_extractf128_si256(sum, 1));
    return _mm256_add_epi32(sum_lo, sum_hi);
}

// handle 2048 pixels (32x64, 64x32)
static INLINE void variance_final_2048_no_sum_avx2(__m256i vsse,
    uint32_t *const sse) {
    variance_final_from_32bit_no_sum_avx2(vsse, sse);
}

static INLINE void variance16_kernel_no_sum_avx2(
    const uint8_t *const src, const int32_t src_stride, const uint8_t *const ref,
    const int32_t ref_stride, __m256i *const sse) {
    const __m128i s0 = _mm_loadu_si128((__m128i const *)(src + 0 * src_stride));
    const __m128i s1 = _mm_loadu_si128((__m128i const *)(src + 1 * src_stride));
    const __m128i r0 = _mm_loadu_si128((__m128i const *)(ref + 0 * ref_stride));
    const __m128i r1 = _mm_loadu_si128((__m128i const *)(ref + 1 * ref_stride));
    const __m256i s = _mm256_inserti128_si256(_mm256_castsi128_si256(s0), s1, 1);
    const __m256i r = _mm256_inserti128_si256(_mm256_castsi128_si256(r0), r1, 1);
    variance_kernel_no_sum_avx2(s, r, sse);
}

static INLINE void variance32_kernel_no_sum_avx2(const uint8_t *const src,
    const uint8_t *const ref,
    __m256i *const sse) {
    const __m256i s = _mm256_loadu_si256((__m256i const *)(src));
    const __m256i r = _mm256_loadu_si256((__m256i const *)(ref));
    variance_kernel_no_sum_avx2(s, r, sse);
}

static INLINE void variance16_no_sum_avx2(const uint8_t *src,
    const int32_t src_stride, const uint8_t *ref, const int32_t ref_stride,
    const int32_t h, __m256i *const vsse) {
    for (int32_t i = 0; i < h; i += 2) {
        variance16_kernel_no_sum_avx2(src, src_stride, ref, ref_stride, vsse);
        src += 2 * src_stride;
        ref += 2 * ref_stride;
    }
}

static INLINE void variance32_no_sum_avx2(const uint8_t *src, const int32_t src_stride,
    const uint8_t *ref, const int32_t ref_stride,
    const int32_t h, __m256i *const vsse) {
    for (int32_t i = 0; i < h; i++) {
        variance32_kernel_no_sum_avx2(src, ref, vsse);
        src += src_stride;
        ref += ref_stride;
    }
}

static INLINE void variance64_no_sum_avx2(const uint8_t *src, const int32_t src_stride,
    const uint8_t *ref, const int32_t ref_stride,
    const int32_t h, __m256i *const vsse) {
    for (int32_t i = 0; i < h; i++) {
        variance32_kernel_no_sum_avx2(src + 0, ref + 0, vsse);
        variance32_kernel_no_sum_avx2(src + 32, ref + 32, vsse);
        src += src_stride;
        ref += ref_stride;
    }
}

static INLINE void variance128_no_sum_avx2(const uint8_t *src, const int32_t src_stride,
    const uint8_t *ref, const int32_t ref_stride,
    const int32_t h, __m256i *const vsse) {
    for (int32_t i = 0; i < h; i++) {
        variance32_kernel_no_sum_avx2(src + 0, ref + 0, vsse);
        variance32_kernel_no_sum_avx2(src + 32, ref + 32, vsse);
        variance32_kernel_no_sum_avx2(src + 64, ref + 64, vsse);
        variance32_kernel_no_sum_avx2(src + 96, ref + 96, vsse);
        src += src_stride;
        ref += ref_stride;
    }
}

#define AOM_VAR_NO_LOOP_NO_SUM_AVX2(bw, bh, bits, max_pixel)                         \
  void aom_variance##bw##x##bh##_no_sum_avx2(                                \
      const uint8_t *src, int32_t src_stride, const uint8_t *ref, int32_t ref_stride, \
      uint32_t *sse) {                                                    \
    __m256i vsse = _mm256_setzero_si256();                                    \
    variance##bw##_no_sum_avx2(src, src_stride, ref, ref_stride, bh, &vsse);  \
    variance_final_##max_pixel##_no_sum_avx2(vsse,  sse);       \
  }

AOM_VAR_NO_LOOP_NO_SUM_AVX2(16, 16, 8, 512);

uint32_t aom_mse16x16_avx2(const uint8_t *src, int32_t src_stride,
    const uint8_t *ref, int32_t ref_stride,
    uint32_t *sse) {
    aom_variance16x16_no_sum_avx2(src, src_stride, ref, ref_stride, sse);
    return *sse;
}

void highbd_variance64_avx2(const uint8_t *a8, int32_t a_stride,
    const uint8_t *b8, int32_t b_stride, int32_t w, int32_t h,
    uint64_t *sse) {
    const uint8_t *a = a8;
    const uint8_t *b = b8;

    if (w == 4) {
        __m128i vsse = _mm_setzero_si128();
        uint32_t tsse;
        variance4_no_sum_sse2(a8, a_stride, b8, b_stride, h, &vsse);
        variance_final_128_pel_no_sum_sse2(vsse, &tsse);
        *sse = tsse;
    }
    else if (w == 8) {
        __m128i vsse = _mm_setzero_si128();
        uint32_t tsse;
        variance8_no_sum_sse2(a8, a_stride, b8, b_stride, h, &vsse);
        variance_final_256_pel_no_sum_sse2(vsse, &tsse);
        *sse = tsse;
    }
    else if (w == 16) {
        __m256i vsse = _mm256_setzero_si256();
        uint32_t tsse;
        variance16_no_sum_avx2(a8, a_stride, b8, b_stride, h, &vsse);
        variance_final_1024_no_sum_avx2(vsse, &tsse);
        *sse = tsse;
    }
    else if (w == 32) {
        if (h <= 64) {
            __m256i vsse = _mm256_setzero_si256();
            uint32_t tsse;
            variance32_no_sum_avx2(a8, a_stride, b8, b_stride, h, &vsse);
            variance_final_2048_no_sum_avx2(vsse, &tsse);
            *sse = tsse;
        }
        else {
            __m256i vsse = _mm256_setzero_si256();
            uint32_t tsse;
            variance32_no_sum_avx2(a8, a_stride, b8, b_stride, 64, &vsse);
            variance32_no_sum_avx2(a8 + 64 * a_stride, a_stride, b8 + 64 * b_stride,
                b_stride, h - 64, &vsse);
            variance_final_from_32bit_no_sum_avx2(vsse, &tsse);
            *sse = tsse;
        }
    }
    else if (w == 64) {
        if (h <= 32) {
            __m256i vsse = _mm256_setzero_si256();
            uint32_t tsse;
            variance64_no_sum_avx2(a8, a_stride, b8, b_stride, h, &vsse);
            variance_final_2048_no_sum_avx2(vsse, &tsse);
            *sse = tsse;
        }
        else {
            __m256i vsse = _mm256_setzero_si256();
            uint32_t tsse;

            int32_t i = 0;
            do {
                variance64_no_sum_avx2(a8, a_stride, b8, b_stride, 32, &vsse);
                a8 += 32 * a_stride;
                b8 += 32 * b_stride;
            } while (++i < (h / 32));
            variance_final_from_32bit_no_sum_avx2(vsse, &tsse);
            *sse = tsse;
        }
    }
    else if (w == 128) {
        __m256i vsse = _mm256_setzero_si256();
        uint32_t tsse;

        int32_t i = 0;
        do {
            variance128_no_sum_avx2(a8, a_stride, b8, b_stride, 16, &vsse);
            a8 += 16 * a_stride;
            b8 += 16 * b_stride;
        } while (++i < (h / 16));
        variance_final_from_32bit_no_sum_avx2(vsse, &tsse);
        *sse = tsse;
    }
    else
        highbd_variance64_c(a, a_stride, b, b_stride, w, h, sse);
#ifdef _WIN32
    // Add this redundant instruction to fix a Visual Studio compiler bug, which
    // falsely loads 64-bit intermediate result into *sse in
    // variance_final_from_32bit_sum_avx2(), instead of 32-bit result as we
    // wanted. We and *sse back to 32-bit correct result.
    // No overflow happens here,  since for the largest 8-bit 128x128 block,
    // *sse is at most 255 * 255 * 128 * 128, i.e., 0x000000003F804000L.
    *sse &= 0x00000000FFFFFFFFL;
#endif
}

static INLINE int variance_final_from_32bit_sum_avx2(__m256i vsse, __m128i vsum,
    unsigned int *const sse) {
    // extract the low lane and add it to the high lane
    const __m128i sse_reg_128 = mm256_add_hi_lo_epi32(vsse);

    // unpack sse and sum registers and add
    const __m128i sse_sum_lo = _mm_unpacklo_epi32(sse_reg_128, vsum);
    const __m128i sse_sum_hi = _mm_unpackhi_epi32(sse_reg_128, vsum);
    const __m128i sse_sum = _mm_add_epi32(sse_sum_lo, sse_sum_hi);

    // perform the final summation and extract the results
    const __m128i res = _mm_add_epi32(sse_sum, _mm_srli_si128(sse_sum, 8));
    *((int *)sse) = _mm_cvtsi128_si32(res);
    return _mm_extract_epi32(res, 1);
}

// handle pixels (<= 512)
static INLINE int variance_final_512_avx2(__m256i vsse, __m256i vsum,
    unsigned int *const sse) {
    // extract the low lane and add it to the high lane
    const __m128i vsum_128 = mm256_add_hi_lo_epi16(vsum);
    const __m128i vsum_64 = _mm_add_epi16(vsum_128, _mm_srli_si128(vsum_128, 8));
    const __m128i sum_int32 = _mm_cvtepi16_epi32(vsum_64);
    return variance_final_from_32bit_sum_avx2(vsse, sum_int32, sse);
}

// handle 1024 pixels (32x32, 16x64, 64x16)
static INLINE int variance_final_1024_avx2(__m256i vsse, __m256i vsum,
    unsigned int *const sse) {
    // extract the low lane and add it to the high lane
    const __m128i vsum_128 = mm256_add_hi_lo_epi16(vsum);
    const __m128i vsum_64 =
        _mm_add_epi32(_mm_cvtepi16_epi32(vsum_128),
            _mm_cvtepi16_epi32(_mm_srli_si128(vsum_128, 8)));
    return variance_final_from_32bit_sum_avx2(vsse, vsum_64, sse);
}

static INLINE int variance_final_2048_avx2(__m256i vsse, __m256i vsum,
    unsigned int *const sse) {
    vsum = sum_to_32bit_avx2(vsum);
    const __m128i vsum_128 = mm256_add_hi_lo_epi32(vsum);
    return variance_final_from_32bit_sum_avx2(vsse, vsum_128, sse);
}

static INLINE void variance_kernel_avx2(const __m256i src, const __m256i ref,
    __m256i *const sse,
    __m256i *const sum) {
    const __m256i adj_sub = _mm256_set1_epi16(0xff01);  // (1,-1)

    // unpack into pairs of source and reference values
    const __m256i src_ref0 = _mm256_unpacklo_epi8(src, ref);
    const __m256i src_ref1 = _mm256_unpackhi_epi8(src, ref);

    // subtract adjacent elements using src*1 + ref*-1
    const __m256i diff0 = _mm256_maddubs_epi16(src_ref0, adj_sub);
    const __m256i diff1 = _mm256_maddubs_epi16(src_ref1, adj_sub);
    const __m256i madd0 = _mm256_madd_epi16(diff0, diff0);
    const __m256i madd1 = _mm256_madd_epi16(diff1, diff1);

    // add to the running totals
    *sum = _mm256_add_epi16(*sum, _mm256_add_epi16(diff0, diff1));
    *sse = _mm256_add_epi32(*sse, _mm256_add_epi32(madd0, madd1));
}

static INLINE void variance16_kernel_avx2(
    const uint8_t *const src, const int src_stride, const uint8_t *const ref,
    const int ref_stride, __m256i *const sse, __m256i *const sum) {
    const __m128i s0 = _mm_loadu_si128((__m128i const *)(src + 0 * src_stride));
    const __m128i s1 = _mm_loadu_si128((__m128i const *)(src + 1 * src_stride));
    const __m128i r0 = _mm_loadu_si128((__m128i const *)(ref + 0 * ref_stride));
    const __m128i r1 = _mm_loadu_si128((__m128i const *)(ref + 1 * ref_stride));
    const __m256i s = _mm256_inserti128_si256(_mm256_castsi128_si256(s0), s1, 1);
    const __m256i r = _mm256_inserti128_si256(_mm256_castsi128_si256(r0), r1, 1);
    variance_kernel_avx2(s, r, sse, sum);
}

static INLINE void variance32_kernel_avx2(const uint8_t *const src,
    const uint8_t *const ref,
    __m256i *const sse,
    __m256i *const sum) {
    const __m256i s = _mm256_loadu_si256((__m256i const *)(src));
    const __m256i r = _mm256_loadu_si256((__m256i const *)(ref));
    variance_kernel_avx2(s, r, sse, sum);
}

static INLINE void variance16_avx2(const uint8_t *src, const int src_stride,
    const uint8_t *ref, const int ref_stride,
    const int h, __m256i *const vsse,
    __m256i *const vsum) {
    *vsum = _mm256_setzero_si256();

    for (int i = 0; i < h; i += 2) {
        variance16_kernel_avx2(src, src_stride, ref, ref_stride, vsse, vsum);
        src += 2 * src_stride;
        ref += 2 * ref_stride;
    }
}

static INLINE void variance32_avx2(const uint8_t *src, const int src_stride,
    const uint8_t *ref, const int ref_stride,
    const int h, __m256i *const vsse,
    __m256i *const vsum) {
    *vsum = _mm256_setzero_si256();

    for (int i = 0; i < h; i++) {
        variance32_kernel_avx2(src, ref, vsse, vsum);
        src += src_stride;
        ref += ref_stride;
    }
}

static INLINE void variance64_avx2(const uint8_t *src, const int src_stride,
    const uint8_t *ref, const int ref_stride,
    const int h, __m256i *const vsse,
    __m256i *const vsum) {
    *vsum = _mm256_setzero_si256();

    for (int i = 0; i < h; i++) {
        variance32_kernel_avx2(src + 0, ref + 0, vsse, vsum);
        variance32_kernel_avx2(src + 32, ref + 32, vsse, vsum);
        src += src_stride;
        ref += ref_stride;
    }
}

static INLINE void variance128_avx2(const uint8_t *src, const int src_stride,
    const uint8_t *ref, const int ref_stride,
    const int h, __m256i *const vsse,
    __m256i *const vsum) {
    *vsum = _mm256_setzero_si256();

    for (int i = 0; i < h; i++) {
        variance32_kernel_avx2(src + 0, ref + 0, vsse, vsum);
        variance32_kernel_avx2(src + 32, ref + 32, vsse, vsum);
        variance32_kernel_avx2(src + 64, ref + 64, vsse, vsum);
        variance32_kernel_avx2(src + 96, ref + 96, vsse, vsum);
        src += src_stride;
        ref += ref_stride;
    }
}

#define AOM_VAR_NO_LOOP_AVX2(bw, bh, bits, max_pixel)                         \
  unsigned int aom_variance##bw##x##bh##_avx2(                                \
      const uint8_t *src, int src_stride, const uint8_t *ref, int ref_stride, \
      unsigned int *sse) {                                                    \
    __m256i vsse = _mm256_setzero_si256();                                    \
    __m256i vsum;                                                             \
    variance##bw##_avx2(src, src_stride, ref, ref_stride, bh, &vsse, &vsum);  \
    const int sum = variance_final_##max_pixel##_avx2(vsse, vsum, sse);       \
    return *sse - (uint32_t)(((int64_t)sum * sum) >> bits);                   \
  }

AOM_VAR_NO_LOOP_AVX2(16, 4, 6, 512);
AOM_VAR_NO_LOOP_AVX2(16, 8, 7, 512);
AOM_VAR_NO_LOOP_AVX2(16, 16, 8, 512);
AOM_VAR_NO_LOOP_AVX2(16, 32, 9, 512);
AOM_VAR_NO_LOOP_AVX2(16, 64, 10, 1024);

AOM_VAR_NO_LOOP_AVX2(32, 8, 8, 512);
AOM_VAR_NO_LOOP_AVX2(32, 16, 9, 512);
AOM_VAR_NO_LOOP_AVX2(32, 32, 10, 1024);
AOM_VAR_NO_LOOP_AVX2(32, 64, 11, 2048);

AOM_VAR_NO_LOOP_AVX2(64, 16, 10, 1024);
AOM_VAR_NO_LOOP_AVX2(64, 32, 11, 2048);

#define AOM_VAR_LOOP_AVX2(bw, bh, bits, uh)                                   \
  unsigned int aom_variance##bw##x##bh##_avx2(                                \
      const uint8_t *src, int src_stride, const uint8_t *ref, int ref_stride, \
      unsigned int *sse) {                                                    \
    __m256i vsse = _mm256_setzero_si256();                                    \
    __m256i vsum = _mm256_setzero_si256();                                    \
    for (int i = 0; i < (bh / uh); i++) {                                     \
      __m256i vsum16;                                                         \
      variance##bw##_avx2(src, src_stride, ref, ref_stride, uh, &vsse,        \
                          &vsum16);                                           \
      vsum = _mm256_add_epi32(vsum, sum_to_32bit_avx2(vsum16));               \
      src += uh * src_stride;                                                 \
      ref += uh * ref_stride;                                                 \
    }                                                                         \
    const __m128i vsum_128 = mm256_add_hi_lo_epi32(vsum);                     \
    const int sum = variance_final_from_32bit_sum_avx2(vsse, vsum_128, sse);  \
    return *sse - (unsigned int)(((int64_t)sum * sum) >> bits);               \
  }

AOM_VAR_LOOP_AVX2(64, 64, 12, 32);    // 64x32 * ( 64/32)
AOM_VAR_LOOP_AVX2(64, 128, 13, 32);   // 64x32 * (128/32)
AOM_VAR_LOOP_AVX2(128, 64, 13, 16);   // 128x16 * ( 64/16)
AOM_VAR_LOOP_AVX2(128, 128, 14, 16);  // 128x16 * (128/16)
