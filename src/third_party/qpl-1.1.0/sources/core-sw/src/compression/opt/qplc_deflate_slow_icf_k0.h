/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#ifndef OWN_DEFLATE_SLOW_ICF_H
#define OWN_DEFLATE_SLOW_ICF_H

#include "own_qplc_defs.h"
#include "immintrin.h"
#include "qplc_compression_consts.h"

#if PLATFORM >= K0

#define MAX_MATCH   258
#define MIN_MATCH4  4
#define _CMP_MATCH_LENGTH MIN_MATCH4

static inline uint32_t hash_crc(const uint8_t* p_src)
{
    return _mm_crc32_u32(0, *(uint32_t*)(p_src));
} /* hash_src */

static inline uint32_t own_get_offset_table_index(const uint32_t offset) {
    uint32_t x, y, z, u;
    uint32_t res;
    x = offset - 1;
    y = x >> 2;
    z = 32 - _lzcnt_u32(y);
    u = z + z;
    res = u + (x >> z);
    return res;
}
/*
ref code of function:
static inline uint32_t own_get_offset_table_index(const uint32_t offset) {
    if (offset <= 2) {
        return offset - 1;
    }
    else if (offset <= 4) {
        return 0 + (offset - 1) / 1;
    }
    else if (offset <= 8) {
        return 2 + (offset - 1) / 2;
    }
    else if (offset <= 16) {
        return 4 + (offset - 1) / 4;
    }
    else if (offset <= 32) {
        return 6 + (offset - 1) / 8;
    }
    else if (offset <= 64) {
        return 8 + (offset - 1) / 16;
    }
    else if (offset <= 128) {
        return 10 + (offset - 1) / 32;
    }
    else if (offset <= 256) {
        return 12 + (offset - 1) / 64;
    }
    else if (offset <= 512) {
        return 14 + (offset - 1) / 128;
    }
    else if (offset <= 1024) {
        return 16 + (offset - 1) / 256;
    }
    else if (offset <= 2048) {
        return 18 + (offset - 1) / 512;
    }
    else if (offset <= 4096) {
        return 20 + (offset - 1) / 1024;
    }
    else if (offset <= 8192) {
        return 22 + (offset - 1) / 2048;
    }
    else if (offset <= 16384) {
        return 24 + (offset - 1) / 4096;
    }
    else if (offset <= 32768) {
        return 26 + (offset - 1) / 8192;
    }
    else {
        // ~0 is an invalid distance code
        return ~0u;
    }
}

*/

inline static void own_write_deflate_icf(struct deflate_icf* icf,
    uint32_t lit_len,
    uint32_t lit_dist,
    uint32_t extra_bits) {
    *(uint32_t*)icf = lit_len |
        (lit_dist << LIT_LEN_BIT_COUNT) |
        (extra_bits << (LIT_LEN_BIT_COUNT + DIST_LIT_BIT_COUNT));
}
inline static void own_write_deflate_icf_lit(struct deflate_icf* icf, uint32_t lit_len) {
    *(uint32_t*)icf = lit_len | (LITERAL_DISTANCE_IN_ICF << LIT_LEN_BIT_COUNT);
}

OWN_OPT_FUN(uint32_t,k0_slow_deflate_icf_body,(uint8_t* current_ptr,
    const uint8_t* const lower_bound_ptr,
    const uint8_t        * const upper_bound_ptr,
    deflate_hash_table_t * hash_table_ptr,
    isal_mod_hist        * histogram_ptr,
    deflate_icf_stream* icf_stream_ptr)) {
    const uint8_t*       p_src_tmp;
    const uint8_t*       p_str;
    const uint8_t* const p_src = lower_bound_ptr;
    int32_t*             p_hash_table = (int32_t*)hash_table_ptr->hash_table_ptr;
    int32_t*             p_hash_story = (int32_t*)hash_table_ptr->hash_story_ptr;
    int                  src_start = (int)(current_ptr - lower_bound_ptr);
    int                  src_len = (int)(upper_bound_ptr - lower_bound_ptr) - (MAX_MATCH + MIN_MATCH4 - 1);
    int                  dst_len = (int)((int*)icf_stream_ptr->end_ptr - (int*)(icf_stream_ptr->next_ptr)) - 1;
    int                  indx_src = (int)(current_ptr - lower_bound_ptr);
    int                  indx_dst = 0;
    int                  hash_mask = hash_table_ptr->hash_mask;
    int                  win_mask = QPLC_DEFLATE_MAXIMAL_OFFSET - 1;
    int                  hash_key = 0;
    int                  bound, win_bound, tmp, candidat, index;
    uint32_t             win_size = QPLC_DEFLATE_MAXIMAL_OFFSET;
    uint16_t             dist;
    uint8_t              length;

    {
        /* The general case has been replaced with a fixed case for perfpormance reason. */
        //int chain_length_current = hash_table_ptr->attempts;
        int chain_length_current = 256;

        int good_match = hash_table_ptr->good_match;
        int nice_match = hash_table_ptr->nice_match;
        int lazy_match = hash_table_ptr->lazy_match;
        int chain_length;

        if ((dst_len > 1) && (indx_src < src_len)) {
            __m256i     a32, b32;
            int         flag_cmp;
            int         prev_bound = 0;

            uint16_t prev_dist = 0;
            uint8_t prev_ch = 0;
            dst_len -= 1;

            for (; (indx_src < src_len) && (indx_dst < dst_len); indx_src++) {
                p_str = p_src + indx_src;
                win_bound = indx_src - (int)win_size;
                hash_key = hash_crc(p_str) & hash_mask;
                index = tmp = p_hash_table[hash_key];
                p_hash_story[indx_src & win_mask] = index;
                p_hash_table[hash_key] = indx_src;
                bound = prev_bound;
                chain_length = chain_length_current;

                if (prev_bound < lazy_match) {
                    if (prev_bound >= good_match) {
                        chain_length >>= 2;
                    }
                    if (bound < (_CMP_MATCH_LENGTH - 1)) {
                        bound = (_CMP_MATCH_LENGTH - 1);
                    }
                    a32 = _mm256_loadu_si256((__m256i const*)p_str);
                    for (int k = 0; k < chain_length; k++) {
                        if (!(win_bound < tmp)) {
                            break;
                        }
                        p_src_tmp = p_src + tmp;
                        candidat = tmp;
                        tmp = p_hash_story[tmp & win_mask];
                        if (*(uint32_t*)(p_str + bound - 3) != *(uint32_t*)(p_src_tmp + bound - 3)) {
                            continue;
                        }
                        if (bound < 32) {
                            b32 = _mm256_loadu_si256((__m256i const*)p_src_tmp);
                            b32 = _mm256_cmpeq_epi8(b32, a32);
                            flag_cmp = (uint32_t)_mm256_movemask_epi8(b32);
                            flag_cmp = ~flag_cmp;
                            flag_cmp = _tzcnt_u32(flag_cmp);

                            if (flag_cmp < bound) {
                                continue;
                            }
                            if (flag_cmp >= good_match) {
                                if (chain_length == chain_length_current) {
                                    chain_length >>= 2;
                                }
                            }
                            bound = flag_cmp;
                            index = candidat;
                            if (flag_cmp != 32) {
                                continue;
                            }
                            for (; bound < 256; bound += 32) {
                                 b32 = _mm256_cmpeq_epi8(_mm256_loadu_si256((__m256i const*)(p_str + bound)), _mm256_loadu_si256((__m256i const*)(p_src_tmp + bound)));
                                flag_cmp = (uint32_t)_mm256_movemask_epi8(b32);
                                if ((uint32_t)flag_cmp != 0xffffffff) {
                                    break;
                                }
                            }
                            if (bound != 256) {
                                flag_cmp = ~flag_cmp;
                                flag_cmp = _tzcnt_u32(flag_cmp);
                                bound += flag_cmp;
                            } else {
                                if (p_str[256] == p_src_tmp[256]) {
                                    bound = 257;
                                    if (p_str[257] == p_src_tmp[257]) {
                                        bound = 258;
                                    }
                                }
                            }
                            if (bound >= nice_match) {
                                break;
                            }
                        } else {
                            int l;
                            for (l = 0; l < 256; l += 32) {
                                b32 = _mm256_cmpeq_epi8(_mm256_loadu_si256((__m256i const*)(p_str + l)), _mm256_loadu_si256((__m256i const*)(p_src_tmp + l)));
                                flag_cmp = (uint32_t)_mm256_movemask_epi8(b32);
                                if ((uint32_t)flag_cmp != 0xffffffff) {
                                    break;
                                }
                            }
                            if (l != 256) {
                                flag_cmp = ~flag_cmp;
                                flag_cmp = _tzcnt_u32(flag_cmp);
                                l += flag_cmp;
                            } else {
                                if (p_str[256] == p_src_tmp[256]) {
                                    l = 257;
                                    if (p_str[257] == p_src_tmp[257]) {
                                        l = 258;
                                    }
                                }
                            }
                            if (l > bound) {
                                bound = l;
                                index = candidat;
                            }
                            if (bound >= nice_match) {
                                break;
                            }
                        }
                    }
                }

                if (prev_bound > 1) {
                    if ((prev_bound >= bound) && (prev_bound > (_CMP_MATCH_LENGTH - 1))) {
                        uint32_t distance = 0;
                        uint32_t extra_bits = 0;
                        int      k;

                        if (prev_dist < prev_bound) {
                            int m = prev_bound - prev_dist - 3;
                            if (m < 1) {
                                m = 1;
                            }
                            for (k = indx_src + m, indx_src += prev_bound - 2; k <= indx_src; k++) {
                                hash_key = hash_crc(p_src + k) & hash_mask;
                                p_hash_story[k & win_mask] = p_hash_table[hash_key];
                                p_hash_table[hash_key] = k;
                            }
                        } else {
                            for (k = indx_src + 1, indx_src += prev_bound - 2; k <= indx_src; k++) {
                                hash_key = hash_crc(p_src + k) & hash_mask;
                                p_hash_story[k & win_mask] = p_hash_table[hash_key];
                                p_hash_table[hash_key] = k;
                            }                           
                        }
                        get_distance_icf_code(prev_dist, &distance, &extra_bits);
                        own_write_deflate_icf(icf_stream_ptr->next_ptr, prev_bound + LEN_OFFSET, distance, extra_bits);
                        icf_stream_ptr->next_ptr++;
                        indx_dst++;
                        bound = 0;
                        histogram_ptr->ll_hist[prev_bound + LEN_OFFSET]++;
                        histogram_ptr->d_hist[own_get_offset_table_index(prev_dist)]++;
                    } else {
                        own_write_deflate_icf_lit(icf_stream_ptr->next_ptr, prev_ch);
                        icf_stream_ptr->next_ptr++;
                        indx_dst++;
                        histogram_ptr->ll_hist[prev_ch]++;
                    }
                }
                prev_dist = (uint16_t)(indx_src - index);
                prev_bound = bound;
                prev_ch = p_src[indx_src];
            }

            {
                int bound_lim;

                src_len += MAX_MATCH;
                for (; ((src_len - indx_src) > 0) && (indx_dst < dst_len); indx_src++) {
                    p_str = p_src + indx_src;
                    win_bound = indx_src - (int)win_size;
                    hash_key = hash_crc(p_str) & hash_mask;
                    index = tmp = p_hash_table[hash_key];
                    p_hash_story[indx_src & win_mask] = index;
                    p_hash_table[hash_key] = indx_src;
                    bound_lim = QPL_MIN((src_len - indx_src), MAX_MATCH);
                    bound = prev_bound;
                    chain_length = chain_length_current;

                    if (prev_bound < lazy_match) {
                        if (prev_bound >= good_match) {
                            chain_length >>= 2;
                        }
                        if (bound < (_CMP_MATCH_LENGTH - 1)) {
                            bound = (_CMP_MATCH_LENGTH - 1);
                        }
                        for (int k = 0; k < chain_length; k++) {
                            if (!(win_bound < tmp)) {
                                break;
                            }
                            p_src_tmp = p_src + tmp;
                            candidat = tmp;
                            tmp = p_hash_story[tmp & win_mask];
                            if (*(uint32_t*)(p_str + bound - 3) != *(uint32_t*)(p_src_tmp + bound - 3)) {
                                continue;
                            }
                            {
                                int l;
                                for (l = 0; l < bound_lim; l++) {
                                    if (p_str[l] != p_src_tmp[l]) {
                                        break;
                                    }
                                }
                                if (bound < l)
                                {
                                    bound = l;
                                    index = candidat;
                                    if (bound >= nice_match) {
                                        break;
                                    }
                                }
                            }
                        }
                    }
                    if (prev_bound > 1) {
                        if ((prev_bound >= bound) && (prev_bound > (_CMP_MATCH_LENGTH - 1))) {
                            uint32_t distance = 0;
                            uint32_t extra_bits = 0;
                            int k;

                            if (prev_dist < prev_bound) {
                                int m = prev_bound - prev_dist - 3;
                                if (m < 1) {
                                    m = 1;
                                }
                                for (k = indx_src + m, indx_src += prev_bound - 2; k <= indx_src; k++) {
                                    hash_key = hash_crc(p_src + k) & hash_mask;
                                    p_hash_story[k & win_mask] = p_hash_table[hash_key];
                                    p_hash_table[hash_key] = k;
                                }
                            } else {
                                for (k = indx_src + 1, indx_src += prev_bound - 2; k <= indx_src; k++) {
                                    hash_key = hash_crc(p_src + k) & hash_mask;
                                    p_hash_story[k & win_mask] = p_hash_table[hash_key];
                                    p_hash_table[hash_key] = k;
                                }
                            }

                            get_distance_icf_code(prev_dist, &distance, &extra_bits);
                            own_write_deflate_icf(icf_stream_ptr->next_ptr, prev_bound + LEN_OFFSET, distance, extra_bits);
                            icf_stream_ptr->next_ptr++;
                            indx_dst++;
                            bound = 0;
                            histogram_ptr->ll_hist[prev_bound + LEN_OFFSET]++;
                            histogram_ptr->d_hist[own_get_offset_table_index(prev_dist)]++;
                        } else {
                            own_write_deflate_icf_lit(icf_stream_ptr->next_ptr, prev_ch);
                            icf_stream_ptr->next_ptr++;
                            indx_dst++;
                            histogram_ptr->ll_hist[prev_ch]++;
                        }
                    }
                    prev_dist = (uint16_t)(indx_src - index);
                    prev_bound = bound;
                    prev_ch = p_src[indx_src];
                }
            }
            dst_len += 1;
            if (prev_bound > 1) {
                if (prev_bound > (_CMP_MATCH_LENGTH - 1)) {
                    uint32_t distance = 0;
                    uint32_t extra_bits = 0;
                    int k;

                    for (k = indx_src + 1, indx_src += prev_bound - 1; k < indx_src; k++) {
                        hash_key = hash_crc(p_src + k) & hash_mask;
                        p_hash_story[k & win_mask] = p_hash_table[hash_key];
                        p_hash_table[hash_key] = k;
                    }
                    get_distance_icf_code(prev_dist, &distance, &extra_bits);
                    own_write_deflate_icf(icf_stream_ptr->next_ptr, prev_bound + LEN_OFFSET, distance, extra_bits);
                    icf_stream_ptr->next_ptr++;
                    indx_dst++;
                    bound = 0;
                    histogram_ptr->ll_hist[prev_bound + LEN_OFFSET]++;
                    histogram_ptr->d_hist[own_get_offset_table_index(prev_dist)]++;
                } else  {
                    own_write_deflate_icf_lit(icf_stream_ptr->next_ptr, prev_ch);
                    icf_stream_ptr->next_ptr++;
                    indx_dst++;
                    histogram_ptr->ll_hist[prev_ch]++;
                }
            }

            {
                for (src_len += (MIN_MATCH4 - 1); ((src_len - indx_src) > 0) && (indx_dst < dst_len); indx_src++, indx_dst++) {
                    prev_ch = p_src[indx_src];
                    own_write_deflate_icf_lit(icf_stream_ptr->next_ptr, prev_ch);
                    icf_stream_ptr->next_ptr++;
                    histogram_ptr->ll_hist[prev_ch]++;
                    if ((src_len - (indx_src + (_CMP_MATCH_LENGTH - 1))) > 0) {
                        hash_key = hash_crc(p_src + indx_src) & hash_mask;
                        p_hash_story[indx_src & win_mask] = p_hash_table[hash_key];
                        p_hash_table[hash_key] = indx_src;
                    }
                }
            }
        } else {
            int mx_match = MAX_MATCH;
            for (; ((src_len - indx_src) > 0) && (indx_dst < dst_len); indx_src++, indx_dst++) {
                p_str = p_src + indx_src;
                win_bound = indx_src - (int)win_size;
                hash_key = hash_crc(p_str) & hash_mask;
                index = tmp = p_hash_table[hash_key];
                p_hash_story[indx_src & win_mask] = index;
                p_hash_table[hash_key] = indx_src;
                bound = (_CMP_MATCH_LENGTH - 1);
                chain_length = chain_length_current;
                for (int k = 0; ((win_bound - tmp) & (k - chain_length)) < 0; k++) {
                    p_src_tmp = p_src + tmp;
                    candidat = tmp;
                    tmp = p_hash_story[tmp & win_mask];
                    if (*(uint32_t*)(p_str + bound - 3) == *(uint32_t*)(p_src_tmp + bound - 3)) {
                        int l;
                        for (l = 0; l < mx_match; l++) {
                            if ((p_str[l] - p_src_tmp[l])) {
                                break;
                            }
                        }
                        if (bound < l) {
                            bound = l;
                            index = candidat;
                            if (bound >= mx_match) {
                                break;
                            }
                        }
                    }
                }
                dist = 0;
                length = p_str[0];
                if (bound >= _CMP_MATCH_LENGTH)  {
                    uint32_t distance = 0;
                    uint32_t extra_bits = 0;
                    int k;

                    dist = (uint16_t)(indx_src - index);
                    length = (uint8_t)(bound - 3);
                    get_distance_icf_code(dist, &distance, &extra_bits);
                    own_write_deflate_icf(icf_stream_ptr->next_ptr, bound + LEN_OFFSET, distance, extra_bits);
                    icf_stream_ptr->next_ptr++;
                    indx_dst++;
                    histogram_ptr->ll_hist[bound + LEN_OFFSET]++;
                    histogram_ptr->d_hist[own_get_offset_table_index(dist)]++;
                    if (dist < bound) {
                        int m = bound - dist - 3;
                        if (m < 1) {
                            m = 1;
                        }
                        for (k = indx_src + m, indx_src += bound - 1; k <= indx_src; k++) {
                            hash_key = hash_crc(p_src + k) & hash_mask;
                            p_hash_story[k & win_mask] = p_hash_table[hash_key];
                            p_hash_table[hash_key] = k;
                        }
                    } else {
                        for (k = indx_src + 1, indx_src += bound - 1; k <= indx_src; k++) {
                            hash_key = hash_crc(p_src + k) & hash_mask;
                            p_hash_story[k & win_mask] = p_hash_table[hash_key];
                            p_hash_table[hash_key] = k;
                        }
                    }
                } else {
                    //own_write_deflate_icf(icf_stream_ptr->next_ptr, length, LITERAL_DISTANCE_IN_ICF, 0);
                    own_write_deflate_icf_lit(icf_stream_ptr->next_ptr, length);
                    icf_stream_ptr->next_ptr++;
                    indx_dst++;
                    histogram_ptr->ll_hist[length]++;
                }
            }

            {
                int bound_lim;

                src_len += MAX_MATCH;
                for (; ((src_len - indx_src) > 0) && (indx_dst < dst_len); indx_src++) {
                    p_str = p_src + indx_src;
                    win_bound = indx_src - (int)win_size;
                    hash_key = hash_crc(p_str) & hash_mask;
                    index = tmp = p_hash_table[hash_key];
                    p_hash_story[indx_src & win_mask] = index;
                    p_hash_table[hash_key] = indx_src;
                    bound_lim = QPL_MIN((src_len - indx_src), MAX_MATCH);
                    bound = 3;
                    chain_length = chain_length_current;
                    for (int k = 0; ((win_bound - tmp) & (k - chain_length)) < 0; k++) {
                        p_src_tmp = p_src + tmp;
                        candidat = tmp;
                        tmp = p_hash_story[tmp & win_mask];
                        if (*(uint32_t*)(p_str + bound - 3) == *(uint32_t*)(p_src_tmp + bound - 3)) {
                            int l;
                            for (l = 0; l < bound_lim; l++) {
                                if ((p_str[l] - p_src_tmp[l])) {
                                    break;
                                }
                            }
                            if (bound < l)
                            {
                                bound = l;
                                index = candidat;
                                if (bound >= bound_lim) {
                                    break;
                                }
                            }
                        }
                    }
                    dist = 0;
                    length = p_str[0];
                    if (bound >= _CMP_MATCH_LENGTH) {
                        uint32_t distance = 0;
                        uint32_t extra_bits = 0;
                        int k;

                        dist = (uint16_t)(indx_src - index);
                        length = (uint8_t)(bound - 3);
                        get_distance_icf_code(dist, &distance, &extra_bits);
                        own_write_deflate_icf(icf_stream_ptr->next_ptr, bound + LEN_OFFSET, distance, extra_bits);
                        icf_stream_ptr->next_ptr++;
                        indx_dst++;
                        histogram_ptr->ll_hist[bound + LEN_OFFSET]++;
                        histogram_ptr->d_hist[own_get_offset_table_index(dist)]++;
                        if (dist < bound) {
                            int m = bound - dist - 3;
                            if (m < 1) {
                                m = 1;
                            }
                            for (k = indx_src + m, indx_src += bound - 1; k <= indx_src; k++) {
                                hash_key = hash_crc(p_src + k) & hash_mask;
                                p_hash_story[k & win_mask] = p_hash_table[hash_key];
                                p_hash_table[hash_key] = k;
                            }
                        } else {
                            for (k = indx_src + 1, indx_src += bound - 1; k <= indx_src; k++) {
                                hash_key = hash_crc(p_src + k) & hash_mask;
                                p_hash_story[k & win_mask] = p_hash_table[hash_key];
                                p_hash_table[hash_key] = k;
                            }
                        }
                    } else {
                        own_write_deflate_icf_lit(icf_stream_ptr->next_ptr, length);
                        icf_stream_ptr->next_ptr++;
                        indx_dst++;
                        histogram_ptr->ll_hist[length]++;
                    }
                }
                for (src_len += (MIN_MATCH4 - 1); ((src_len - indx_src) > 0) && (indx_dst < dst_len); indx_src++) {
                    own_write_deflate_icf_lit(icf_stream_ptr->next_ptr, p_src[indx_src]);
                    icf_stream_ptr->next_ptr++;
                    indx_dst++;
                    histogram_ptr->ll_hist[p_src[indx_src]]++;
                    if ((src_len - (indx_src + (_CMP_MATCH_LENGTH - 1))) > 0) {
                        hash_key = hash_crc(p_src + indx_src) & hash_mask;
                        p_hash_story[indx_src & win_mask] = p_hash_table[hash_key];
                        p_hash_table[hash_key] = indx_src;
                    }
                }
            }
        }
    }
    return (uint32_t)(indx_src - src_start);
}
#endif

#endif // OWN_DEFLATE_SLOW_ICF_H
