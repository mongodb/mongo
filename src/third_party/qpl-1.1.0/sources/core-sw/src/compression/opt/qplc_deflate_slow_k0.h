/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#ifndef OWN_DEFLATE_SLOW_H
#define OWN_DEFLATE_SLOW_H

#include "own_qplc_defs.h"
#include "qplc_compression_consts.h"
#include "immintrin.h"

#if PLATFORM >= K0

#define MAX_MATCH   258
#define MIN_MATCH4  4
#define CMP_MATCH_LENGTH MIN_MATCH4

static inline uint32_t hash_crc(const uint8_t* p_src)
{
    return _mm_crc32_u32(0, *(uint32_t*)(p_src));
} /* hash_src */


static inline void own_flush_bits(struct BitBuf2* me)
{
    uint32_t bits;
    *(int64_t*)me->m_out_buf = me->m_bits;
    bits = me->m_bit_count & ~7;
    me->m_bit_count -= bits;
    me->m_out_buf += bits / 8;
    me->m_bits >>= bits;
}

static inline void own_flush_bits_safe(struct BitBuf2* me)
{
    uint32_t bits;
    if ((me->m_out_buf + 8) <= me->m_out_end) {
        *(int64_t*)me->m_out_buf = me->m_bits;
        bits = me->m_bit_count & ~7;
        me->m_bit_count -= bits;
        me->m_out_buf += bits / 8;
        me->m_bits >>= bits;
    }
    else {
        for (; me->m_bit_count >= 8; me->m_bit_count -= 8) {
            if (me->m_out_buf >= me->m_out_end) {
                break;
            }
            *me->m_out_buf++ = (uint8_t)me->m_bits;
            me->m_bits >>= 8;
        }
    }
}

static inline void own_write_bits(struct BitBuf2* me, uint64_t code, uint32_t count)
{	/* Assumes there is space to fit code into m_bits. */
    me->m_bits |= code << me->m_bit_count;
    me->m_bit_count += count;
    own_flush_bits(me);
}

static inline void own_write_bits_safe(struct BitBuf2* me, uint64_t code, uint32_t count)
{	/* Assumes there is space to fit code into m_bits. */
    me->m_bits |= code << me->m_bit_count;
    me->m_bit_count += count;
    own_flush_bits_safe(me);
}


static inline uint32_t own_count_significant_bits(uint32_t value) {
    return (32 - _lzcnt_u32(value));
}

static inline void _compute_offset_code(const struct isal_hufftables* huffman_table_ptr,
    uint16_t offset,
    uint64_t* const code_ptr,
    uint32_t* const code_length_ptr) {
    // Variables
    uint32_t significant_bits;
    uint32_t number_of_extra_bits;
    uint32_t extra_bits;
    uint32_t symbol;
    uint32_t length;
    uint32_t code;

    offset -= 1u;
    significant_bits = own_count_significant_bits(offset);

    number_of_extra_bits = significant_bits - 2u;
    //extra_bits = offset & ((1u << number_of_extra_bits) - 1u);
    extra_bits = _bzhi_u32(offset, number_of_extra_bits);
    offset >>= number_of_extra_bits;
    symbol = offset + 2 * number_of_extra_bits;

    // Extracting information from table
    code = huffman_table_ptr->dcodes[symbol];
    length = huffman_table_ptr->dcodes_sizes[symbol];

    // Return of the calculated results
    *code_ptr = code | (extra_bits << length);
    *code_length_ptr = length + number_of_extra_bits;
}


void _get_offset_code(const struct isal_hufftables* const huffman_table_ptr,
    uint32_t offset,
    uint64_t* const code_ptr,
    uint32_t* const code_length_ptr) {

    if (offset <= IGZIP_DIST_TABLE_SIZE && offset > 0u) {
        const uint64_t offset_info = huffman_table_ptr->dist_table[offset - 1];

        *code_ptr = offset_info >> 5u;
        *code_length_ptr = offset_info & 0x1Fu;
    }
    else {
        _compute_offset_code(huffman_table_ptr, offset, code_ptr, code_length_ptr);
    }
}


OWN_OPT_FUN(uint32_t, k0_slow_deflate_body,(uint8_t* current_ptr,
    const uint8_t* const lower_bound_ptr,
    const uint8_t          * const upper_bound_ptr,
    deflate_hash_table_t   * hash_table_ptr,
    struct isal_hufftables * huffman_tables_ptr,
    struct BitBuf2* bit_writer_ptr)) {

    const uint8_t* p_src_tmp;
    const uint8_t* p_str;
    const uint8_t* const p_src = lower_bound_ptr;
    int32_t* p_hash_table = (int32_t*)hash_table_ptr->hash_table_ptr;
    int32_t* p_hash_story = (int32_t*)hash_table_ptr->hash_story_ptr;
    int      src_start = (int)(current_ptr - lower_bound_ptr);
    int      src_len = (int)(upper_bound_ptr - lower_bound_ptr) - (MAX_MATCH + MIN_MATCH4 - 1);
    int      indx_src = (int)(current_ptr - lower_bound_ptr);
    int      hash_mask = hash_table_ptr->hash_mask;
    int      win_mask = QPLC_DEFLATE_MAXIMAL_OFFSET - 1;
    int      hash_key = 0;
    int      bound, win_bound, tmp, candidat, index;
    uint32_t win_size = QPLC_DEFLATE_MAXIMAL_OFFSET;

    {
        //        int chain_length_current = hash_table_ptr->attempts;
        int chain_length_current = 256; /* temporary */
        int good_match = hash_table_ptr->good_match;
        int nice_match = hash_table_ptr->nice_match;
        int lazy_match = hash_table_ptr->lazy_match;
        int chain_length;

        {
            __m256i     a32, b32;
            int         flag_cmp;
            int         prev_bound = 0;

            uint16_t prev_dist = 0;
            uint8_t prev_ch = 0;
            //dst_len -= 1;

            for (; (indx_src < src_len) && ((bit_writer_ptr->m_out_buf + 8) <= bit_writer_ptr->m_out_end); indx_src++) {
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
                    if (bound < (CMP_MATCH_LENGTH - 1)) {
                        bound = (CMP_MATCH_LENGTH - 1);
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
                        }
                        else {
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
                            }
                            else {
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
                    uint64_t code;
                    uint32_t code_length;
                    if ((prev_bound >= bound) && (prev_bound > (CMP_MATCH_LENGTH - 1))) {
                        const uint64_t match_length_info = huffman_tables_ptr->len_table[prev_bound - 3u];
                        uint64_t code_match;
                        uint32_t code_match_length;
                        uint64_t code_offset;
                        uint32_t code_offset_length;
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
                        }
                        else {
                            for (k = indx_src + 1, indx_src += prev_bound - 2; k <= indx_src; k++) {
                                hash_key = hash_crc(p_src + k) & hash_mask;
                                p_hash_story[k & win_mask] = p_hash_table[hash_key];
                                p_hash_table[hash_key] = k;
                            }
                        }

                        code_match = match_length_info >> 5u;
                        code_match_length = match_length_info & 0x1Fu;
                        _get_offset_code(huffman_tables_ptr, prev_dist, &code_offset, &code_offset_length);
                        // Combining two codes
                        code_match |= code_offset << code_match_length;
                        // Writing to the output
                        own_write_bits(bit_writer_ptr, code_match, code_match_length + code_offset_length);
                        bound = 0;
                    }
                    else {
                        code = huffman_tables_ptr->lit_table[prev_ch];
                        code_length = huffman_tables_ptr->lit_table_sizes[prev_ch];
                        own_write_bits(bit_writer_ptr, code, code_length);
                    }
                }
                prev_dist = (uint16_t)(indx_src - index);
                prev_bound = bound;
                prev_ch = p_src[indx_src];
            }

            {
                int bound_lim;

                src_len += MAX_MATCH;
                for (; ((src_len - indx_src) > 0) && (bit_writer_ptr->m_out_buf < bit_writer_ptr->m_out_end); indx_src++) {
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
                        if (bound < (CMP_MATCH_LENGTH - 1)) {
                            bound = (CMP_MATCH_LENGTH - 1);
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
                                if (bound < l) {
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
                        uint64_t code;
                        uint32_t code_length;
                        if ((prev_bound >= bound) && (prev_bound > (CMP_MATCH_LENGTH - 1))) {
                            const uint64_t match_length_info = huffman_tables_ptr->len_table[prev_bound - 3u];
                            uint64_t code_match;
                            uint32_t code_match_length;
                            uint64_t code_offset;
                            uint32_t code_offset_length;
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
                            }
                            else {
                                for (k = indx_src + 1, indx_src += prev_bound - 2; k <= indx_src; k++) {
                                    hash_key = hash_crc(p_src + k) & hash_mask;
                                    p_hash_story[k & win_mask] = p_hash_table[hash_key];
                                    p_hash_table[hash_key] = k;
                                }
                            }
                            code_match = match_length_info >> 5u;
                            code_match_length = match_length_info & 0x1Fu;
                            _get_offset_code(huffman_tables_ptr, prev_dist, &code_offset, &code_offset_length);
                            // Combining two codes
                            code_match |= code_offset << code_match_length;
                            // Writing to the output
                            own_write_bits_safe(bit_writer_ptr, code_match, code_match_length + code_offset_length);
                            bound = 0;
                        }
                        else {
                            code = huffman_tables_ptr->lit_table[prev_ch];
                            code_length = huffman_tables_ptr->lit_table_sizes[prev_ch];
                            own_write_bits_safe(bit_writer_ptr, code, code_length);
                        }
                    }
                    prev_dist = (uint16_t)(indx_src - index);
                    prev_bound = bound;
                    prev_ch = p_src[indx_src];
                }
            }
            if (prev_bound > 1) {
                uint64_t code;
                uint32_t code_length;
                if (prev_bound > (CMP_MATCH_LENGTH - 1)) {
                    const uint64_t match_length_info = huffman_tables_ptr->len_table[prev_bound - 3u];
                    uint64_t code_match;
                    uint32_t code_match_length;
                    uint64_t code_offset;
                    uint32_t code_offset_length;
                    int k;

                    for (k = indx_src + 1, indx_src += prev_bound - 1; k < indx_src; k++) {
                        hash_key = hash_crc(p_src + k) & hash_mask;
                        p_hash_story[k & win_mask] = p_hash_table[hash_key];
                        p_hash_table[hash_key] = k;
                    }
                    code_match = match_length_info >> 5u;
                    code_match_length = match_length_info & 0x1Fu;
                    _get_offset_code(huffman_tables_ptr, prev_dist, &code_offset, &code_offset_length);
                    // Combining two codes
                    code_match |= code_offset << code_match_length;
                    // Writing to the output
                    own_write_bits_safe(bit_writer_ptr, code_match, code_match_length + code_offset_length);
                    bound = 0;
                } else {
                    code = huffman_tables_ptr->lit_table[prev_ch];
                    code_length = huffman_tables_ptr->lit_table_sizes[prev_ch];
                    own_write_bits_safe(bit_writer_ptr, code, code_length);
                }
            }
            {
                uint64_t code;
                uint32_t code_length;
                for (src_len += (MIN_MATCH4 - 1); ((src_len - indx_src) > 0) && (bit_writer_ptr->m_out_buf < bit_writer_ptr->m_out_end); indx_src++) {
                    prev_ch = p_src[indx_src];
                    code = huffman_tables_ptr->lit_table[prev_ch];
                    code_length = huffman_tables_ptr->lit_table_sizes[prev_ch];
                    own_write_bits_safe(bit_writer_ptr, code, code_length);
                    if ((src_len - (indx_src + (CMP_MATCH_LENGTH - 1))) > 0) {
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

#endif // OWN_DEFLATE_SLOW_H
