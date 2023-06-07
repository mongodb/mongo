/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/**
 * @date 03/23/2020
 * @brief Internal HW API functions for @ref hw_descriptor_compress_init_deflate_base API implementation
 *
 * @addtogroup HW_SUBMIT_COMPRESS_API
 * @{
 */

#include "hw_aecs_api.h"
#include "own_compress.h"

#define PLATFORM 2
#include "qplc_memop.h"

#define OWN_ONE_64U (1ULL)

/**
 * @todo
 */
typedef struct {
    uint64_t bits;         /**< @todo */
    uint8_t  *out_buf;     /**< @todo */
    uint8_t  *start_buf;   /**< @todo */
    uint8_t  *end_buf;     /**< @todo */
    uint32_t bit_count;    /**< @todo */
} bitbuf2;

extern const uint8_t own_reversed_bits_table[];

static inline uint16_t own_bit_reverse(uint16_t code, const uint32_t length) {
    code = (own_reversed_bits_table[code & 0x00FF] << 8u) | (own_reversed_bits_table[code >> 8u]);

    return (code >> (16 - length));
}

/**
 * @brief Helper for shifting heap array 1..n to start from idx
 *
 * @param[in]  heap_ptr  pointer to heap array
 * @param[in]  len       array size
 * @param[in]  idx       shift parameter
 *
 */
static void hw_heapify64(uint64_t *heap_ptr, uint32_t len, uint32_t idx) {
    uint32_t child;
    uint64_t tmp;

    while ((child = 2u * idx) <= len) {
        if (heap_ptr[child] > heap_ptr[child + 1u]) {
            child++;
        }
        if (heap_ptr[idx] <= heap_ptr[child]) {
            break;
        }
        // Swap idx and child
        tmp = heap_ptr[idx];
        heap_ptr[idx]   = heap_ptr[child];
        heap_ptr[child] = tmp;
        idx = child;
    }
}

/**
 * @brief Helper for building heap array
 *
 * @param[in]  heap_ptr  pointer to heap array
 * @param[in]  len       array size
 *
 */
static void hw_build_heap64(uint64_t *heap_ptr, uint32_t len) {
    uint32_t idx;

    heap_ptr[len + 1u] = UINT64_MAX;
    for (idx = len / 2u; idx > 0u; idx--) {
        hw_heapify64(heap_ptr, len, idx);
    }
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstack-usage=4096"
#endif

/**
 * @brief Helper for building Huffman tree
 *
 * @param[in]   histogram_ptr  pointer to histogram
 * @param[in]   hist_size      histogram size
 * @param[in]   bl_count_ptr   pointer to code lengths
 * @param[out]  codes_ptr      pointer to Huffman tree
 * @param[in]   max_code_len   maximum Huffman code length
 *
 */
void hw_create_huff_tree(uint32_t *histogram_ptr,
                         uint32_t hist_size,
                         uint32_t *bl_count_ptr,
                         uint32_t *codes_ptr,
                         uint32_t max_code_len) {
    uint64_t heap[HEAP_SIZE];
    uint64_t *heap_freq_ptr = (uint64_t *) (4u + (uint8_t *) &heap[0]);
    uint32_t heap_size;
    uint32_t idx;
    uint32_t jdx;
    uint32_t kdx;
    uint32_t child;
    uint32_t d1;
    uint32_t d2;
    uint32_t code_len;
    uint64_t h1;
    uint64_t h2;
    uint64_t h_new;
    uint32_t node_ptr       = NODE_START;

    heap_size = 0u;

    for (idx       = 0u; idx < hist_size; idx++) {
        if (histogram_ptr[idx] != 0u) {
            heap[++heap_size] = (((uint64_t) histogram_ptr[idx]) << FREQ_SHIFT) | idx;
        }
    }

    // make sure heap has at least two elements in it
    if (2u > heap_size) {
        if (0u == heap_size) {
            heap[1] = OWN_ONE_64U << FREQ_SHIFT;
            heap[2] = (OWN_ONE_64U << FREQ_SHIFT) | 1u;
            heap_size = 2u;
        } else {
            // heap size == 1
            heap[2] = (0u == histogram_ptr[0]) ? OWN_ONE_64U << FREQ_SHIFT : (OWN_ONE_64U << FREQ_SHIFT) | 1u;
            heap_size = 2u;
        }
    }

    hw_build_heap64(heap, heap_size);

    do {
        h1 = heap[1];
        heap[1]           = heap[heap_size];
        heap[heap_size--] = UINT64_MAX;
        hw_heapify64(heap, heap_size, 1u);

        h2 = heap[1];
        heap[node_ptr]      = (uint32_t) h1;
        heap[node_ptr - 1u] = (uint32_t) h2;

        h_new = (h1 + h2) & FREQ_MASK_HI;
        d1    = (uint32_t) h1;
        d2    = (uint32_t) h2;
        if (d1 > d2) {
            h2 = h1;
        }
        h_new |= node_ptr | ((h2 + DEPTH_1) & DEPTH_MASK_HI);

        node_ptr -= 2u;
        heap[1] = h_new;
        hw_heapify64(heap, heap_size, 1u);
    } while (heap_size > 1u);
    heap[node_ptr] = (uint32_t) heap[1];

    // compute code lengths and code length counts
    code_len = 0u;
    jdx      = node_ptr;
    for (idx = node_ptr; idx <= NODE_START; idx++) {
        child = (uint16_t) heap[idx];
        if (MAX_HEAP < child) {
            d1 = 1u + *(uint32_t *) &(heap_freq_ptr[idx]);
            *(uint32_t *) &(heap_freq_ptr[child])      = d1;
            *(uint32_t *) &(heap_freq_ptr[child - 1u]) = d1;
        } else {
            heap[jdx++] = heap[idx];
            d1 = *(uint32_t *) &(heap_freq_ptr[idx]);
            while (code_len < d1) {
                code_len++;
                heap[code_len] = 0u;
            }
            heap[d1]++;
        }
    }

    if (code_len > max_code_len) {
        while (code_len > max_code_len) {
            for (idx = max_code_len - 1u; idx != 0u; idx--) {
                if (0u != heap[idx]) {
                    break;
                }
            }
            heap[idx]--;
            heap[idx + 1u] += 2u;
            heap[code_len - 1u]++;
            heap[code_len] -= 2u;
            if (0u == heap[code_len]) {
                code_len--;
            }
        }

        for (idx = 1u; idx <= code_len; idx++) {
            bl_count_ptr[idx] = (uint32_t) (heap[idx]);
        }
        for (; idx <= max_code_len; idx++) {
            bl_count_ptr[idx] = 0u;
        }
        for (kdx = 1u; heap[kdx] == 0u; kdx++) {
        }
        for (idx = node_ptr; idx < jdx; idx++) {
            *(uint32_t *) &(heap_freq_ptr[idx]) = kdx;
            heap[kdx]--;
            for (; heap[kdx] == 0u; kdx++) {
            }
        }
    } else {
        for (idx = 1u; idx <= code_len; idx++) {
            bl_count_ptr[idx] = (uint32_t) (heap[idx]);
        }
        for (; idx <= max_code_len; idx++) {
            bl_count_ptr[idx] = 0u;
        }
    }

    avx512_qplc_zero_8u((uint8_t *) codes_ptr, hist_size * sizeof(*codes_ptr));
    for (idx = node_ptr; idx < jdx; idx++) {
        h1 = heap[idx];
        codes_ptr[(uint32_t) h1] = (uint32_t) (h1 >> 32u);
    }
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

/**
 * @brief helper to compute Huffman codes
 *
 * @param[out]  codes_ptr     pointer to calculated codes: 18:15 code length, 14:0  unreversed code value
 *                            in low order bits
 * @param[in]   num_codes     number of codes
 * @param[in]   bl_count_ptr  pointer to code lengths
 * @param[in]   max_code_len  maximum Huffman code length
 * @param[out]  max_code_ptr  pointer to calculated max code value. Can be set to NULL to ignore max code value
 *
 */
void hw_compute_codes(uint32_t *codes_ptr, uint32_t num_codes, uint32_t *bl_count_ptr, uint32_t max_code_len, uint32_t *max_code_ptr) {
    uint32_t next_code[MAX_CODE_LEN + 1u];
    uint32_t code     = 0u;
    uint32_t max_code = 0u;

    bl_count_ptr[0] = 0u;

    for (uint32_t bits = 1u; bits <= max_code_len; bits++) {
        code = (code + bl_count_ptr[bits - 1u]) << 1u;
        next_code[bits] = code;
    }

    for (uint32_t idx  = 0u; idx < num_codes; idx++) {
        code = codes_ptr[idx];
        if (0u != code) {
            codes_ptr[idx] = (code << 15u) | next_code[code];
            next_code[code] += 1u;
            max_code = idx;
        }
    }

    // if max_code is needed, return it.
    if (max_code_ptr) {
        *max_code_ptr = max_code;
    }
}

/**
 *  @brief helper run-length encoding
 *  @todo
 */
static uint16_t *hw_write_rl(uint16_t *out_ptr, uint16_t last_len, uint32_t run_len, uint32_t *counts_ptr) {
    if (0u == last_len) {
        while (138u < run_len) {
            *out_ptr++ = 18u | ((138u - 11u) << 8u);
            run_len -= 138u;
            counts_ptr[18]++;
        }
        // 1 <= run_len <= 138
        if (10u < run_len) {
            *out_ptr++ = 18u | ((run_len - 11u) << 8u);
            counts_ptr[18]++;
        } else if (2u < run_len) {
            *out_ptr++ = 17u | ((run_len - 3u) << 8u);
            counts_ptr[17]++;
        } else if (1u == run_len) {
            *out_ptr++ = 0u;
            counts_ptr[0]++;
        } else {
            out_ptr[0] = out_ptr[1] = 0u;
            out_ptr += 2u;
            counts_ptr[0] += 2u;
        }
    } else {
        // last_len != 0
        *out_ptr++ = last_len;
        counts_ptr[last_len]++;
        run_len--;
        if (0u != run_len) {
            while (6u < run_len) {
                *out_ptr++ = 16u | ((6u - 3u) << 8u);
                run_len -= 6u;
                counts_ptr[16]++;
            }
            // 1 <= run_len <= 6
            switch (run_len) {
                case 1:
                    *out_ptr++ = last_len;
                    counts_ptr[last_len]++;
                    break;
                case 2:
                    out_ptr[0] = out_ptr[1] = last_len;
                    out_ptr += 2u;
                    counts_ptr[last_len] += 2u;
                    break;
                default: // 3...6
                    *out_ptr++ = 16u | ((run_len - 3u) << 8u);
                    counts_ptr[16]++;
            }
        }
    }

    return out_ptr;
}

/**
 * @brief helper to convert codes into run-length symbols, write symbols into dst_ptr, and generate histogram
 *        into counts_ptr (assumed to be initialized to 0)
 *
 * @param[in]  codes_ptr   pointer to codes
 * @param[in]  num_codes   number of codes
 * @param[in]  counts_ptr  pointer to histogram
 * @param[in]  dst_ptr     converted codes output - format of dst_ptr: 4:0  code (0...18), 15:8 Extra bits (0...127)
 *
 * @return number of symbols in dst_ptr
 *
 */
uint32_t hw_rl_encode(uint32_t *codes_ptr, uint32_t num_codes, uint32_t *counts_ptr, uint16_t *dst_ptr) {
    uint32_t idx;
    uint32_t run_len;
    uint16_t *out_ptr;
    uint16_t last_len;
    uint16_t len;

    out_ptr  = dst_ptr;
    last_len = (uint16_t) (codes_ptr[0] >> 15u);
    run_len  = 1u;
    for (idx = 1u; idx < num_codes; idx++) {
        len = (uint16_t) (codes_ptr[idx] >> 15u);
        if (len == last_len) {
            run_len++;
            continue;
        }
        out_ptr  = hw_write_rl(out_ptr, last_len, run_len, counts_ptr);
        last_len = len;
        run_len  = 1;
    }
    out_ptr  = hw_write_rl(out_ptr, last_len, run_len, counts_ptr);

    return (uint32_t) (out_ptr - dst_ptr);
}

/**
 * @brief Helper for creating deflate header
 * @todo
 *
 */
static void hw_bitbuf_2_write(bitbuf2 *bb_ptr, uint64_t code, uint32_t code_len) {
    uint32_t bit_count;
    uint32_t bytes;
    uint32_t bits;

    bit_count = bb_ptr->bit_count;
    code <<= bit_count;
    bb_ptr->bits |= code;
    bit_count += code_len;
    *(uint64_t *) (bb_ptr->out_buf) = bb_ptr->bits;
    bytes = bit_count >> 3u;
    bb_ptr->out_buf += bytes;
    bits = bytes * 8u;
    bb_ptr->bit_count = bit_count - bits;
    bb_ptr->bits >>= bits;
}

/**
 * @brief Helper for creating deflate header
 * @todo
 */
static uint32_t hw_bitbuf_2_bits_written(bitbuf2 *bb_ptr) {
    return (8u * (uint32_t) (bb_ptr->out_buf - bb_ptr->start_buf) + bb_ptr->bit_count);
}

/**
 * @brief Helper for creating deflate header
 * @todo
 */
static void hw_bitbuf_2_flush(bitbuf2 *bb_ptr) {
    uint32_t bit_count;
    uint32_t bytes;

    bit_count = bb_ptr->bit_count;
    if (bit_count != 0u) {
        bytes = (bit_count + 7u) / 8u;
        *(uint64_t *) (bb_ptr->out_buf) = bb_ptr->bits;
        bb_ptr->out_buf += bytes;
        bb_ptr->bit_count               = 0u;
        bb_ptr->bits                    = 0u;
    }
}

/**
 * @todo
 */
static const uint32_t cl_perm[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

/**
 * @brief helper to create deflate header
 * @todo
 */
uint32_t hw_create_header(bitbuf2 *bb_ptr,
                          uint32_t *ll_codes_ptr,
                          uint32_t num_ll_codes,
                          uint32_t *d_codes_ptr,
                          uint32_t num_d_codes,
                          uint32_t end_of_block) {
    uint32_t       cl_counts[19];
    uint32_t       idx;
    uint32_t       num_cl_tokens;
    uint32_t       max_cl_code;
    uint32_t       code;
    uint32_t       bit_count;
    uint16_t       cl_tokens[286u + 30u];
    uint16_t       token;
    uint16_t       len;
    uint32_t       bl_count[MAX_CODE_LEN + 1u];
    uint32_t       cl_codes[19];
    uint64_t       data;
    const uint32_t extra_bits[3] = {2, 3, 7};

    avx512_qplc_zero_8u((uint8_t *) cl_counts, sizeof(cl_counts));
    num_cl_tokens = hw_rl_encode(ll_codes_ptr, num_ll_codes, cl_counts, cl_tokens);
    num_cl_tokens += hw_rl_encode(d_codes_ptr, num_d_codes, cl_counts, cl_tokens + num_cl_tokens);

    hw_create_huff_tree(cl_counts, 19u, bl_count, cl_codes, MAX_BL_CODE_LEN);

    uint32_t next_code[MAX_CODE_LEN + 1u];

    code = bl_count[0] = 0u;

    for (idx = 1u; idx <= MAX_BL_CODE_LEN; idx++) {
        code = (code + bl_count[idx - 1u]) << 1u;
        next_code[idx] = code;
    }
    for (idx = 0u; idx < 19u; idx++) {
        code = cl_codes[idx];
        if (0u != code) {
            cl_codes[idx] = (code << 24u) | own_bit_reverse(next_code[code], code);
            next_code[code] += 1u;
        }
    }
    for (max_cl_code = 18u; max_cl_code >= 4u; max_cl_code--) {
        if (0 != cl_codes[cl_perm[max_cl_code]]) {
            break;
        }
    }
    // 3 <= max_cl_code <= 18  (4 <= num_cl_code <= 19)

    bit_count = hw_bitbuf_2_bits_written(bb_ptr);
    // Actually write header
    data      = (end_of_block ? 5u : 4u) |
                ((num_ll_codes - 257u) << 3u) |
                ((num_d_codes - 1u) << (3u + 5u)) |
                ((max_cl_code - 3u) << (3u + 5u + 5u));
    // Write the first CL code here, because bitbuf2Write can only safely write up to 56 bits
    data |= (cl_codes[cl_perm[0]] >> 24u) << (3u + 5u + 5u + 4u);
    hw_bitbuf_2_write(bb_ptr, data, 3u + 5u + 5u + 4u + 3u);
    data     = 0u;
    for (idx = max_cl_code; idx >= 1u; idx--) {
        data = (data << 3u) | (cl_codes[cl_perm[idx]] >> 24u);
    }
    hw_bitbuf_2_write(bb_ptr, data, 3u * (max_cl_code));
    for (idx  = 0u; idx < num_cl_tokens; idx++) {
        token = cl_tokens[idx];
        len   = token & 0x1Fu;
        code  = cl_codes[len];
        hw_bitbuf_2_write(bb_ptr, code & 0xFFFFu, code >> 24u);
        if (len > 15u) {
            hw_bitbuf_2_write(bb_ptr, token >> 8u, extra_bits[len - 16]);
        }
    }
    bit_count = hw_bitbuf_2_bits_written(bb_ptr) - bit_count;

    return bit_count;
}

/**
 * @brief Creates a compression Huffman table and deflate header from collected statistics
 *
 * @param[out]  ll_codes_ptr   pointer to literal/length codes
 * @param[out]  d_codes_ptr    pointer to distance codes
 * @param[in]   out_accum_ptr  pointer to AECS output accumulator
 * @param[in]   oa_size        output accumulator size
 * @param[in]   oa_valid_bits  number of valib bits in output accumulator
 * @param[in]   ll_hist_ptr    pointer to literal/length histogram
 * @param[in]   d_hist_ptr     pointer to distances histogram
 *
 * @return Number of valid bits in output accumulator
 *
 */
uint32_t hw_create_huff_tables(uint32_t *ll_codes_ptr,
                               uint32_t *d_codes_ptr,
                               uint8_t *out_accum_ptr,
                               uint32_t oa_size,
                               uint32_t oa_valid_bits,
                               uint32_t *ll_hist_ptr,
                               uint32_t *d_hist_ptr) {
    uint32_t       bl_count[MAX_CODE_LEN + 1u];
    uint32_t       max_ll_code;
    uint32_t       max_d_code;
    uint32_t       header_bits;
    uint32_t       bit_off;
    uint32_t       byte_off;
    const uint32_t eob = 0u;
    bitbuf2        bb_ptr;
    uint32_t       excess;

    // Make sure EOB is present
    if (ll_hist_ptr[256] == 0u) {
        ll_hist_ptr[256] = 1u;
    }

    hw_create_huff_tree(ll_hist_ptr, 286u, bl_count, ll_codes_ptr, MAX_CODE_LEN);
    hw_compute_codes(ll_codes_ptr, 286u, bl_count, MAX_CODE_LEN, &max_ll_code);
    hw_create_huff_tree(d_hist_ptr, 30u, bl_count, d_codes_ptr, MAX_CODE_LEN);
    hw_compute_codes(d_codes_ptr, 30u, bl_count, MAX_CODE_LEN, &max_d_code);

    bit_off  = oa_valid_bits & 7u;
    byte_off = oa_valid_bits >> 3u;
    out_accum_ptr += byte_off;
    oa_size -= byte_off;
    // Excess bits
    excess   = bit_off & ~7u;

    bb_ptr.bits      = (*(uint64_t *) out_accum_ptr >> excess) & ((1u << (bit_off & 7u)) - 1u);
    bb_ptr.bit_count = bit_off - excess;
    excess /= 8u;
    bb_ptr.start_buf = bb_ptr.out_buf = out_accum_ptr + excess;

    // Safe to write a qword if out_buf < end_buf;
    bb_ptr.end_buf = out_accum_ptr + oa_size - BITBUF2_BUFFER_SLOP;

    header_bits = hw_create_header(&bb_ptr, ll_codes_ptr, max_ll_code + 1, d_codes_ptr, max_d_code + 1u, eob);
    hw_bitbuf_2_flush(&bb_ptr);

    return oa_valid_bits + header_bits;
}

/**
 * @brief Creates compression Huffman table from collected statistics
 *
 * @param[out]  ll_codes_ptr  pointer to literal/length codes
 * @param[in]   ll_hist_ptr   pointer to literal/length histogram
 *
 */
void hw_create_huff_tables_no_hdr(uint32_t *ll_codes_ptr, uint32_t *ll_hist_ptr) {
    uint32_t bl_count[MAX_CODE_LEN + 1u];

    hw_create_huff_tree(ll_hist_ptr, 286u, bl_count, ll_codes_ptr, MAX_CODE_LEN);
    hw_compute_codes(ll_codes_ptr, 286u, bl_count, MAX_CODE_LEN, NULL);
}

/** @} */
