/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#include "huffman_table.hpp"

#include "common/bit_reverse.hpp"
#include "compression/deflate/utils/fixed_huffman_table.hpp"
#include "compression/deflate/utils/compression_defs.hpp"
#include "compression/deflate/utils/huffman_table_utils.hpp"
#include "util/memory.hpp"

#include "qplc_compression_consts.h"

#include "flatten_ll.h"

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstack-usage=4096"
#endif

namespace qpl::ml::compression {
huffman_table_icf::huffman_table_icf(hufftables_icf *huffman_table_ptr) noexcept
        : huffman_table_(huffman_table_ptr) {

}

void huffman_table_icf::init_isal_huffman_tables(hufftables_icf *huffman_table_ptr) noexcept {
    huffman_table_ = huffman_table_ptr;
}

auto huffman_table_icf::get_isal_huffman_tables() const noexcept -> hufftables_icf * {
    return huffman_table_;
}

void huffman_table_icf::expand_huffman_tables() noexcept {
    uint32_t  i    = 0;
    uint32_t  len  = 0;
    uint32_t  code = 0;
    huff_code *p_code;

    huff_code *lit_len_codes = get_isal_huffman_tables()->lit_len_table;
    huff_code *dist_codes    = get_isal_huffman_tables()->dist_table;

    std::array<huff_code, number_of_length_codes> length_codes;

    for (uint32_t i = 0; i < number_of_length_codes; i++) {
        length_codes[i] = get_isal_huffman_tables()->lit_len_table[i + 265];
    }

    p_code = &lit_len_codes[265];

    i = 0;
    for (uint32_t eb = 1; eb < 6; eb++) {
        for (uint32_t k = 0; k < 4; k++) {
            len  = length_codes[i].length;
            code = length_codes[i++].code;
            for (uint32_t j = 0; j < (1u << eb); j++) {
                p_code->code_and_extra = code | (j << len);
                p_code->length         = len + eb;
                p_code++;
            }
        }        // end for k
    }            // end for eb
    // fix up last record
    p_code[-1] = length_codes[i];

    dist_codes[DIST_LEN].code_and_extra = 0;
    dist_codes[DIST_LEN].length         = 0;
}

void build_huffman_table_icf(huffman_table_icf &huffman_table, isal_mod_hist *histogram) noexcept {
    uint32_t  heap_size = 0;
    heap_tree heap_space;

    uint32_t bl_count[MAX_DEFLATE_CODE_LEN + 1];

    huff_code *ll_codes     = huffman_table.get_isal_huffman_tables()->lit_len_table;
    huff_code *d_codes      = huffman_table.get_isal_huffman_tables()->dist_table;
    uint32_t  *ll_histogram = histogram->ll_hist;
    uint32_t  *d_histogram  = histogram->d_hist;

    heap_size = init_heap32(&heap_space, ll_histogram, LIT_LEN);
    generate_huffman_codes(&heap_space, heap_size, bl_count, ll_codes, LIT_LEN, MAX_DEFLATE_CODE_LEN);
    huffman_table.max_ll_code_index_ = set_huffman_codes(ll_codes, LIT_LEN, bl_count);

    heap_size = init_heap32(&heap_space, d_histogram, DIST_LEN);
    generate_huffman_codes(&heap_space, heap_size, bl_count, d_codes, DIST_LEN, MAX_DEFLATE_CODE_LEN);
    huffman_table.max_d_code_index_ = set_dist_huff_codes(d_codes, bl_count);
}

auto write_huffman_table_icf(BitBuf2 *bit_buffer,
                             huffman_table_icf &huffman_table,
                             isal_mod_hist *histogram,
                             compression_mode_t compression_mode,
                             uint32_t end_of_block) noexcept -> uint64_t {
    uint32_t num_cl_tokens        = 0;
    uint32_t i                    = 0;
    rl_code  cl_tokens[LIT_LEN + DIST_LEN];
    uint64_t cl_counts[CODE_LEN_CODES];
    uint16_t combined_table[LIT_LEN + DIST_LEN];
    uint64_t compressed_len       = 0;
    uint64_t fixed_compressed_len = 3;    /* The fixed header size */

    huff_code *ll_codes       = huffman_table.get_isal_huffman_tables()->lit_len_table;
    huff_code *d_codes        = huffman_table.get_isal_huffman_tables()->dist_table;
    uint32_t  *ll_histogram   = histogram->ll_hist;
    uint32_t  *d_histogram    = histogram->d_hist;
    huff_code *fixed_ll_codes = fixed_hufftables.lit_len_table;
    huff_code *fixed_d_codes  = fixed_hufftables.dist_table;

    /* Run length encode the length and distance huffman codes */
    memset(cl_counts, 0, sizeof(cl_counts));

    if (compression_mode != fixed_mode) {
        for (i = 0; i < QPLC_DEFLATE_LITERALS_COUNT; i++) {
            combined_table[i] = ll_codes[i].length;
            compressed_len += ll_codes[i].length * ll_histogram[i];
            fixed_compressed_len += fixed_ll_codes[i].length * ll_histogram[i];
        }

        for (; i < huffman_table.max_ll_code_index_ + 1; i++) {
            combined_table[i] = ll_codes[i].length;
            compressed_len += (ll_codes[i].length + length_code_extra_bits[i - 257]) * ll_histogram[i];
            fixed_compressed_len += (fixed_ll_codes[i].length + length_code_extra_bits[i - 257]) * ll_histogram[i];
        }

        for (i = 0; i < huffman_table.max_d_code_index_ + 1; i++) {
            combined_table[i + huffman_table.max_ll_code_index_ + 1] = d_codes[i].length;
            compressed_len += (d_codes[i].length + distance_code_extra_bits[i]) * d_histogram[i];
            fixed_compressed_len += (fixed_d_codes[i].length + distance_code_extra_bits[i]) * d_histogram[i];
        }
    } else {
        huffman_table.max_ll_code_index_ = max_ll_code_index;
        huffman_table.max_d_code_index_  = max_d_code_index;
        for (i = 0; i <= end_of_block_code_index; i++) {
            fixed_compressed_len += fixed_ll_codes[i].length * ll_histogram[i];
        }

        for (; i < huffman_table.max_ll_code_index_ + 1; i++) {
            fixed_compressed_len += (fixed_ll_codes[i].length + length_code_extra_bits[i - 257]) * ll_histogram[i];
        }

        for (i = 0; i < huffman_table.max_d_code_index_ + 1; i++) {
            fixed_compressed_len += (fixed_d_codes[i].length + distance_code_extra_bits[i]) * d_histogram[i];
        }
    }

    if (fixed_compressed_len > compressed_len && compression_mode != fixed_mode) {
        // The following code calls rle encoding function twice, so literal/length and distance
        // Code lengths will be encoded independently and distance length codes won't span literal/lengths.

        // Perform RLE for literal/length length codes
        num_cl_tokens = rl_encode(combined_table,
                                  huffman_table.max_ll_code_index_ + 1,
                                  cl_counts,
                                  cl_tokens);

        // Perform RLE for distance length codes
        num_cl_tokens += rl_encode(combined_table + huffman_table.max_ll_code_index_ + 1,
                                   huffman_table.max_d_code_index_ + 1,
                                   cl_counts,
                                   cl_tokens + num_cl_tokens);

        /* Create header */
        create_header(bit_buffer,
                      cl_tokens,
                      num_cl_tokens,
                      cl_counts,
                      huffman_table.max_ll_code_index_ - end_of_block_code_index,
                      huffman_table.max_d_code_index_,
                      end_of_block);

        compressed_len += 8 * buffer_used(bit_buffer) + bit_buffer->m_bit_count;
    } else {
        /* Substitute in fixed block since it creates smaller block or fixed mode enabled */
        auto *fixed_hufftables_ptr = reinterpret_cast<uint8_t *>(&fixed_hufftables);
        util::copy(fixed_hufftables_ptr,
                   fixed_hufftables_ptr + sizeof(hufftables_icf),
                   reinterpret_cast<uint8_t *>(huffman_table.get_isal_huffman_tables()));
        end_of_block = end_of_block ? 1 : 0;
        write_bits(bit_buffer, 0x2 | end_of_block, 3);
        compressed_len = fixed_compressed_len;
    }

    return compressed_len;
}

void prepare_histogram(isal_mod_hist *histogram) noexcept {
    flatten_ll(histogram->ll_hist);

    // make sure EOB is present
    if (histogram->ll_hist[end_of_block_code_index] == 0) {
        histogram->ll_hist[end_of_block_code_index] = 1;
    }
}

} // namespace qpl::ml::compression

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

