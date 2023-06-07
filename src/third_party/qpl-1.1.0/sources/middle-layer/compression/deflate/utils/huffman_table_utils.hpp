/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#ifndef QPL_MIDDLE_LAYER_COMPRESSION_UTILS_HUFFMAN_TABLE_UTILS_HPP
#define QPL_MIDDLE_LAYER_COMPRESSION_UTILS_HUFFMAN_TABLE_UTILS_HPP

#include "huff_codes.h"
#include "bitbuf2.h"

namespace qpl::ml::compression {
/**
 * @brief Determines the code each element of a deflate compliant huffman tree and stores
 * it in a lookup table
 * @requires table has been initialized to already contain the code length for each element.
 * @param table: A lookup table used to store the codes.
 * @param table_length: The length of table.
 * @param count: a histogram representing the number of occurences of codes of a given length
 */
auto set_huffman_codes(huff_code *huff_code_table,
                       int table_length,
                       uint32_t *count) noexcept -> uint32_t;

// on input, codes contain the code lengths
// on output, code contains:
// 23:16 code length
// 15:0  code value in low order bits
// returns max code value
auto set_dist_huff_codes(huff_code *codes, uint32_t * bl_count) noexcept -> uint32_t;

/**
 * @brief Creates the header for run length encoded huffman trees.
 * @param header: the output header.
 * @param lookup_table: a huffman lookup table.
 * @param huffman_rep: a run length encoded huffman tree.
 * @extra_bits: extra bits associated with the corresponding spot in huffman_rep
 * @param huffman_rep_length: the length of huffman_rep.
 * @param end_of_block: Value determining whether end of block header is produced or not;
 * 0 corresponds to not end of block and all other inputs correspond to end of block.
 * @param hclen: Length of huffman code for huffman codes minus 4.
 * @param hlit: Length of literal/length table minus 257.
 * @parm hdist: Length of distance table minus 1.
 */
auto create_huffman_header(BitBuf2 *header_bitbuf,
                           huff_code *lookup_table,
                           rl_code *huffman_rep,
                           uint16_t huffman_rep_length,
                           uint32_t end_of_block,
                           uint32_t hclen,
                           uint32_t hlit,
                           uint32_t hdist) noexcept -> int;
/**
 * @brief Creates the dynamic huffman deflate header.
 * @returns Returns the  length of header in bits.
 * @requires This function requires header is large enough to store the whole header.
 * @param header: The output header.
 * @param lit_huff_table: A literal/length code huffman lookup table.\
 * @param dist_huff_table: A distance huffman code lookup table.
 * @param end_of_block: Value determining whether end of block header is produced or not;
 * 0 corresponds to not end of block and all other inputs correspond to end of block.
 */
auto create_header(BitBuf2 *header_bitbuf,
                   rl_code *huffman_rep,
                   uint32_t length,
                   uint64_t * histogram,
                   uint32_t hlit,
                   uint32_t hdist,
                   uint32_t end_of_block) noexcept -> int;

/* Init heap with the histogram, and return the histogram size */
auto init_heap32(heap_tree *heap_space,
                 uint32_t * histogram,
                 uint32_t hist_size) noexcept -> uint32_t;

// convert codes into run-length symbols, write symbols into OUT
// generate histogram into COUNTS (assumed to be initialized to 0)
// Format of OUT:
// 4:0  code (0...18)
// 15:8 Extra bits (0...127)
// returns number of symbols in out
auto rl_encode(uint16_t * codes,
               uint32_t num_codes,
               uint64_t * counts,
               rl_code *out) noexcept -> uint32_t;

void generate_huffman_codes(heap_tree *heap_space,
                            uint32_t heap_size,
                            uint32_t *bl_count,
                            huff_code *codes,
                            uint32_t codes_count,
                            uint32_t max_code_len) noexcept;

} // namespace qpl::ml::compression

#endif // QPL_MIDDLE_LAYER_COMPRESSION_UTILS_HUFFMAN_TABLE_UTILS_HPP
