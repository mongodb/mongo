/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#ifndef HUFF_CODES_H
#define HUFF_CODES_H

#include <stdint.h>
#include <string.h>
#include <assert.h>
#include "igzip_lib.h"
#include "bitbuf2.h"

#ifdef QPL_LIB
#ifdef __cplusplus
extern "C" {
#endif
#endif

#if __x86_64__  || __i386__ || _M_X64 || _M_IX86
# include <immintrin.h>
#ifdef _MSC_VER
# include <intrin.h>
#else
# include <x86intrin.h>
#endif
#endif //__x86_64__  || __i386__ || _M_X64 || _M_IX86

#define LIT_LEN ISAL_DEF_LIT_LEN_SYMBOLS
#define DIST_LEN ISAL_DEF_DIST_SYMBOLS
#define CODE_LEN_CODES 19
#define HUFF_LEN 19
#ifdef LONGER_HUFFTABLE
# define DCODE_OFFSET 26
#else
# define DCODE_OFFSET 0
#endif
#define DYN_HDR_START_LEN 17
#define MAX_HISTHEAP_SIZE LIT_LEN
#define MAX_HUFF_TREE_DEPTH 15
#define D      IGZIP_HIST_SIZE	/* Amount of history */

#define MAX_DEFLATE_CODE_LEN 15
#define MAX_SAFE_LIT_CODE_LEN 13
#define MAX_SAFE_DIST_CODE_LEN 12

#define LONG_DIST_TABLE_SIZE 8192
#define SHORT_DIST_TABLE_SIZE 2
#define LEN_TABLE_SIZE 256
#define LIT_TABLE_SIZE 257
#define LAST_BLOCK 1

#define LEN_EXTRA_BITS_START 264
#define LEN_EXTRA_BITS_INTERVAL 4
#define DIST_EXTRA_BITS_START 3
#define DIST_EXTRA_BITS_INTERVAL 2

#define INVALID_LIT_LEN_HUFFCODE 1
#define INVALID_DIST_HUFFCODE 1
#define INVALID_HUFFCODE 1

#define HASH8K_HASH_MASK  (IGZIP_HASH8K_HASH_SIZE - 1)
#define HASH_HIST_HASH_MASK  (IGZIP_HASH_HIST_SIZE - 1)
#define HASH_MAP_HASH_MASK  (IGZIP_HASH_MAP_HASH_SIZE - 1)

#define LVL0_HASH_MASK  (IGZIP_LVL0_HASH_SIZE - 1)
#define LVL1_HASH_MASK  (IGZIP_LVL1_HASH_SIZE - 1)
#define LVL2_HASH_MASK  (IGZIP_LVL2_HASH_SIZE - 1)
#define LVL3_HASH_MASK  (IGZIP_LVL3_HASH_SIZE - 1)
#define SHORTEST_MATCH  4

#define LENGTH_BITS 5
#define FREQ_SHIFT 16
#define FREQ_MASK_HI (0xFFFFFFFFFFFF0000)
#define DEPTH_SHIFT 24
#define DEPTH_MASK 0x7F
#define DEPTH_MASK_HI (DEPTH_MASK << DEPTH_SHIFT)
#define DEPTH_1       (1 << DEPTH_SHIFT)
#define HEAP_TREE_SIZE (3*MAX_HISTHEAP_SIZE + 1)
#define HEAP_TREE_NODE_START (HEAP_TREE_SIZE-1)
#define MAX_BL_CODE_LEN 7

/**
 * @brief Structure used to store huffman codes
 */
struct huff_code {
	union {

                struct {
                        uint32_t code_and_extra:24;
                        uint32_t length2:8;
                };

		struct {
			uint16_t code;
			uint8_t extra_bit_count;
			uint8_t length;
		};

		uint32_t code_and_length;
	};
};

struct tree_node {
	uint32_t child;
	uint32_t depth;
};

struct heap_tree {
	union {
		uint64_t heap[HEAP_TREE_SIZE];
		uint64_t code_len_count[MAX_HUFF_TREE_DEPTH + 1];
		struct tree_node tree[HEAP_TREE_SIZE];
	};
};

struct rl_code {
	uint8_t code;
	uint8_t extra_bits;
};

struct hufftables_icf {
	union {
		struct {
			struct huff_code dist_table[31];
			struct huff_code lit_len_table[513];
		};

		struct {
			struct huff_code dist_lit_table[288];
			struct huff_code len_table[256];
		};
	};
};

/**
 * @brief Creates a representation of the huffman code from a histogram used to
 * decompress the intermediate compression format.
 *
 * @param bb: bitbuf structure where the header huffman code header is written
 * @param hufftables: output huffman code representation
 * @param hist: histogram used to generat huffman code
 * @param end_of_block: flag whether this is the final huffman code
 *
 * @returns Returns the length in bits of the block with histogram hist encoded
 * with the set hufftable
 */
uint64_t
create_hufftables_icf(struct BitBuf2 *bb, struct hufftables_icf * hufftables,
                  struct isal_mod_hist *hist, uint32_t end_of_block);
#ifdef QPL_LIB
#ifdef __cplusplus
} // extern "C"
#endif
#endif

#endif
