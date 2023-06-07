/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#include <stdint.h>
#include "igzip_lib.h"
#include "crc.h"
#include "huff_codes.h"
#include "igzip_checksums.h"
#include "igzip_wrapper.h"
#include "unaligned.h"

#ifndef NO_STATIC_INFLATE_H
#include "static_inflate.h"
#endif

#ifdef __FreeBSD__
#include <sys/types.h>
#include <sys/endian.h>
# define bswap_32(x) bswap32(x)
#elif defined (__APPLE__)
#include <libkern/OSByteOrder.h>
# define bswap_32(x) OSSwapInt32(x)
#elif defined (__GNUC__) && !defined (__MINGW32__)
# include <byteswap.h>
#elif defined _WIN64
# define bswap_32(x) _byteswap_ulong(x)
#endif

extern int decode_huffman_code_block_stateless(struct inflate_state *, uint8_t * start_out);
extern struct isal_hufftables hufftables_default;	/* For known header detection */

#define LARGE_SHORT_SYM_LEN 25
#define LARGE_SHORT_SYM_MASK ((1 << LARGE_SHORT_SYM_LEN) - 1)
#define LARGE_LONG_SYM_LEN 10
#define LARGE_LONG_SYM_MASK ((1 << LARGE_LONG_SYM_LEN) - 1)
#define LARGE_SHORT_CODE_LEN_OFFSET 28
#define LARGE_LONG_CODE_LEN_OFFSET 10
#define LARGE_FLAG_BIT_OFFSET 25
#define LARGE_FLAG_BIT (1 << LARGE_FLAG_BIT_OFFSET)
#define LARGE_SYM_COUNT_OFFSET 26
#define LARGE_SYM_COUNT_LEN 2
#define LARGE_SYM_COUNT_MASK ((1 << LARGE_SYM_COUNT_LEN) - 1)
#define LARGE_SHORT_MAX_LEN_OFFSET 26

#define SMALL_SHORT_SYM_LEN 9
#define SMALL_SHORT_SYM_MASK ((1 << SMALL_SHORT_SYM_LEN) - 1)
#define SMALL_LONG_SYM_LEN 9
#define SMALL_LONG_SYM_MASK ((1 << SMALL_LONG_SYM_LEN) - 1)
#define SMALL_SHORT_CODE_LEN_OFFSET 11
#define SMALL_LONG_CODE_LEN_OFFSET 10
#define SMALL_FLAG_BIT_OFFSET 10
#define SMALL_FLAG_BIT (1 << SMALL_FLAG_BIT_OFFSET)

#define DIST_SYM_OFFSET 0
#define DIST_SYM_LEN 5
#define DIST_SYM_MASK ((1 << DIST_SYM_LEN) - 1)
#define DIST_SYM_EXTRA_OFFSET 5
#define DIST_SYM_EXTRA_LEN 4
#define DIST_SYM_EXTRA_MASK ((1 << DIST_SYM_EXTRA_LEN) - 1)

#define MAX_LIT_LEN_CODE_LEN 21
#define MAX_LIT_LEN_COUNT (MAX_LIT_LEN_CODE_LEN + 2)
#define MAX_LIT_LEN_SYM 512
#define LIT_LEN_ELEMS 514

#define INVALID_SYMBOL 0x1FFF
#define INVALID_CODE 0xFFFFFF

#define MIN_DEF_MATCH 3

#define TRIPLE_SYM_FLAG 0
#define DOUBLE_SYM_FLAG TRIPLE_SYM_FLAG + 1
#define SINGLE_SYM_FLAG DOUBLE_SYM_FLAG + 1
#define DEFAULT_SYM_FLAG TRIPLE_SYM_FLAG

#define SINGLE_SYM_THRESH (2 * 1024)
#define DOUBLE_SYM_THRESH (4 * 1024)

/* Avoid getting warnings on unused variables which might be used later */
#define MAYBE_UNUSED(x) ((void)x)

/* structure contain lookup data based on RFC 1951 */
struct rfc1951_tables {
	uint8_t dist_extra_bit_count[32];
	uint32_t dist_start[32];
	uint8_t len_extra_bit_count[32];
	uint16_t len_start[32];

};

/* The following tables are based on the tables in the deflate standard,
 * RFC 1951 page 11. */
static struct rfc1951_tables rfc_lookup_table = {
	.dist_extra_bit_count = {
				 0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0x02, 0x02,
				 0x03, 0x03, 0x04, 0x04, 0x05, 0x05, 0x06, 0x06,
				 0x07, 0x07, 0x08, 0x08, 0x09, 0x09, 0x0a, 0x0a,
				 0x0b, 0x0b, 0x0c, 0x0c, 0x0d, 0x0d, 0x00, 0x00},

	.dist_start = {
		       0x0001, 0x0002, 0x0003, 0x0004, 0x0005, 0x0007, 0x0009, 0x000d,
		       0x0011, 0x0019, 0x0021, 0x0031, 0x0041, 0x0061, 0x0081, 0x00c1,
		       0x0101, 0x0181, 0x0201, 0x0301, 0x0401, 0x0601, 0x0801, 0x0c01,
		       0x1001, 0x1801, 0x2001, 0x3001, 0x4001, 0x6001, 0x0000, 0x0000},

	.len_extra_bit_count = {
				0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
				0x01, 0x01, 0x01, 0x01, 0x02, 0x02, 0x02, 0x02,
				0x03, 0x03, 0x03, 0x03, 0x04, 0x04, 0x04, 0x04,
				0x05, 0x05, 0x05, 0x05, 0x00, 0x00, 0x00, 0x00},

	.len_start = {
		      0x0003, 0x0004, 0x0005, 0x0006, 0x0007, 0x0008, 0x0009, 0x000a,
		      0x000b, 0x000d, 0x000f, 0x0011, 0x0013, 0x0017, 0x001b, 0x001f,
		      0x0023, 0x002b, 0x0033, 0x003b, 0x0043, 0x0053, 0x0063, 0x0073,
		      0x0083, 0x00a3, 0x00c3, 0x00e3, 0x0102, 0x0103, 0x0000, 0x0000}
};

struct slver {
	uint16_t snum;
	uint8_t ver;
	uint8_t core;
};

/* Version info */
struct slver isal_inflate_init_slver_00010088;
struct slver isal_inflate_init_slver = { 0x0088, 0x01, 0x00 };

struct slver isal_inflate_reset_slver_0001008f;
struct slver isal_inflate_reset_slver = { 0x008f, 0x01, 0x00 };

struct slver isal_inflate_stateless_slver_00010089;
struct slver isal_inflate_stateless_slver = { 0x0089, 0x01, 0x00 };

struct slver isal_inflate_slver_0001008a;
struct slver isal_inflate_slver = { 0x008a, 0x01, 0x00 };

struct slver isal_inflate_set_dict_slver_0001008d;
struct slver isal_inflate_set_dict_slver = { 0x008d, 0x01, 0x00 };

/*Performs a copy of length repeat_length data starting at dest -
 * lookback_distance into dest. This copy copies data previously copied when the
 * src buffer and the dest buffer overlap. */
static inline void byte_copy(uint8_t * dest, uint64_t lookback_distance, int repeat_length)
{
	uint8_t *src = dest - lookback_distance;

	for (; repeat_length > 0; repeat_length--)
		*dest++ = *src++;
}

static void update_checksum(struct inflate_state *state, uint8_t * start_in, uint64_t length)
{
	switch (state->crc_flag) {
	case ISAL_GZIP:
	case ISAL_GZIP_NO_HDR:
	case ISAL_GZIP_NO_HDR_VER:
		state->crc = crc32_gzip_refl(state->crc, start_in, length);
		break;
	case ISAL_ZLIB:
	case ISAL_ZLIB_NO_HDR:
	case ISAL_ZLIB_NO_HDR_VER:
		state->crc = isal_adler32_bam1(state->crc, start_in, length);
		break;
	}
}

static void finalize_adler32(struct inflate_state *state)
{

	state->crc = (state->crc & 0xffff0000) | (((state->crc & 0xffff) + 1) % ADLER_MOD);
}

static const uint8_t bitrev_table[] = {
	0x00, 0x80, 0x40, 0xc0, 0x20, 0xa0, 0x60, 0xe0,
	0x10, 0x90, 0x50, 0xd0, 0x30, 0xb0, 0x70, 0xf0,
	0x08, 0x88, 0x48, 0xc8, 0x28, 0xa8, 0x68, 0xe8,
	0x18, 0x98, 0x58, 0xd8, 0x38, 0xb8, 0x78, 0xf8,
	0x04, 0x84, 0x44, 0xc4, 0x24, 0xa4, 0x64, 0xe4,
	0x14, 0x94, 0x54, 0xd4, 0x34, 0xb4, 0x74, 0xf4,
	0x0c, 0x8c, 0x4c, 0xcc, 0x2c, 0xac, 0x6c, 0xec,
	0x1c, 0x9c, 0x5c, 0xdc, 0x3c, 0xbc, 0x7c, 0xfc,
	0x02, 0x82, 0x42, 0xc2, 0x22, 0xa2, 0x62, 0xe2,
	0x12, 0x92, 0x52, 0xd2, 0x32, 0xb2, 0x72, 0xf2,
	0x0a, 0x8a, 0x4a, 0xca, 0x2a, 0xaa, 0x6a, 0xea,
	0x1a, 0x9a, 0x5a, 0xda, 0x3a, 0xba, 0x7a, 0xfa,
	0x06, 0x86, 0x46, 0xc6, 0x26, 0xa6, 0x66, 0xe6,
	0x16, 0x96, 0x56, 0xd6, 0x36, 0xb6, 0x76, 0xf6,
	0x0e, 0x8e, 0x4e, 0xce, 0x2e, 0xae, 0x6e, 0xee,
	0x1e, 0x9e, 0x5e, 0xde, 0x3e, 0xbe, 0x7e, 0xfe,
	0x01, 0x81, 0x41, 0xc1, 0x21, 0xa1, 0x61, 0xe1,
	0x11, 0x91, 0x51, 0xd1, 0x31, 0xb1, 0x71, 0xf1,
	0x09, 0x89, 0x49, 0xc9, 0x29, 0xa9, 0x69, 0xe9,
	0x19, 0x99, 0x59, 0xd9, 0x39, 0xb9, 0x79, 0xf9,
	0x05, 0x85, 0x45, 0xc5, 0x25, 0xa5, 0x65, 0xe5,
	0x15, 0x95, 0x55, 0xd5, 0x35, 0xb5, 0x75, 0xf5,
	0x0d, 0x8d, 0x4d, 0xcd, 0x2d, 0xad, 0x6d, 0xed,
	0x1d, 0x9d, 0x5d, 0xdd, 0x3d, 0xbd, 0x7d, 0xfd,
	0x03, 0x83, 0x43, 0xc3, 0x23, 0xa3, 0x63, 0xe3,
	0x13, 0x93, 0x53, 0xd3, 0x33, 0xb3, 0x73, 0xf3,
	0x0b, 0x8b, 0x4b, 0xcb, 0x2b, 0xab, 0x6b, 0xeb,
	0x1b, 0x9b, 0x5b, 0xdb, 0x3b, 0xbb, 0x7b, 0xfb,
	0x07, 0x87, 0x47, 0xc7, 0x27, 0xa7, 0x67, 0xe7,
	0x17, 0x97, 0x57, 0xd7, 0x37, 0xb7, 0x77, 0xf7,
	0x0f, 0x8f, 0x4f, 0xcf, 0x2f, 0xaf, 0x6f, 0xef,
	0x1f, 0x9f, 0x5f, 0xdf, 0x3f, 0xbf, 0x7f, 0xff,

};

/*
 * Returns integer with first length bits reversed and all higher bits zeroed
 */
static inline uint32_t bit_reverse2(uint16_t bits, uint8_t length)
{
	uint32_t bitrev;
	bitrev = bitrev_table[bits >> 8];
	bitrev |= bitrev_table[bits & 0xFF] << 8;

	return bitrev >> (16 - length);
}

/* Load data from the in_stream into a buffer to allow for handling unaligned data*/
static inline void inflate_in_load(struct inflate_state *state, int min_required)
{
	uint64_t temp = 0;
	uint8_t new_bytes;
        MAYBE_UNUSED(min_required);

	if (state->read_in_length >= 64)
		return;

	if (state->avail_in >= 8) {
		/* If there is enough space to load a 64 bits, load the data and use
		 * that to fill read_in */
		new_bytes = 8 - (state->read_in_length + 7) / 8;
		temp = load_u64(state->next_in);

		state->read_in |= temp << state->read_in_length;
		state->next_in += new_bytes;
		state->avail_in -= new_bytes;
		state->read_in_length += new_bytes * 8;

	} else {
		/* Else fill the read_in buffer 1 byte at a time */
		while (state->read_in_length < 57 && state->avail_in > 0) {
			temp = *state->next_in;
			state->read_in |= temp << state->read_in_length;
			state->next_in++;
			state->avail_in--;
			state->read_in_length += 8;

		}
	}
}

static inline uint64_t inflate_in_read_bits_unsafe(struct inflate_state *state,
						   uint8_t bit_count)
{
	uint64_t ret;

	ret = (state->read_in) & ((1 << bit_count) - 1);
	state->read_in >>= bit_count;
	state->read_in_length -= bit_count;

	return ret;
}

/* Returns the next bit_count bits from the in stream and shifts the stream over
 * by bit-count bits */
static inline uint64_t inflate_in_read_bits(struct inflate_state *state, uint8_t bit_count)
{
	/* Load inflate_in if not enough data is in the read_in buffer */
	inflate_in_load(state, bit_count);
	return inflate_in_read_bits_unsafe(state, bit_count);
}

static inline void write_huff_code(struct huff_code *huff_code, uint32_t code, uint32_t length)
{
	huff_code->code_and_length = code | length << 24;
}

static inline int set_codes(struct huff_code *huff_code_table, int table_length,
			    uint16_t * count)
{
	uint32_t max, code, length;
	uint32_t next_code[MAX_HUFF_TREE_DEPTH + 1];
	int i;
	struct huff_code *table_end = huff_code_table + table_length;

	/* Setup for calculating huffman codes */
	next_code[0] = 0;
	next_code[1] = 0;
	for (i = 2; i < MAX_HUFF_TREE_DEPTH + 1; i++)
		next_code[i] = (next_code[i - 1] + count[i - 1]) << 1;

	max = (next_code[MAX_HUFF_TREE_DEPTH] + count[MAX_HUFF_TREE_DEPTH]);

	if (max > (1 << MAX_HUFF_TREE_DEPTH))
#if defined(QPL_LIB)
        return QPL_AD_ERROR_CODE_BAD_DIST_CODE_LEN;
#else
        return ISAL_INVALID_BLOCK;
#endif

	/* Calculate code corresponding to a given symbol */
	for (; huff_code_table < table_end; huff_code_table++) {
		length = huff_code_table->length;
		if (length == 0)
			continue;

		code = bit_reverse2(next_code[length], length);

		write_huff_code(huff_code_table, code, length);
		next_code[length] += 1;
	}
	return 0;
}

static inline int set_and_expand_lit_len_huffcode(struct huff_code *lit_len_huff,
						  uint32_t table_length,
						  uint16_t * count,
						  uint16_t * expand_count,
						  uint32_t * code_list)
{
	int len_sym, len_size, extra_count, extra;
	uint32_t count_total, count_tmp;
	uint32_t code, code_len, expand_len;
	struct huff_code *expand_next = &lit_len_huff[ISAL_DEF_LIT_SYMBOLS];
	struct huff_code tmp_table[LIT_LEN - ISAL_DEF_LIT_SYMBOLS];
	uint32_t max;
	uint32_t next_code[MAX_HUFF_TREE_DEPTH + 1];
	int i;
	struct huff_code *table_end;
	struct huff_code *huff_code_table = lit_len_huff;
	uint32_t insert_index;

	/* Setup for calculating huffman codes */
	count_total = 0;
	count_tmp = expand_count[1];
	next_code[0] = 0;
	next_code[1] = 0;
	expand_count[0] = 0;
	expand_count[1] = 0;

	for (i = 1; i < MAX_HUFF_TREE_DEPTH; i++) {
		count_total = count[i] + count_tmp + count_total;
		count_tmp = expand_count[i + 1];
		expand_count[i + 1] = count_total;
		next_code[i + 1] = (next_code[i] + count[i]) << 1;
	}

	count_tmp = count[i] + count_tmp;

	for (; i < MAX_LIT_LEN_COUNT - 1; i++) {
		count_total = count_tmp + count_total;
		count_tmp = expand_count[i + 1];
		expand_count[i + 1] = count_total;
	}

	/* Correct for extra symbols used by static header */
	if (table_length > LIT_LEN)
		count[8] -= 2;

	max = (next_code[MAX_HUFF_TREE_DEPTH] + count[MAX_HUFF_TREE_DEPTH]);

	if (max > (1 << MAX_HUFF_TREE_DEPTH))
#if defined(QPL_LIB)
        return QPL_AD_ERROR_CODE_BAD_CL_CODE_LEN;
#else
        return ISAL_INVALID_BLOCK;
#endif

	memcpy(count, expand_count, sizeof(*count) * MAX_LIT_LEN_COUNT);

	memcpy(tmp_table, &lit_len_huff[ISAL_DEF_LIT_SYMBOLS],
	       sizeof(*lit_len_huff) * (LIT_LEN - ISAL_DEF_LIT_SYMBOLS));
	memset(&lit_len_huff[ISAL_DEF_LIT_SYMBOLS], 0,
	       sizeof(*lit_len_huff) * (LIT_LEN_ELEMS - ISAL_DEF_LIT_SYMBOLS));

	/* Calculate code corresponding to a given literal symbol */
	table_end = huff_code_table + ISAL_DEF_LIT_SYMBOLS;
	for (; huff_code_table < table_end; huff_code_table++) {
		code_len = huff_code_table->length;
		if (code_len == 0)
			continue;

		code = bit_reverse2(next_code[code_len], code_len);

		insert_index = expand_count[code_len];
		code_list[insert_index] = huff_code_table - lit_len_huff;
		expand_count[code_len]++;

		write_huff_code(huff_code_table, code, code_len);
		next_code[code_len] += 1;
	}

	/* Calculate code corresponding to a given len symbol */
	for (len_sym = 0; len_sym < LIT_LEN - ISAL_DEF_LIT_SYMBOLS; len_sym++) {
		extra_count = rfc_lookup_table.len_extra_bit_count[len_sym];
		len_size = (1 << extra_count);

		code_len = tmp_table[len_sym].length;
		if (code_len == 0) {
			expand_next += len_size;
			continue;
		}

		code = bit_reverse2(next_code[code_len], code_len);
		expand_len = code_len + extra_count;
		next_code[code_len] += 1;
		insert_index = expand_count[expand_len];
		expand_count[expand_len] += len_size;

		for (extra = 0; extra < len_size; extra++) {
			code_list[insert_index] = expand_next - lit_len_huff;
			write_huff_code(expand_next, code | (extra << code_len), expand_len);
			insert_index++;
			expand_next++;
		}
	}

	return 0;
}

static inline int index_to_sym(int index)
{
	return (index != 513) ? index : 512;
}

/* Sets result to the inflate_huff_code corresponding to the huffcode defined by
 * the lengths in huff_code_table,where count is a histogram of the appearance
 * of each code length */
static void make_inflate_huff_code_lit_len(struct inflate_huff_code_large *result,
					   struct huff_code *huff_code_table,
					   uint32_t table_length, uint16_t * count_total,
					   uint32_t * code_list, uint32_t multisym)
{
	uint32_t i, j;
	uint16_t code = 0;
	uint32_t *long_code_list;
	uint32_t long_code_length = 0;
	uint16_t temp_code_list[1 << (MAX_LIT_LEN_CODE_LEN - ISAL_DECODE_LONG_BITS)];
	uint32_t temp_code_length;
	uint32_t long_code_lookup_length = 0;
	uint32_t max_length;
	uint16_t first_bits;
	uint32_t code_length;
	uint16_t long_bits;
	uint16_t min_increment;
	uint32_t code_list_len;
	uint32_t last_length, min_length;
	uint32_t copy_size;
	uint32_t *short_code_lookup = result->short_code_lookup;
	int index1, index2, index3;
	uint32_t sym1, sym2, sym3;
        int sym1_index, sym2_index, sym3_index;
	uint32_t sym1_code, sym2_code, sym3_code, sym1_len, sym2_len, sym3_len;

	uint32_t max_symbol = MAX_LIT_LEN_SYM;

        MAYBE_UNUSED(table_length);

	code_list_len = count_total[MAX_LIT_LEN_COUNT - 1];

	if (code_list_len == 0) {
		memset(result->short_code_lookup, 0, sizeof(result->short_code_lookup));
		return;
	}

	/* Determine the length of the first code */
	last_length = huff_code_table[code_list[0]].length;
	if (last_length > ISAL_DECODE_LONG_BITS)
		last_length = ISAL_DECODE_LONG_BITS + 1;
	copy_size = (1 << (last_length - 1));

	/* Initialize short_code_lookup, so invalid lookups process data */
	memset(short_code_lookup, 0x00, copy_size * sizeof(*short_code_lookup));

	min_length = last_length;
	for (; last_length <= ISAL_DECODE_LONG_BITS; last_length++) {
		/* Copy forward previosly set codes */
		memcpy(short_code_lookup + copy_size, short_code_lookup,
		       sizeof(*short_code_lookup) * copy_size);
		copy_size *= 2;

		/* Encode code singletons */
		for (index1 = count_total[last_length];
		     index1 < count_total[last_length + 1]; index1++) {
			sym1_index = code_list[index1];
			sym1 = index_to_sym(sym1_index);
			sym1_len = huff_code_table[sym1_index].length;
			sym1_code = huff_code_table[sym1_index].code;

			if (sym1 > max_symbol)
				continue;

			/* Set new codes */
			short_code_lookup[sym1_code] =
			    sym1 | sym1_len << LARGE_SHORT_CODE_LEN_OFFSET |
			    (1 << LARGE_SYM_COUNT_OFFSET);
		}

		/* Continue if no pairs are possible */
		if (multisym >= SINGLE_SYM_FLAG || last_length < 2 * min_length)
			continue;

		/* Encode code pairs */
		for (index1 = count_total[min_length];
		     index1 < count_total[last_length - min_length + 1]; index1++) {
			sym1_index = code_list[index1];
			sym1 = index_to_sym(sym1_index);
			sym1_len = huff_code_table[sym1_index].length;
			sym1_code = huff_code_table[sym1_index].code;

			/*Check that sym1 is a literal */
			if (sym1 >= 256) {
				index1 = count_total[sym1_len + 1] - 1;
				continue;
			}

			sym2_len = last_length - sym1_len;
			for (index2 = count_total[sym2_len];
			     index2 < count_total[sym2_len + 1]; index2++) {
				sym2_index = code_list[index2];
				sym2 = index_to_sym(sym2_index);

				/* Check that sym2 is an existing symbol */
				if (sym2 > max_symbol)
					break;

				sym2_code = huff_code_table[sym2_index].code;
				code = sym1_code | (sym2_code << sym1_len);
				code_length = sym1_len + sym2_len;
				short_code_lookup[code] =
				    sym1 | (sym2 << 8) |
				    (code_length << LARGE_SHORT_CODE_LEN_OFFSET)
				    | (2 << LARGE_SYM_COUNT_OFFSET);
			}
		}

		/* Continue if no triples are possible */
		if (multisym >= DOUBLE_SYM_FLAG || last_length < 3 * min_length)
			continue;

		/* Encode code triples */
		for (index1 = count_total[min_length];
		     index1 < count_total[last_length - 2 * min_length + 1]; index1++) {
			sym1_index = code_list[index1];
			sym1 = index_to_sym(sym1_index);
			sym1_len = huff_code_table[sym1_index].length;
			sym1_code = huff_code_table[sym1_index].code;
			/*Check that sym1 is a literal */
			if (sym1 >= 256) {
				index1 = count_total[sym1_len + 1] - 1;
				continue;
			}

			if (last_length - sym1_len < 2 * min_length)
				break;

			for (index2 = count_total[min_length];
			     index2 < count_total[last_length - sym1_len - min_length + 1];
			     index2++) {
				sym2_index = code_list[index2];
				sym2 = index_to_sym(sym2_index);
				sym2_len = huff_code_table[sym2_index].length;
				sym2_code = huff_code_table[sym2_index].code;

				/* Check that sym2 is a literal */
				if (sym2 >= 256) {
					index2 = count_total[sym2_len + 1] - 1;
					continue;
				}

				sym3_len = last_length - sym1_len - sym2_len;
				for (index3 = count_total[sym3_len];
				     index3 < count_total[sym3_len + 1]; index3++) {
					sym3_index = code_list[index3];
					sym3 = index_to_sym(sym3_index);
					sym3_code = huff_code_table[sym3_index].code;

					/* Check that sym3 is writable existing symbol */
					if (sym3 > max_symbol - 1)
						break;

					code = sym1_code | (sym2_code << sym1_len) |
					    (sym3_code << (sym2_len + sym1_len));
					code_length = sym1_len + sym2_len + sym3_len;
					short_code_lookup[code] =
					    sym1 | (sym2 << 8) | sym3 << 16 |
					    (code_length << LARGE_SHORT_CODE_LEN_OFFSET)
					    | (3 << LARGE_SYM_COUNT_OFFSET);

				}

			}
		}

	}

	index1 = count_total[ISAL_DECODE_LONG_BITS + 1];
	long_code_length = code_list_len - index1;
	long_code_list = &code_list[index1];
	for (i = 0; i < long_code_length; i++) {
		/*Set the look up table to point to a hint where the symbol can be found
		 * in the list of long codes and add the current symbol to the list of
		 * long codes. */
		if (huff_code_table[long_code_list[i]].code_and_extra == INVALID_CODE)
			continue;

		max_length = huff_code_table[long_code_list[i]].length;
		first_bits =
		    huff_code_table[long_code_list[i]].code_and_extra
		    & ((1 << ISAL_DECODE_LONG_BITS) - 1);

		temp_code_list[0] = long_code_list[i];
		temp_code_length = 1;

		for (j = i + 1; j < long_code_length; j++) {
			if ((huff_code_table[long_code_list[j]].code &
			     ((1 << ISAL_DECODE_LONG_BITS) - 1)) == first_bits) {
				max_length = huff_code_table[long_code_list[j]].length;
				temp_code_list[temp_code_length] = long_code_list[j];
				temp_code_length++;
			}
		}

		memset(&result->long_code_lookup[long_code_lookup_length], 0x00,
		       sizeof(*result->long_code_lookup) *
		       (1 << (max_length - ISAL_DECODE_LONG_BITS)));

		for (j = 0; j < temp_code_length; j++) {
			sym1_index = temp_code_list[j];
			sym1 = index_to_sym(sym1_index);
			sym1_len = huff_code_table[sym1_index].length;
			sym1_code = huff_code_table[sym1_index].code_and_extra;

			long_bits = sym1_code >> ISAL_DECODE_LONG_BITS;
			min_increment = 1 << (sym1_len - ISAL_DECODE_LONG_BITS);

			for (; long_bits < (1 << (max_length - ISAL_DECODE_LONG_BITS));
			     long_bits += min_increment) {
				result->long_code_lookup[long_code_lookup_length + long_bits] =
				    sym1 | (sym1_len << LARGE_LONG_CODE_LEN_OFFSET);
			}
			huff_code_table[sym1_index].code_and_extra = INVALID_CODE;

		}
		result->short_code_lookup[first_bits] = long_code_lookup_length |
		    (max_length << LARGE_SHORT_MAX_LEN_OFFSET) | LARGE_FLAG_BIT;
		long_code_lookup_length += 1 << (max_length - ISAL_DECODE_LONG_BITS);
	}
}

static inline void make_inflate_huff_code_dist(struct inflate_huff_code_small *result,
					       struct huff_code *huff_code_table,
					       uint32_t table_length, uint16_t * count,
					       uint32_t max_symbol)
{
	uint32_t i, j, k;
	uint32_t *long_code_list;
	uint32_t long_code_length = 0;
	uint16_t temp_code_list[1 << (15 - ISAL_DECODE_SHORT_BITS)];
	uint32_t temp_code_length;
	uint32_t long_code_lookup_length = 0;
	uint32_t max_length;
	uint16_t first_bits;
	uint32_t code_length;
	uint16_t long_bits;
	uint16_t min_increment;
	uint32_t code_list[DIST_LEN + 2];	/* The +2 is for the extra codes in the static header */
	uint32_t code_list_len;
	uint32_t count_total[17], count_total_tmp[17];
	uint32_t insert_index;
	uint32_t last_length;
	uint32_t copy_size;
	uint16_t *short_code_lookup = result->short_code_lookup;
	uint32_t sym;

	count_total[0] = 0;
	count_total[1] = 0;
	for (i = 2; i < 17; i++)
		count_total[i] = count_total[i - 1] + count[i - 1];
	memcpy(count_total_tmp, count_total, sizeof(count_total_tmp));

	code_list_len = count_total[16];
	if (code_list_len == 0) {
		memset(result->short_code_lookup, 0, sizeof(result->short_code_lookup));
		return;
	}

	for (i = 0; i < table_length; i++) {
		code_length = huff_code_table[i].length;
		if (code_length == 0)
			continue;

		insert_index = count_total_tmp[code_length];
		code_list[insert_index] = i;
		count_total_tmp[code_length]++;
	}

	last_length = huff_code_table[code_list[0]].length;
	if (last_length > ISAL_DECODE_SHORT_BITS)
		last_length = ISAL_DECODE_SHORT_BITS + 1;
	copy_size = (1 << (last_length - 1));

	/* Initialize short_code_lookup, so invalid lookups process data */
	memset(short_code_lookup, 0x00, copy_size * sizeof(*short_code_lookup));

	for (; last_length <= ISAL_DECODE_SHORT_BITS; last_length++) {
		memcpy(short_code_lookup + copy_size, short_code_lookup,
		       sizeof(*short_code_lookup) * copy_size);
		copy_size *= 2;

		for (k = count_total[last_length]; k < count_total[last_length + 1]; k++) {
			i = code_list[k];

			if (i >= max_symbol) {
				/* If the symbol is invalid, set code to be the
				 * length of the symbol and the code_length to 0
				 * to determine if there was enough input */
				short_code_lookup[huff_code_table[i].code] =
				    huff_code_table[i].length;
				continue;
			}

			/* Set lookup table to return the current symbol concatenated
			 * with the code length when the first DECODE_LENGTH bits of the
			 * address are the same as the code for the current symbol. The
			 * first 9 bits are the code, bits 14:10 are the code length,
			 * bit 15 is a flag representing this is a symbol*/
			short_code_lookup[huff_code_table[i].code] = i |
			    rfc_lookup_table.dist_extra_bit_count[i] << DIST_SYM_EXTRA_OFFSET |
			    (huff_code_table[i].length) << SMALL_SHORT_CODE_LEN_OFFSET;
		}
	}

	k = count_total[ISAL_DECODE_SHORT_BITS + 1];
	long_code_list = &code_list[k];
	long_code_length = code_list_len - k;
	for (i = 0; i < long_code_length; i++) {
		/*Set the look up table to point to a hint where the symbol can be found
		 * in the list of long codes and add the current symbol to the list of
		 * long codes. */
		if (huff_code_table[long_code_list[i]].code == 0xFFFF)
			continue;

		max_length = huff_code_table[long_code_list[i]].length;
		first_bits =
		    huff_code_table[long_code_list[i]].code
		    & ((1 << ISAL_DECODE_SHORT_BITS) - 1);

		temp_code_list[0] = long_code_list[i];
		temp_code_length = 1;

		for (j = i + 1; j < long_code_length; j++) {
			if ((huff_code_table[long_code_list[j]].code &
			     ((1 << ISAL_DECODE_SHORT_BITS) - 1)) == first_bits) {
				max_length = huff_code_table[long_code_list[j]].length;
				temp_code_list[temp_code_length] = long_code_list[j];
				temp_code_length++;
			}
		}

		memset(&result->long_code_lookup[long_code_lookup_length], 0x00,
		       2 * (1 << (max_length - ISAL_DECODE_SHORT_BITS)));

		for (j = 0; j < temp_code_length; j++) {
			sym = temp_code_list[j];
			code_length = huff_code_table[sym].length;
			long_bits = huff_code_table[sym].code >> ISAL_DECODE_SHORT_BITS;
			min_increment = 1 << (code_length - ISAL_DECODE_SHORT_BITS);
			for (; long_bits < (1 << (max_length - ISAL_DECODE_SHORT_BITS));
			     long_bits += min_increment) {
				if (sym >= max_symbol) {
					/* If the symbol is invalid, set code to be the
					 * length of the symbol and the code_length to 0
					 * to determine if there was enough input */
					result->long_code_lookup[long_code_lookup_length +
								 long_bits] = code_length;
					continue;
				}
				result->long_code_lookup[long_code_lookup_length + long_bits] =
				    sym |
				    rfc_lookup_table.dist_extra_bit_count[sym] <<
				    DIST_SYM_EXTRA_OFFSET |
				    (code_length << SMALL_LONG_CODE_LEN_OFFSET);
			}
			huff_code_table[sym].code = 0xFFFF;
		}
		result->short_code_lookup[first_bits] = long_code_lookup_length |
		    (max_length << SMALL_SHORT_CODE_LEN_OFFSET) | SMALL_FLAG_BIT;
		long_code_lookup_length += 1 << (max_length - ISAL_DECODE_SHORT_BITS);

	}
}

static inline void make_inflate_huff_code_header(struct inflate_huff_code_small *result,
						 struct huff_code *huff_code_table,
						 uint32_t table_length, uint16_t * count,
						 uint32_t max_symbol)
{
	uint32_t i, j, k;
	uint32_t *long_code_list;
	uint32_t long_code_length = 0;
	uint16_t temp_code_list[1 << (15 - ISAL_DECODE_SHORT_BITS)];
	uint32_t temp_code_length;
	uint32_t long_code_lookup_length = 0;
	uint32_t max_length;
	uint16_t first_bits;
	uint32_t code_length;
	uint16_t long_bits;
	uint16_t min_increment;
	uint32_t code_list[DIST_LEN + 2];	/* The +2 is for the extra codes in the static header */
	uint32_t code_list_len;
	uint32_t count_total[17], count_total_tmp[17];
	uint32_t insert_index;
	uint32_t last_length;
	uint32_t copy_size;
	uint16_t *short_code_lookup = result->short_code_lookup;

	count_total[0] = 0;
	count_total[1] = 0;
	for (i = 2; i < 17; i++)
		count_total[i] = count_total[i - 1] + count[i - 1];

	memcpy(count_total_tmp, count_total, sizeof(count_total_tmp));

	code_list_len = count_total[16];
	if (code_list_len == 0) {
		memset(result->short_code_lookup, 0, sizeof(result->short_code_lookup));
		return;
	}

	for (i = 0; i < table_length; i++) {
		code_length = huff_code_table[i].length;
		if (code_length == 0)
			continue;

		insert_index = count_total_tmp[code_length];
		code_list[insert_index] = i;
		count_total_tmp[code_length]++;
	}

	last_length = huff_code_table[code_list[0]].length;
	if (last_length > ISAL_DECODE_SHORT_BITS)
		last_length = ISAL_DECODE_SHORT_BITS + 1;
	copy_size = (1 << (last_length - 1));

	/* Initialize short_code_lookup, so invalid lookups process data */
	memset(short_code_lookup, 0x00, copy_size * sizeof(*short_code_lookup));

	for (; last_length <= ISAL_DECODE_SHORT_BITS; last_length++) {
		memcpy(short_code_lookup + copy_size, short_code_lookup,
		       sizeof(*short_code_lookup) * copy_size);
		copy_size *= 2;

		for (k = count_total[last_length]; k < count_total[last_length + 1]; k++) {
			i = code_list[k];

			if (i >= max_symbol)
				continue;

			/* Set lookup table to return the current symbol concatenated
			 * with the code length when the first DECODE_LENGTH bits of the
			 * address are the same as the code for the current symbol. The
			 * first 9 bits are the code, bits 14:10 are the code length,
			 * bit 15 is a flag representing this is a symbol*/
			short_code_lookup[huff_code_table[i].code] =
			    i | (huff_code_table[i].length) << SMALL_SHORT_CODE_LEN_OFFSET;
		}
	}

	k = count_total[ISAL_DECODE_SHORT_BITS + 1];
	long_code_list = &code_list[k];
	long_code_length = code_list_len - k;
	for (i = 0; i < long_code_length; i++) {
		/*Set the look up table to point to a hint where the symbol can be found
		 * in the list of long codes and add the current symbol to the list of
		 * long codes. */
		if (huff_code_table[long_code_list[i]].code == 0xFFFF)
			continue;

		max_length = huff_code_table[long_code_list[i]].length;
		first_bits =
		    huff_code_table[long_code_list[i]].code
		    & ((1 << ISAL_DECODE_SHORT_BITS) - 1);

		temp_code_list[0] = long_code_list[i];
		temp_code_length = 1;

		for (j = i + 1; j < long_code_length; j++) {
			if ((huff_code_table[long_code_list[j]].code &
			     ((1 << ISAL_DECODE_SHORT_BITS) - 1)) == first_bits) {
				if (max_length < huff_code_table[long_code_list[j]].length)
					max_length = huff_code_table[long_code_list[j]].length;
				temp_code_list[temp_code_length] = long_code_list[j];
				temp_code_length++;
			}
		}

		memset(&result->long_code_lookup[long_code_lookup_length], 0x00,
		       2 * (1 << (max_length - ISAL_DECODE_SHORT_BITS)));

		for (j = 0; j < temp_code_length; j++) {
			code_length = huff_code_table[temp_code_list[j]].length;
			long_bits =
			    huff_code_table[temp_code_list[j]].code >> ISAL_DECODE_SHORT_BITS;
			min_increment = 1 << (code_length - ISAL_DECODE_SHORT_BITS);
			for (; long_bits < (1 << (max_length - ISAL_DECODE_SHORT_BITS));
			     long_bits += min_increment) {
				result->long_code_lookup[long_code_lookup_length + long_bits] =
				    temp_code_list[j] |
				    (code_length << SMALL_LONG_CODE_LEN_OFFSET);
			}
			huff_code_table[temp_code_list[j]].code = 0xFFFF;
		}
		result->short_code_lookup[first_bits] = long_code_lookup_length |
		    (max_length << SMALL_SHORT_CODE_LEN_OFFSET) | SMALL_FLAG_BIT;
		long_code_lookup_length += 1 << (max_length - ISAL_DECODE_SHORT_BITS);

	}
}

static int header_matches_pregen(struct inflate_state *state)
{
	return 0;
#ifndef ISAL_STATIC_INFLATE_TABLE
	return 0;
#else
	uint8_t *in, *hdr;
	uint32_t in_end_bits, hdr_end_bits;
	uint32_t bytes_read_in, header_len, last_bits, last_bit_mask;
	uint64_t bits_read_mask;
	uint64_t hdr_stash, in_stash;
	const uint64_t bits_read_prior = 3;	// Have read bfinal(1) and btype(2)

	/* Check if stashed read_in_bytes match header */
	hdr = &(hufftables_default.deflate_hdr[0]);
	bits_read_mask = (1ull << state->read_in_length) - 1;
	hdr_stash = (load_u64(hdr) >> bits_read_prior) & bits_read_mask;
	in_stash = state->read_in & bits_read_mask;

	if (hdr_stash != in_stash)
		return 0;

	/* Check if input is byte aligned */
	if ((state->read_in_length + bits_read_prior) % 8)
		return 0;

	/* Check if header bulk is the same */
	in = state->next_in;
	bytes_read_in = (state->read_in_length + bits_read_prior) / 8;
	header_len = hufftables_default.deflate_hdr_count;

	if (memcmp(in, &hdr[bytes_read_in], header_len - bytes_read_in))
		return 0;

	/* If there are any last/end bits to the header check them too */
	last_bits = hufftables_default.deflate_hdr_extra_bits;
	last_bit_mask = (1 << last_bits) - 1;

	if (0 == last_bits) {
		state->next_in += header_len - bytes_read_in;
		state->avail_in -= header_len - bytes_read_in;
		state->read_in_length = 0;
		state->read_in = 0;
		return 1;
	}

	in_end_bits = in[header_len - bytes_read_in] & last_bit_mask;
	hdr_end_bits = hdr[header_len] & last_bit_mask;
	if (in_end_bits == hdr_end_bits) {
		state->next_in += header_len - bytes_read_in;
		state->avail_in -= header_len - bytes_read_in;
		state->read_in_length = 0;
		state->read_in = 0;
		inflate_in_read_bits(state, last_bits);
		return 1;
	}

	return 0;
#endif // ISAL_STATIC_INFLATE_TABLE
}

static int setup_pregen_header(struct inflate_state *state)
{
#ifdef ISAL_STATIC_INFLATE_TABLE
	memcpy(&state->lit_huff_code, &pregen_lit_huff_code, sizeof(pregen_lit_huff_code));
	memcpy(&state->dist_huff_code, &pregen_dist_huff_code, sizeof(pregen_dist_huff_code));
	state->block_state = ISAL_BLOCK_CODED;
#endif // ISAL_STATIC_INFLATE_TABLE
	return 0;
}

/* Sets the inflate_huff_codes in state to be the huffcodes corresponding to the
 * deflate static header */
static inline int setup_static_header(struct inflate_state *state)
{
#ifdef ISAL_STATIC_INFLATE_TABLE
	memcpy(&state->lit_huff_code, &static_lit_huff_code, sizeof(static_lit_huff_code));
	memcpy(&state->dist_huff_code, &static_dist_huff_code, sizeof(static_dist_huff_code));
#else

#ifndef NO_STATIC_INFLATE_H
# warning "Defaulting to static inflate table fallback."
# warning "For best performance, run generate_static_inflate, replace static_inflate.h, and recompile"
#endif
	int i;
	struct huff_code lit_code[LIT_LEN_ELEMS];
	struct huff_code dist_code[DIST_LEN + 2];
	uint32_t multisym = SINGLE_SYM_FLAG, max_dist = DIST_LEN;
	/* These tables are based on the static huffman tree described in RFC
	 * 1951 */
	uint16_t lit_count[MAX_LIT_LEN_COUNT] = {
		0, 0, 0, 0, 0, 0, 0, 24, 152, 112, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
	};

	uint16_t lit_expand_count[MAX_LIT_LEN_COUNT] = {
		0, 0, 0, 0, 0, 0, 0, -15, 1, 16, 32, 48, 16, 128, 0, 0, 0, 0, 0, 0, 0, 0
	};
	uint16_t dist_count[16] = {
		0, 0, 0, 0, 0, 32, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
	};
	uint32_t code_list[LIT_LEN_ELEMS + 2];	/* The +2 is for the extra codes in the static header */
	/* These for loops set the code lengths for the static literal/length
	 * and distance codes defined in the deflate standard RFC 1951 */
	for (i = 0; i < 144; i++)
		lit_code[i].length = 8;

	for (i = 144; i < 256; i++)
		lit_code[i].length = 9;

	for (i = 256; i < 280; i++)
		lit_code[i].length = 7;

	for (i = 280; i < LIT_LEN + 2; i++)
		lit_code[i].length = 8;

	for (i = 0; i < DIST_LEN + 2; i++)
		dist_code[i].length = 5;

	set_and_expand_lit_len_huffcode(lit_code, LIT_LEN + 2, lit_count, lit_expand_count,
					code_list);

	set_codes(dist_code, DIST_LEN + 2, dist_count);

	make_inflate_huff_code_lit_len(&state->lit_huff_code, lit_code, LIT_LEN_ELEMS,
				       lit_count, code_list, multisym);

	if (state->hist_bits && state->hist_bits < 15)
		max_dist = 2 * state->hist_bits;

	make_inflate_huff_code_dist(&state->dist_huff_code, dist_code, DIST_LEN + 2,
				    dist_count, max_dist);
#endif
#if defined(QPL_LIB)
    state->eob_code_and_len = (7u << 16) | 0x800;
#endif
	state->block_state = ISAL_BLOCK_CODED;

	return 0;
}

/* Decodes the next symbol symbol in in_buffer using the huff code defined by
 * huff_code  and returns the value in next_lits and sym_count */
static inline void decode_next_lit_len(uint32_t * next_lits, uint32_t * sym_count,
				       struct inflate_state *state,
				       struct inflate_huff_code_large *huff_code)
{
	uint32_t next_bits;
	uint32_t next_sym;
	uint32_t bit_count;
	uint32_t bit_mask;

	if (state->read_in_length <= ISAL_DEF_MAX_CODE_LEN)
		inflate_in_load(state, 0);

	next_bits = state->read_in & ((1 << ISAL_DECODE_LONG_BITS) - 1);

	/* next_sym is a possible symbol decoded from next_bits. If bit 15 is 0,
	 * next_code is a symbol. Bits 9:0 represent the symbol, and bits 14:10
	 * represent the length of that symbols huffman code. If next_sym is not
	 * a symbol, it provides a hint of where the large symbols containin
	 * this code are located. Note the hint is at largest the location the
	 * first actual symbol in the long code list.*/
	next_sym = huff_code->short_code_lookup[next_bits];

	if ((next_sym & LARGE_FLAG_BIT) == 0) {
		/* Return symbol found if next_code is a complete huffman code
		 * and shift in buffer over by the length of the next_code */
		bit_count = next_sym >> LARGE_SHORT_CODE_LEN_OFFSET;
		state->read_in >>= bit_count;
		state->read_in_length -= bit_count;

		if (bit_count == 0)
			next_sym = INVALID_SYMBOL;

		*sym_count = (next_sym >> LARGE_SYM_COUNT_OFFSET) & LARGE_SYM_COUNT_MASK;
		*next_lits = next_sym & LARGE_SHORT_SYM_MASK;

	} else {
		/* If a symbol is not found, do a lookup in the long code
		 * list starting from the hint in next_sym */
		bit_mask = next_sym >> LARGE_SHORT_MAX_LEN_OFFSET;
		bit_mask = (1 << bit_mask) - 1;
		next_bits = state->read_in & bit_mask;
		next_sym =
		    huff_code->long_code_lookup[(next_sym & LARGE_SHORT_SYM_MASK) +
						(next_bits >> ISAL_DECODE_LONG_BITS)];
		bit_count = next_sym >> LARGE_LONG_CODE_LEN_OFFSET;
		state->read_in >>= bit_count;
		state->read_in_length -= bit_count;

		if (bit_count == 0)
			next_sym = INVALID_SYMBOL;

		*sym_count = 1;
		*next_lits = next_sym & LARGE_LONG_SYM_MASK;
	}
}

static inline uint16_t decode_next_dist(struct inflate_state *state,
					struct inflate_huff_code_small *huff_code)
{
	uint16_t next_bits;
	uint16_t next_sym;
	uint32_t bit_count;
	uint32_t bit_mask;

	if (state->read_in_length <= ISAL_DEF_MAX_CODE_LEN)
		inflate_in_load(state, 0);

	next_bits = state->read_in & ((1 << ISAL_DECODE_SHORT_BITS) - 1);

	/* next_sym is a possible symbol decoded from next_bits. If bit 15 is 0,
	 * next_code is a symbol. Bits 9:0 represent the symbol, and bits 14:10
	 * represent the length of that symbols huffman code. If next_sym is not
	 * a symbol, it provides a hint of where the large symbols containin
	 * this code are located. Note the hint is at largest the location the
	 * first actual symbol in the long code list.*/
	next_sym = huff_code->short_code_lookup[next_bits];

	if ((next_sym & SMALL_FLAG_BIT) == 0) {
		/* Return symbol found if next_code is a complete huffman code
		 * and shift in buffer over by the length of the next_code */
		bit_count = next_sym >> SMALL_SHORT_CODE_LEN_OFFSET;
		state->read_in >>= bit_count;
		state->read_in_length -= bit_count;

		if (bit_count == 0) {
			state->read_in_length -= next_sym;
			next_sym = INVALID_SYMBOL;
		}

		return next_sym & DIST_SYM_MASK;

	} else {
		/* If a symbol is not found, perform a linear search of the long code
		 * list starting from the hint in next_sym */
		bit_mask = (next_sym - SMALL_FLAG_BIT) >> SMALL_SHORT_CODE_LEN_OFFSET;
		bit_mask = (1 << bit_mask) - 1;
		next_bits = state->read_in & bit_mask;
		next_sym =
		    huff_code->long_code_lookup[(next_sym & SMALL_SHORT_SYM_MASK) +
						(next_bits >> ISAL_DECODE_SHORT_BITS)];
		bit_count = next_sym >> SMALL_LONG_CODE_LEN_OFFSET;
		state->read_in >>= bit_count;
		state->read_in_length -= bit_count;

		if (bit_count == 0) {
			state->read_in_length -= next_sym;
			next_sym = INVALID_SYMBOL;
		}

		return next_sym & DIST_SYM_MASK;
	}
}

static inline uint16_t decode_next_header(struct inflate_state *state,
					  struct inflate_huff_code_small *huff_code)
{
	uint16_t next_bits;
	uint16_t next_sym;
	uint32_t bit_count;
	uint32_t bit_mask;

	if (state->read_in_length <= ISAL_DEF_MAX_CODE_LEN)
		inflate_in_load(state, 0);

	next_bits = state->read_in & ((1 << ISAL_DECODE_SHORT_BITS) - 1);

	/* next_sym is a possible symbol decoded from next_bits. If bit 15 is 0,
	 * next_code is a symbol. Bits 9:0 represent the symbol, and bits 14:10
	 * represent the length of that symbols huffman code. If next_sym is not
	 * a symbol, it provides a hint of where the large symbols containin
	 * this code are located. Note the hint is at largest the location the
	 * first actual symbol in the long code list.*/
	next_sym = huff_code->short_code_lookup[next_bits];

	if ((next_sym & SMALL_FLAG_BIT) == 0) {
		/* Return symbol found if next_code is a complete huffman code
		 * and shift in buffer over by the length of the next_code */
		bit_count = next_sym >> SMALL_SHORT_CODE_LEN_OFFSET;
		state->read_in >>= bit_count;
		state->read_in_length -= bit_count;

		if (bit_count == 0)
			next_sym = INVALID_SYMBOL;

		return next_sym & SMALL_SHORT_SYM_MASK;

	} else {
		/* If a symbol is not found, perform a linear search of the long code
		 * list starting from the hint in next_sym */
		bit_mask = (next_sym - SMALL_FLAG_BIT) >> SMALL_SHORT_CODE_LEN_OFFSET;
		bit_mask = (1 << bit_mask) - 1;
		next_bits = state->read_in & bit_mask;
		next_sym =
		    huff_code->long_code_lookup[(next_sym & SMALL_SHORT_SYM_MASK) +
						(next_bits >> ISAL_DECODE_SHORT_BITS)];
		bit_count = next_sym >> SMALL_LONG_CODE_LEN_OFFSET;
		state->read_in >>= bit_count;
		state->read_in_length -= bit_count;
		return next_sym & SMALL_LONG_SYM_MASK;

	}
}

#ifdef QPL_LIB
static inline int is_correct_huffman_tree(uint16_t *bit_length_count_table, uint32_t bit_length_count_table_size)
{
    uint32_t accum = 1;
    for (uint32_t code_bit_length = 1; code_bit_length <= bit_length_count_table_size; code_bit_length++)
    {
        accum = (accum << 1u) - bit_length_count_table[code_bit_length];
        if (accum & QPL_32U_SIGN_BIT_MASK)
            return 0;
    }
    return 1;
}
#endif

/* Reads data from the in_buffer and sets the huff code corresponding to that
 * data */
static inline int setup_dynamic_header(struct inflate_state *state)
{
	uint64_t i, j;
	struct huff_code code_huff[CODE_LEN_CODES];
	struct huff_code lit_and_dist_huff[LIT_LEN_ELEMS];
	struct huff_code *previous = NULL, *current, *end, rep_code;
	struct inflate_huff_code_small inflate_code_huff;
	uint64_t hclen, hdist, hlit;
	uint16_t code_count[16], lit_count[MAX_LIT_LEN_COUNT],
	    lit_expand_count[MAX_LIT_LEN_COUNT], dist_count[16];
	uint16_t *count;
	uint16_t symbol;
	uint32_t multisym = DEFAULT_SYM_FLAG, length, max_dist = DIST_LEN;
	struct huff_code *code;
	uint64_t flag = 0;
#ifdef    QPL_LIB
    uint32_t error_code = 0;
#endif

	int extra_count;
	uint32_t code_list[LIT_LEN_ELEMS + 2];	/* The +2 is for the extra codes in the static header */

	/* This order is defined in RFC 1951 page 13 */
	const uint8_t code_length_order[CODE_LEN_CODES] = {
		0x10, 0x11, 0x12, 0x00, 0x08, 0x07, 0x09, 0x06,
		0x0a, 0x05, 0x0b, 0x04, 0x0c, 0x03, 0x0d, 0x02, 0x0e, 0x01, 0x0f
	};

	/* If you are given a whole header and it matches the pregen header */
	if (state->avail_in > (hufftables_default.deflate_hdr_count + sizeof(uint64_t))
	    && header_matches_pregen(state))
		return setup_pregen_header(state);

	if (state->bfinal && state->avail_in <= SINGLE_SYM_THRESH) {
		multisym = SINGLE_SYM_FLAG;
	} else if (state->bfinal && state->avail_in <= DOUBLE_SYM_THRESH) {
		multisym = DOUBLE_SYM_FLAG;
	}

#ifdef QPL_LIB
    // Disable multi-symbol writing to lookup table
    if (state->disable_multisymbol_lookup_table) {
        multisym = SINGLE_SYM_FLAG;
    }
#endif

	memset(code_count, 0, sizeof(code_count));
	memset(lit_count, 0, sizeof(lit_count));
	memset(lit_expand_count, 0, sizeof(lit_expand_count));
	memset(dist_count, 0, sizeof(dist_count));
	memset(code_huff, 0, sizeof(code_huff));
	memset(lit_and_dist_huff, 0, sizeof(lit_and_dist_huff));

	/* These variables are defined in the deflate standard, RFC 1951 */
	inflate_in_load(state, 0);
	if (state->read_in_length < 14)
		return ISAL_END_INPUT;

	hlit = inflate_in_read_bits_unsafe(state, 5);
	hdist = inflate_in_read_bits_unsafe(state, 5);
	hclen = inflate_in_read_bits_unsafe(state, 4);

#if defined(QPL_LIB)
    if (hlit > 29)
        return QPL_AD_ERROR_CODE_WRONG_NUM_LL_CODES;
    if (hdist > 29)
        return QPL_AD_ERROR_CODE_WRONG_NUM_DIST_CODES;
    if (hclen > 15)
        return QPL_AD_ERROR_CODE_BAD_CL_CODE_LEN;
#else
    if (hlit > 29 || hdist > 29 || hclen > 15)
        return ISAL_INVALID_BLOCK;
#endif

	/* Create the code huffman code for decoding the lit/len and dist huffman codes */
	for (i = 0; i < 4; i++) {
		code = &code_huff[code_length_order[i]];
		length = inflate_in_read_bits_unsafe(state, 3);
		write_huff_code(code, 0, length);
		code_count[length] += 1;
		flag |= length;
	}

	inflate_in_load(state, 0);

	for (i = 4; i < hclen + 4; i++) {
		code = &code_huff[code_length_order[i]];
		length = inflate_in_read_bits_unsafe(state, 3);
		write_huff_code(code, 0, length);
		code_count[length] += 1;
		flag |= length;
	}

	if (state->read_in_length < 0)
		return ISAL_END_INPUT;

#if defined(QPL_LIB)
    if(!flag)
        return ISAL_INVALID_BLOCK;
    if (!is_correct_huffman_tree(code_count, QPL_MAX_CL_HUFFMAN_CODE_LEN))
        return QPL_AD_ERROR_CODE_BAD_CL_CODE_LEN;
    error_code = set_codes(code_huff, CODE_LEN_CODES, code_count);
    if (error_code)
        return error_code;
#else
    if (!flag || set_codes(code_huff, CODE_LEN_CODES, code_count))
        return ISAL_INVALID_BLOCK;
#endif

	make_inflate_huff_code_header(&inflate_code_huff, code_huff, CODE_LEN_CODES,
				      code_count, CODE_LEN_CODES);

	/* Decode the lit/len and dist huffman codes using the code huffman code */
	count = lit_count;
	current = lit_and_dist_huff;
	end = lit_and_dist_huff + LIT_LEN + hdist + 1;
    uint32_t distanceCounter = 0;

	while (current < end) {
		symbol = decode_next_header(state, &inflate_code_huff);

		if (state->read_in_length < 0) {
			if (current > &lit_and_dist_huff[256]
			    && lit_and_dist_huff[256].length <= 0)
				return ISAL_INVALID_BLOCK;
			return ISAL_END_INPUT;
		}

		if (symbol < 16) {
			/* If a length is found, update the current lit/len/dist
			 * to have length symbol */
			if (current == lit_and_dist_huff + LIT_TABLE_SIZE + hlit) {
				/* Switch code upon completion of lit_len table */
				current = lit_and_dist_huff + LIT_LEN;
				count = dist_count;
			}
			count[symbol]++;
			write_huff_code(current, 0, symbol);
			previous = current;
			current++;

            if (count == dist_count)
            {
                distanceCounter++;
            }

			if (symbol == 0	// No symbol
			    || (previous >= lit_and_dist_huff + LIT_TABLE_SIZE + hlit)	// Dist table
			    || (previous < lit_and_dist_huff + 264))	// Lit/Len with no extra bits
				continue;

			extra_count =
			    rfc_lookup_table.len_extra_bit_count[previous - LIT_TABLE_SIZE -
								 lit_and_dist_huff];
			lit_expand_count[symbol]--;
			lit_expand_count[symbol + extra_count] += (1 << extra_count);

		} else if (symbol == 16) {
			/* If a repeat length is found, update the next repeat
			 * length lit/len/dist elements to have the value of the
			 * repeated length */

			i = 3 + inflate_in_read_bits(state, 2);

#ifndef QPL_LIB
            if (current + i > end || previous == NULL)
                return ISAL_INVALID_BLOCK;
#else
            if (previous == NULL) {
                    return QPL_AD_ERROR_CODE_FIRST_LL_CODE_16;
            }
#endif

			rep_code = *previous;
			for (j = 0; j < i; j++) {
				if (current == lit_and_dist_huff + LIT_TABLE_SIZE + hlit) {
					/* Switch code upon completion of lit_len table */
					current = lit_and_dist_huff + LIT_LEN;
					count = dist_count;
				}

				*current = rep_code;
				count[rep_code.length]++;
				previous = current;
				current++;

                if (count == dist_count)
                {
                    distanceCounter++;
                }

				if (rep_code.length == 0	// No symbol
				    || (previous >= lit_and_dist_huff + LIT_TABLE_SIZE + hlit)	// Dist table
				    || (previous < lit_and_dist_huff + 264))	// Lit/Len with no extra
					continue;

				extra_count =
				    rfc_lookup_table.len_extra_bit_count
				    [previous - lit_and_dist_huff - LIT_TABLE_SIZE];
				lit_expand_count[rep_code.length]--;
				lit_expand_count[rep_code.length +
						 extra_count] += (1 << extra_count);

			}
		} else if (symbol == 17) {
			/* If a repeat zeroes if found, update then next
			 * repeated zeroes length lit/len/dist elements to have
			 * length 0. */
			i = 3 + inflate_in_read_bits(state, 3);

			current = current + i;
			previous = current - 1;

            if (count == dist_count)
            {
                distanceCounter += i;
            }

			if (count != dist_count
			    && current > lit_and_dist_huff + LIT_TABLE_SIZE + hlit) {
				/* Switch code upon completion of lit_len table */
				current += LIT_LEN - LIT_TABLE_SIZE - hlit;
				count = dist_count;
                distanceCounter += (current - (lit_and_dist_huff + LIT_LEN));

				if (current > lit_and_dist_huff + LIT_LEN)
					previous = current - 1;
			}

		} else if (symbol == 18) {
			/* If a repeat zeroes if found, update then next
			 * repeated zeroes length lit/len/dist elements to have
			 * length 0. */
			i = 11 + inflate_in_read_bits(state, 7);

			current = current + i;
			previous = current - 1;

            if (count == dist_count)
            {
                distanceCounter += i;
            }

			if (count != dist_count
			    && current > lit_and_dist_huff + LIT_TABLE_SIZE + hlit) {
				/* Switch code upon completion of lit_len table */
				current += LIT_LEN - LIT_TABLE_SIZE - hlit;
				count = dist_count;
                distanceCounter += (current - (lit_and_dist_huff + LIT_LEN));

				if (current > lit_and_dist_huff + LIT_LEN)
					previous = current - 1;
			}

		} else
#ifdef QPL_LIB
            return QPL_AD_ERROR_CODE_UNDEF_CL_CODE;
#else
            return ISAL_INVALID_BLOCK;
#endif

	}

#ifdef QPL_LIB
    if (hdist + 1u != distanceCounter)
    {
        return QPL_AD_ERROR_CODE_WRONG_NUM_DIST_CODES;
    }
#else 
    if (hdist + 1u != distanceCounter)
    {
        return ISAL_INVALID_BLOCK;
    }
#endif

#if defined(QPL_LIB)
    if (current > end)
        return QPL_AD_ERROR_CODE_WRONG_NUM_DIST_CODES;
    if (lit_and_dist_huff[256].length <= 0)
        return QPL_AD_ERROR_CODE_NO_LL_CODE;
#else
    if (current > end || lit_and_dist_huff[256].length <= 0)
        return ISAL_INVALID_BLOCK;
#endif

	if (state->read_in_length < 0)
		return ISAL_END_INPUT;

#if defined(QPL_LIB)
    if (!is_correct_huffman_tree(dist_count, QPL_MAX_LL_D_HUFFMAN_CODE_LEN))
        return QPL_AD_ERROR_CODE_BAD_DIST_CODE_LEN;
    if ((i = set_codes(&lit_and_dist_huff[LIT_LEN], DIST_LEN, dist_count)))
        return i;
#else
    if (set_codes(&lit_and_dist_huff[LIT_LEN], DIST_LEN, dist_count))
        return ISAL_INVALID_BLOCK;
#endif

	if (state->hist_bits && state->hist_bits < 15)
		max_dist = 2 * state->hist_bits;

	make_inflate_huff_code_dist(&state->dist_huff_code, &lit_and_dist_huff[LIT_LEN],
				    DIST_LEN, dist_count, max_dist);

	if (set_and_expand_lit_len_huffcode
	    (lit_and_dist_huff, LIT_LEN, lit_count, lit_expand_count, code_list))
#if defined(QPL_LIB)
        return QPL_AD_ERROR_CODE_BAD_LL_CODE_LEN;
#else
        return ISAL_INVALID_BLOCK;
#endif

#if defined(QPL_LIB)
    state->eob_code_and_len = (lit_and_dist_huff[256].length << 16) | (lit_and_dist_huff[256].code);
#endif

	make_inflate_huff_code_lit_len(&state->lit_huff_code, lit_and_dist_huff, LIT_LEN_ELEMS,
				       lit_count, code_list, multisym);

	state->block_state = ISAL_BLOCK_CODED;

	return 0;
}

/* Reads in the header pointed to by in_stream and sets up state to reflect that
 * header information*/
#ifndef QPL_LIB
static int read_header(struct inflate_state *state)
#else
int read_header(struct inflate_state *state)
#endif
{
	uint8_t bytes;
	uint32_t btype;
	uint16_t len, nlen;
	int ret = 0;

	/* btype and bfinal are defined in RFC 1951, bfinal represents whether
	 * the current block is the end of block, and btype represents the
	 * encoding method on the current block. */

	state->bfinal = inflate_in_read_bits(state, 1);
	btype = inflate_in_read_bits(state, 2);

	if (state->read_in_length < 0)
		ret = ISAL_END_INPUT;

	else if (btype == 0) {
		inflate_in_load(state, 40);
		bytes = state->read_in_length / 8;

		if (bytes < 4)
			return ISAL_END_INPUT;

		state->read_in >>= state->read_in_length % 8;
		state->read_in_length = bytes * 8;

		len = state->read_in & 0xFFFF;
		state->read_in >>= 16;
		nlen = state->read_in & 0xFFFF;
		state->read_in >>= 16;
		state->read_in_length -= 32;

		/* Check if len and nlen match */
		if (len != (~nlen & 0xffff))
#if defined(QPL_LIB)
            return QPL_AD_ERROR_CODE_INVALID_STORED_LEN;
#else
            return ISAL_INVALID_BLOCK;
#endif

		state->type0_block_len = len;
		state->block_state = ISAL_BLOCK_TYPE0;

		ret = 0;

	} else if (btype == 1)
		ret = setup_static_header(state);

	else if (btype == 2)
		ret = setup_dynamic_header(state);

	else
#if defined(QPL_LIB)
        ret = QPL_AD_ERROR_CODE_INVALID_BLOCK_TYPE;
#else
        ret = ISAL_INVALID_BLOCK;
#endif

	return ret;
}

/* Reads in the header pointed to by in_stream and sets up state to reflect that
 * header information*/
static int read_header_stateful(struct inflate_state *state)
{
	uint64_t read_in_start = state->read_in;
	int32_t read_in_length_start = state->read_in_length;
	uint8_t *next_in_start = state->next_in;
	uint32_t avail_in_start = state->avail_in;
	int block_state_start = state->block_state;
	int ret;
	uint32_t copy_size;
	int bytes_read;

	if (block_state_start == ISAL_BLOCK_HDR) {
		/* Setup so read_header decodes data in tmp_in_buffer */
		copy_size = ISAL_DEF_MAX_HDR_SIZE - state->tmp_in_size;
		if (copy_size > state->avail_in)
			copy_size = state->avail_in;

		memcpy(&state->tmp_in_buffer[state->tmp_in_size], state->next_in, copy_size);
		state->next_in = state->tmp_in_buffer;
		state->avail_in = state->tmp_in_size + copy_size;
	}

	ret = read_header(state);

#if defined(QPL_LIB)
    if (ret < QPL_HW_BASE_CODE)
        return ret;
#endif

	if (block_state_start == ISAL_BLOCK_HDR) {
		/* Setup so state is restored to a valid state */
		bytes_read = state->next_in - state->tmp_in_buffer - state->tmp_in_size;
		if (bytes_read < 0)
			bytes_read = 0;
		state->next_in = next_in_start + bytes_read;
		state->avail_in = avail_in_start - bytes_read;
	}

	if (ret == ISAL_END_INPUT) {
		/* Save off data so header can be decoded again with more data */
		state->read_in = read_in_start;
		state->read_in_length = read_in_length_start;
		memcpy(&state->tmp_in_buffer[state->tmp_in_size], next_in_start,
		       avail_in_start);
		state->tmp_in_size += avail_in_start;
		state->avail_in = 0;
		state->next_in = next_in_start + avail_in_start;
#if defined(QPL_LIB)
        if(state->tmp_in_size > 0)
            state->block_state = ISAL_BLOCK_HDR;    /* Changing block state */
#else
		state->block_state = ISAL_BLOCK_HDR;
#endif
	} else
		state->tmp_in_size = 0;

	return ret;

}

static inline int decode_literal_block(struct inflate_state *state)
{
	uint32_t len = state->type0_block_len;
	uint32_t bytes = state->read_in_length / 8;
	/* If the block is uncompressed, perform a memcopy while
	 * updating state data */
	state->block_state = state->bfinal ? ISAL_BLOCK_INPUT_DONE : ISAL_BLOCK_NEW_HDR;

	if (state->avail_out < len) {
		len = state->avail_out;
		state->block_state = ISAL_BLOCK_TYPE0;
	}

	if (state->avail_in + bytes < len) {
		len = state->avail_in + bytes;
		state->block_state = ISAL_BLOCK_TYPE0;
	}
	if (state->read_in_length) {
		if (len >= bytes) {
			memcpy(state->next_out, &state->read_in, bytes);

			state->next_out += bytes;
			state->avail_out -= bytes;
			state->total_out += bytes;
			state->type0_block_len -= bytes;

			state->read_in = 0;
			state->read_in_length = 0;
			len -= bytes;
			bytes = 0;

		} else {
			memcpy(state->next_out, &state->read_in, len);

			state->next_out += len;
			state->avail_out -= len;
			state->total_out += len;
			state->type0_block_len -= len;

			state->read_in >>= 8 * len;
			state->read_in_length -= 8 * len;
			bytes -= len;
			len = 0;
		}
	}
	memcpy(state->next_out, state->next_in, len);

	state->next_out += len;
	state->avail_out -= len;
	state->total_out += len;
	state->next_in += len;
	state->avail_in -= len;
	state->type0_block_len -= len;

	if (state->avail_in + bytes == 0 && state->block_state != ISAL_BLOCK_INPUT_DONE)
		return ISAL_END_INPUT;

	if (state->avail_out == 0 && state->type0_block_len > 0)
		return ISAL_OUT_OVERFLOW;

	return 0;

}

/* Decodes the next block if it was encoded using a huffman code */
int decode_huffman_code_block_stateless_base(struct inflate_state *state, uint8_t * start_out)
{
	uint16_t next_lit;
	uint8_t next_dist;
	uint32_t repeat_length;
	uint32_t look_back_dist;
	uint64_t read_in_tmp;
	int32_t read_in_length_tmp;
	uint8_t *next_in_tmp, *next_out_tmp;
	uint32_t avail_in_tmp, avail_out_tmp, total_out_tmp;
	uint32_t next_lits, sym_count;
	struct rfc1951_tables *rfc = &rfc_lookup_table;

#if defined(QPL_LIB)
    uint8_t* pMiniBlockStart = start_out;
    uint8_t* pMiniBlockEnd   = pMiniBlockStart + state->mini_block_size;
#endif

	state->copy_overflow_length = 0;
	state->copy_overflow_distance = 0;

	while (state->block_state == ISAL_BLOCK_CODED) {
		/* While not at the end of block, decode the next
		 * symbol */
		inflate_in_load(state, 0);

		read_in_tmp = state->read_in;
		read_in_length_tmp = state->read_in_length;
		next_in_tmp = state->next_in;
		avail_in_tmp = state->avail_in;
		next_out_tmp = state->next_out;
		avail_out_tmp = state->avail_out;
		total_out_tmp = state->total_out;

		decode_next_lit_len(&next_lits, &sym_count, state, &state->lit_huff_code);

		if (sym_count == 0)
#if defined(QPL_LIB)
            return QPL_AD_ERROR_CODE_BAD_LL_CODE;
#else
            return ISAL_INVALID_SYMBOL;
#endif

		if (state->read_in_length < 0) {
			state->read_in = read_in_tmp;
			state->read_in_length = read_in_length_tmp;
			state->next_in = next_in_tmp;
			state->avail_in = avail_in_tmp;
			return ISAL_END_INPUT;
		}

		while (sym_count > 0) {
			next_lit = next_lits & 0xffff;
			if (next_lit < 256 || sym_count > 1) {
				/* If the next symbol is a literal,
				 * write out the symbol and update state
				 * data accordingly. */
				if (state->avail_out < 1) {
					state->write_overflow_lits = next_lits;
					state->write_overflow_len = sym_count;
					next_lits = next_lits >> (8 * (sym_count - 1));
					sym_count = 1;

					if (next_lits < 256)
						return ISAL_OUT_OVERFLOW;
					else if (next_lits == 256) {
						state->write_overflow_len -= 1;
						state->block_state = state->bfinal ?
						    ISAL_BLOCK_INPUT_DONE : ISAL_BLOCK_NEW_HDR;
						return ISAL_OUT_OVERFLOW;
					} else {
						state->write_overflow_len -= 1;
						continue;
					}
				}

				*state->next_out = next_lit;
				state->next_out++;
				state->avail_out--;
				state->total_out++;

			} else if (next_lit == 256) {
				/* If the next symbol is the end of
				 * block, update the state data
				 * accordingly */
				state->block_state = state->bfinal ?
				    ISAL_BLOCK_INPUT_DONE : ISAL_BLOCK_NEW_HDR;

			} else if (next_lit <= MAX_LIT_LEN_SYM) {
				/* Else if the next symbol is a repeat
				 * length, read in the length extra
				 * bits, the distance code, the distance
				 * extra bits. Then write out the
				 * corresponding data and update the
				 * state data accordingly*/
				repeat_length = next_lit - 254;
				next_dist = decode_next_dist(state, &state->dist_huff_code);

#if defined(QPL_LIB)
                if (state->mini_block_size != 0 && pMiniBlockEnd < state->next_out + repeat_length)
                {
                    return QPL_AD_ERROR_MINI_BLOCK_OVERWRITE;
                }
#endif

				if (state->read_in_length >= 0) {
					if (next_dist >= DIST_LEN)
#if defined(QPL_LIB)
                        return QPL_AD_ERROR_CODE_BAD_D_CODE;
#else
                        return ISAL_INVALID_SYMBOL;
#endif

					look_back_dist = rfc->dist_start[next_dist] +
					    inflate_in_read_bits(state,
								 rfc->dist_extra_bit_count
								 [next_dist]);
				}

				if (state->read_in_length < 0) {
					state->read_in = read_in_tmp;
					state->read_in_length = read_in_length_tmp;
					state->next_in = next_in_tmp;
					state->avail_in = avail_in_tmp;
					state->next_out = next_out_tmp;
					state->avail_out = avail_out_tmp;
					state->total_out = total_out_tmp;
					state->write_overflow_lits = 0;
					state->write_overflow_len = 0;
					return ISAL_END_INPUT;
				}

#if defined(QPL_LIB)
                if (look_back_dist > IGZIP_HIST_SIZE)
                {
                    return QPL_AD_ERROR_CODE_BAD_DIST;
                }

                if (state->next_out - look_back_dist < pMiniBlockStart)
                {
                    if (state->mini_block_size == 0)
                    {
                        return QPL_AD_ERROR_CODE_REF_BEFORE_START;
                    }
                    else
                    {
                        return QPL_AD_ERROR_MINI_BLOCK_OVERREAD;
                    }
                }
#else
               if (state->next_out - look_back_dist < start_out)
                    return ISAL_INVALID_LOOKBACK;
#endif

				if (state->avail_out < repeat_length) {
					state->copy_overflow_length =
					    repeat_length - state->avail_out;
					state->copy_overflow_distance = look_back_dist;
					repeat_length = state->avail_out;
				}

				if (look_back_dist > repeat_length)
					memcpy(state->next_out,
					       state->next_out - look_back_dist,
					       repeat_length);
				else
					byte_copy(state->next_out, look_back_dist,
						  repeat_length);

				state->next_out += repeat_length;
				state->avail_out -= repeat_length;
				state->total_out += repeat_length;

				if (state->copy_overflow_length > 0)
					return ISAL_OUT_OVERFLOW;
			} else
				/* Else the read in bits do not
				 * correspond to any valid symbol */
#if defined(QPL_LIB)
                 return QPL_AD_ERROR_CODE_BAD_LL_CODE;
#else
                return ISAL_INVALID_SYMBOL;
#endif

			next_lits >>= 8;
			sym_count--;

#if defined(QPL_LIB)
            if (0 != state->mini_block_size)
            {
                if (state->next_out == pMiniBlockEnd)
                {
                    pMiniBlockStart = pMiniBlockEnd;
                    pMiniBlockEnd   = pMiniBlockStart + state->mini_block_size;
                }
            }
#endif
		}

	}
	return 0;
}

void isal_inflate_init(struct inflate_state *state)
{

	state->read_in = 0;
	state->read_in_length = 0;
	state->next_in = NULL;
	state->avail_in = 0;
	state->next_out = NULL;
	state->avail_out = 0;
	state->total_out = 0;
	state->dict_length = 0;
	state->block_state = ISAL_BLOCK_NEW_HDR;
	state->bfinal = 0;
	state->crc_flag = 0;
	state->crc = 0;
	state->hist_bits = 0;
	state->type0_block_len = 0;
	state->write_overflow_lits = 0;
	state->write_overflow_len = 0;
	state->copy_overflow_length = 0;
	state->copy_overflow_distance = 0;
	state->wrapper_flag = 0;
	state->tmp_in_size = 0;
	state->tmp_out_processed = 0;
	state->tmp_out_valid = 0;
#if defined(QPL_LIB)
    state->decomp_end_proc = DECOMP_STOP_AND_CHECK_FOR_BFINAL_EOB;
#endif
}

void isal_inflate_reset(struct inflate_state *state)
{
	state->read_in = 0;
	state->read_in_length = 0;
	state->total_out = 0;
	state->dict_length = 0;
	state->block_state = ISAL_BLOCK_NEW_HDR;
	state->bfinal = 0;
	state->crc = 0;
	state->type0_block_len = 0;
	state->write_overflow_lits = 0;
	state->write_overflow_len = 0;
	state->copy_overflow_length = 0;
	state->copy_overflow_distance = 0;
	state->wrapper_flag = 0;
	state->tmp_in_size = 0;
	state->tmp_out_processed = 0;
	state->tmp_out_valid = 0;
}

static inline uint32_t fixed_size_read(struct inflate_state *state,
				       uint8_t ** read_buf, uint32_t read_size)
{
	uint32_t tmp_in_size = state->tmp_in_size;

	if (state->avail_in + tmp_in_size < read_size) {
		memcpy(state->tmp_in_buffer + tmp_in_size, state->next_in, state->avail_in);
		tmp_in_size += state->avail_in;
		state->tmp_in_size = tmp_in_size;
		state->next_in += state->avail_in;
		state->avail_in = 0;

		return ISAL_END_INPUT;
	}

	*read_buf = state->next_in;
	if (tmp_in_size) {
		memcpy(state->tmp_in_buffer + tmp_in_size, state->next_in,
		       read_size - tmp_in_size);
		*read_buf = state->tmp_in_buffer;
		state->tmp_in_size = 0;
	}

	state->next_in += read_size - tmp_in_size;
	state->avail_in -= read_size - tmp_in_size;
	tmp_in_size = 0;

	return 0;

}

static inline uint32_t buffer_header_copy(struct inflate_state *state, uint32_t in_len,
					  uint8_t * buf, uint32_t buffer_len, uint32_t offset,
					  uint32_t buf_error)
{
	uint32_t len = in_len;
	uint32_t buf_len = buffer_len - offset;

	if (len > state->avail_in)
		len = state->avail_in;

	if (buf != NULL && buf_len < len) {
		memcpy(&buf[offset], state->next_in, buf_len);
		state->next_in += buf_len;
		state->avail_in -= buf_len;
		state->count = in_len - buf_len;
		return buf_error;
	} else {
		if (buf != NULL)
			memcpy(&buf[offset], state->next_in, len);
		state->next_in += len;
		state->avail_in -= len;
		state->count = in_len - len;

		if (len == in_len)
			return 0;
		else
			return ISAL_END_INPUT;
	}
}

static inline uint32_t string_header_copy(struct inflate_state *state,
					  char *str_buf, uint32_t str_len,
					  uint32_t offset, uint32_t str_error)
{
	uint32_t len, max_len = str_len - offset;

	if (max_len > state->avail_in || str_buf == NULL)
		max_len = state->avail_in;

	len = strnlen((char *)state->next_in, max_len);

	if (str_buf != NULL)
		memcpy(&str_buf[offset], state->next_in, len);

	state->next_in += len;
	state->avail_in -= len;
	state->count += len;

	if (str_buf != NULL && len == (str_len - offset))
		return str_error;
	else if (state->avail_in <= 0)
		return ISAL_END_INPUT;
	else {
		state->next_in++;
		state->avail_in--;
		state->count = 0;
		if (str_buf != NULL)
			str_buf[len] = 0;
	}

	return 0;
}

#ifndef QPL_LIB
static int check_gzip_checksum(struct inflate_state *state)
#else
int check_gzip_checksum(struct inflate_state *state)
#endif
{
	uint64_t trailer, crc, total_out;
	uint8_t *next_in;
	uint32_t byte_count, offset, tmp_in_size = state->tmp_in_size;
	int ret;

	if (state->read_in_length >= 8 * GZIP_TRAILER_LEN) {
		/* The following is unecessary as state->read_in_length == 64 */
		/* bit_count = state->read_in_length % 8; */
		/* state->read_in >>= bit_count; */
		/* state->read_in_length -= bit_count; */

		trailer = state->read_in;
		state->read_in_length = 0;
		state->read_in = 0;
	} else {
		if (state->read_in_length >= 8) {
			byte_count = state->read_in_length / 8;
			offset = state->read_in_length % 8;

			store_u64(state->tmp_in_buffer + tmp_in_size,
				  state->read_in >> offset);
			state->read_in = 0;
			state->read_in_length = 0;

			tmp_in_size += byte_count;
			state->tmp_in_size = tmp_in_size;
		}

		ret = fixed_size_read(state, &next_in, GZIP_TRAILER_LEN);
		if (ret) {
			state->block_state = ISAL_CHECKSUM_CHECK;
			return ret;
		}

		trailer = load_u64(next_in);
	}

	state->block_state = ISAL_BLOCK_FINISH;

	crc = state->crc;
	total_out = state->total_out;

	if (trailer != (crc | (total_out << 32)))
		return ISAL_INCORRECT_CHECKSUM;
	else
		return ISAL_DECOMP_OK;
}

static int check_zlib_checksum(struct inflate_state *state)
{

	uint32_t trailer;
	uint8_t *next_in;
	uint32_t byte_count, offset, tmp_in_size = state->tmp_in_size;
	int ret, bit_count;

	if (state->read_in_length >= 8 * ZLIB_TRAILER_LEN) {
		bit_count = state->read_in_length % 8;
		state->read_in >>= bit_count;
		state->read_in_length -= bit_count;

		trailer = state->read_in;

		state->read_in_length -= 8 * ZLIB_TRAILER_LEN;
		state->read_in >>= 8 * ZLIB_TRAILER_LEN;
	} else {
		if (state->read_in_length >= 8) {
			byte_count = state->read_in_length / 8;
			offset = state->read_in_length % 8;

			store_u64(state->tmp_in_buffer + tmp_in_size,
				  state->read_in >> offset);
			state->read_in = 0;
			state->read_in_length = 0;

			tmp_in_size += byte_count;
			state->tmp_in_size = tmp_in_size;
		}

		ret = fixed_size_read(state, &next_in, ZLIB_TRAILER_LEN);
		if (ret) {
			state->block_state = ISAL_CHECKSUM_CHECK;
			return ret;
		}

		trailer = load_u32(next_in);
	}

	state->block_state = ISAL_BLOCK_FINISH;

	if (bswap_32(trailer) != state->crc)
		return ISAL_INCORRECT_CHECKSUM;
	else
		return ISAL_DECOMP_OK;
}

int isal_read_gzip_header(struct inflate_state *state, struct isal_gzip_header *gz_hdr)
{
	int cm, flags = gz_hdr->flags, id1, id2;
	uint16_t xlen = gz_hdr->extra_len;
	uint32_t block_state = state->block_state;
	uint8_t *start_in = state->next_in, *next_in;
	uint32_t tmp_in_size = state->tmp_in_size;
	uint32_t count = state->count, offset;
	uint32_t hcrc = gz_hdr->hcrc;
	int ret = 0;

	/* This switch is a jump table into the function so that decoding the
	 * header can continue where it stopped on the last call */
	switch (block_state) {
	case ISAL_BLOCK_NEW_HDR:
		state->count = 0;
		flags = UNDEFINED_FLAG;
		if (tmp_in_size == 0)
			hcrc = 0;

		ret = fixed_size_read(state, &next_in, GZIP_HDR_BASE);
		if (ret)
			break;

		id1 = next_in[0];
		id2 = next_in[1];
		cm = next_in[2];
		flags = next_in[3];
		gz_hdr->time = load_u32(next_in + 4);
		gz_hdr->xflags = *(next_in + 8);
		gz_hdr->os = *(next_in + 9);

		if (id1 != 0x1f || id2 != 0x8b)
			return ISAL_INVALID_WRAPPER;

		if (cm != DEFLATE_METHOD)
			return ISAL_UNSUPPORTED_METHOD;

		gz_hdr->text = 0;
		if (flags & TEXT_FLAG)
			gz_hdr->text = 1;

		gz_hdr->flags = flags;

		if (flags & EXTRA_FLAG) {
	case ISAL_GZIP_EXTRA_LEN:
			ret = fixed_size_read(state, &next_in, GZIP_EXTRA_LEN);
			if (ret) {
				state->block_state = ISAL_GZIP_EXTRA_LEN;
				break;
			}

			xlen = load_u16(next_in);
			count = xlen;

			gz_hdr->extra_len = xlen;

	case ISAL_GZIP_EXTRA:
			offset = gz_hdr->extra_len - count;
			ret =
			    buffer_header_copy(state, count, gz_hdr->extra,
					       gz_hdr->extra_buf_len,
					       offset, ISAL_EXTRA_OVERFLOW);

			if (ret) {
				state->block_state = ISAL_GZIP_EXTRA;
				break;
			}
		} else {
			gz_hdr->extra_len = 0;
		}

		if (flags & NAME_FLAG) {
	case ISAL_GZIP_NAME:
			offset = state->count;
			ret = string_header_copy(state, gz_hdr->name,
						 gz_hdr->name_buf_len,
						 offset, ISAL_NAME_OVERFLOW);
			if (ret) {
				state->block_state = ISAL_GZIP_NAME;
				break;
			}
		}

		if (flags & COMMENT_FLAG) {
	case ISAL_GZIP_COMMENT:
			offset = state->count;
			ret = string_header_copy(state, gz_hdr->comment,
						 gz_hdr->comment_buf_len,
						 offset, ISAL_COMMENT_OVERFLOW);
			if (ret) {
				state->block_state = ISAL_GZIP_COMMENT;
				break;
			}
		}

		if (flags & HCRC_FLAG) {
			hcrc = crc32_gzip_refl(hcrc, start_in, state->next_in - start_in);
			gz_hdr->hcrc = hcrc;

	case ISAL_GZIP_HCRC:
			ret = fixed_size_read(state, &next_in, GZIP_HCRC_LEN);
			if (ret) {
				state->block_state = ISAL_GZIP_HCRC;
				return ret;
			}

			if ((hcrc & 0xffff) != load_u16(next_in))
				return ISAL_INCORRECT_CHECKSUM;
		}

		state->wrapper_flag = 1;
		state->block_state = ISAL_BLOCK_NEW_HDR;
		return ISAL_DECOMP_OK;
	}

	if (flags & HCRC_FLAG)
		gz_hdr->hcrc = crc32_gzip_refl(hcrc, start_in, state->next_in - start_in);

	return ret;
}

int isal_read_zlib_header(struct inflate_state *state, struct isal_zlib_header *zlib_hdr)
{
	int cmf, method, flags;
	uint32_t block_state = state->block_state;
	uint8_t *next_in;
	int ret = 0;

	switch (block_state) {
	case ISAL_BLOCK_NEW_HDR:
		zlib_hdr->dict_flag = 0;
		ret = fixed_size_read(state, &next_in, ZLIB_HDR_BASE);
		if (ret)
			break;

		cmf = *next_in;
		method = cmf & 0xf;
		flags = *(next_in + 1);

		zlib_hdr->info = cmf >> ZLIB_INFO_OFFSET;
		zlib_hdr->dict_flag = (flags & ZLIB_DICT_FLAG) ? 1 : 0;
		zlib_hdr->level = flags >> ZLIB_LEVEL_OFFSET;

		if (method != DEFLATE_METHOD)
			return ISAL_UNSUPPORTED_METHOD;

		if ((256 * cmf + flags) % 31 != 0)
			return ISAL_INCORRECT_CHECKSUM;

		if (zlib_hdr->dict_flag) {
	case ISAL_ZLIB_DICT:
			ret = fixed_size_read(state, &next_in, ZLIB_DICT_LEN);
			if (ret) {
				state->block_state = ISAL_ZLIB_DICT;
				break;
			}

			zlib_hdr->dict_id = load_u32(next_in);
		}

		state->wrapper_flag = 1;
		state->block_state = ISAL_BLOCK_NEW_HDR;
	}

	return ret;
}

int isal_inflate_set_dict(struct inflate_state *state, uint8_t * dict, uint32_t dict_len)
{

	if (state->block_state != ISAL_BLOCK_NEW_HDR
	    || state->tmp_out_processed != state->tmp_out_valid)
		return ISAL_INVALID_STATE;

	if (dict_len > IGZIP_HIST_SIZE) {
		dict = dict + dict_len - IGZIP_HIST_SIZE;
		dict_len = IGZIP_HIST_SIZE;
	}

	memcpy(state->tmp_out_buffer, dict, dict_len);
	state->tmp_out_processed = dict_len;
	state->tmp_out_valid = dict_len;
	state->dict_length = dict_len;

	return COMP_OK;
}

int isal_inflate_stateless(struct inflate_state *state)
{
	uint32_t ret = 0;
	uint8_t *start_out = state->next_out;

	state->read_in = 0;
	state->read_in_length = 0;
	state->block_state = ISAL_BLOCK_NEW_HDR;
	state->dict_length = 0;
	state->bfinal = 0;
	state->crc = 0;
	state->total_out = 0;
	state->hist_bits = 0;
	state->tmp_in_size = 0;

	if (state->crc_flag == IGZIP_GZIP) {
		struct isal_gzip_header gz_hdr;
		isal_gzip_header_init(&gz_hdr);
		ret = isal_read_gzip_header(state, &gz_hdr);
		if (ret)
			return ret;
	} else if (state->crc_flag == IGZIP_ZLIB) {
		struct isal_zlib_header z_hdr = { 0 };
		ret = isal_read_zlib_header(state, &z_hdr);
		if (ret)
			return ret;
		if (z_hdr.dict_flag)
			return ISAL_NEED_DICT;

	}

	while (state->block_state != ISAL_BLOCK_FINISH) {
		if (state->block_state == ISAL_BLOCK_NEW_HDR) {
			ret = read_header(state);

			if (ret)
				break;
		}

		if (state->block_state == ISAL_BLOCK_TYPE0)
			ret = decode_literal_block(state);
		else
			ret = decode_huffman_code_block_stateless(state, start_out);

		if (ret)
			break;
		if (state->block_state == ISAL_BLOCK_INPUT_DONE)
			state->block_state = ISAL_BLOCK_FINISH;
	}

	/* Undo count stuff of bytes read into the read buffer */
	state->next_in -= state->read_in_length / 8;
	state->avail_in += state->read_in_length / 8;
	state->read_in_length = 0;
	state->read_in = 0;

	if (!ret && state->crc_flag) {
		update_checksum(state, start_out, state->next_out - start_out);
		switch (state->crc_flag) {
		case ISAL_ZLIB:
		case ISAL_ZLIB_NO_HDR_VER:
			finalize_adler32(state);
			ret = check_zlib_checksum(state);
			break;

		case ISAL_ZLIB_NO_HDR:
			finalize_adler32(state);
			break;

		case ISAL_GZIP:
		case ISAL_GZIP_NO_HDR_VER:
			ret = check_gzip_checksum(state);
			break;
		}
	}

	return ret;
}

int isal_inflate(struct inflate_state *state)
{

	uint8_t *start_out = state->next_out;
	uint32_t avail_out = state->avail_out;
	uint32_t copy_size = 0;
	int32_t shift_size = 0;
	int ret = 0;
#if defined(QPL_LIB)
    int qpl_break = 0;
#endif

	if (!state->wrapper_flag && state->crc_flag == IGZIP_GZIP) {
		struct isal_gzip_header gz_hdr;
		isal_gzip_header_init(&gz_hdr);
		ret = isal_read_gzip_header(state, &gz_hdr);
		if (ret < 0)
			return ret;
		else if (ret > 0)
			return ISAL_DECOMP_OK;
	} else if (!state->wrapper_flag && state->crc_flag == IGZIP_ZLIB) {
		struct isal_zlib_header z_hdr = { 0 };
		ret = isal_read_zlib_header(state, &z_hdr);
		if (ret < 0)
			return ret;
		else if (ret > 0)
			return ISAL_DECOMP_OK;

		if (z_hdr.dict_flag) {
			state->dict_id = z_hdr.dict_id;
			return ISAL_NEED_DICT;
		}
	} else if (state->block_state == ISAL_CHECKSUM_CHECK) {
		switch (state->crc_flag) {
		case ISAL_ZLIB:
		case ISAL_ZLIB_NO_HDR_VER:
			ret = check_zlib_checksum(state);
			break;
		case ISAL_GZIP:
		case ISAL_GZIP_NO_HDR_VER:
			ret = check_gzip_checksum(state);
			break;
		}

		return (ret > 0) ? ISAL_DECOMP_OK : ret;
	}

	if (state->block_state != ISAL_BLOCK_FINISH) {
		state->total_out += state->tmp_out_valid - state->tmp_out_processed;
		/* If space in tmp_out buffer, decompress into the tmp_out_buffer */
		if (state->tmp_out_valid < 2 * ISAL_DEF_HIST_SIZE) {
			/* Setup to start decoding into temp buffer */
			state->next_out = &state->tmp_out_buffer[state->tmp_out_valid];
			state->avail_out =
			    sizeof(state->tmp_out_buffer) - ISAL_LOOK_AHEAD -
			    state->tmp_out_valid;

			if ((int32_t) state->avail_out < 0)
				state->avail_out = 0;

			/* Decode into internal buffer until exit */
			while (state->block_state != ISAL_BLOCK_INPUT_DONE) {
				if (state->block_state == ISAL_BLOCK_NEW_HDR
				    || state->block_state == ISAL_BLOCK_HDR) {
					ret = read_header_stateful(state);

#if defined(QPL_LIB)
                    if (ret < QPL_HW_BASE_CODE)
                        return ret;
                    if (ret)
                        break;
#else
                    if (ret)
                        break;
#endif
				}

				if (state->block_state == ISAL_BLOCK_TYPE0) {
					ret = decode_literal_block(state);
				} else {
                    uint8_t *tmp = state->tmp_out_buffer;
#if defined(QPL_LIB)
                    if(0 == state->mini_block_size)
                    {
#endif
					ret = decode_huffman_code_block_stateless(state, tmp);
#if defined(QPL_LIB)
                    }
                    else
                    {
                        ret = decode_huffman_code_block_stateless_base(state, tmp);
                    }
#endif
				}

#if defined(QPL_LIB)
                if (ret < QPL_HW_BASE_CODE)
                    return ret;
                if (ret)
                    break;
                /* If block done and we must stop at any EOB, break */
                if ((state->block_state == ISAL_BLOCK_NEW_HDR) &&
                    ((state->decomp_end_proc == DECOMP_STOP_AND_CHECK_FOR_ANY_EOB) || (state->decomp_end_proc == DECOMP_STOP_ON_ANY_EOB)))
                {
                    qpl_break = 1;
                    break;
                }
                /* If decomp procedure is don't stop on EOB and we have final block done, check for next block */
                if ((state->block_state == ISAL_BLOCK_INPUT_DONE) &&
                    ((state->decomp_end_proc == DECOMP_DONT_STOP_OR_CHECK) || (state->decomp_end_proc == DECOMP_CHECK_FOR_ANY_EOB))
                    && state->avail_in > 0)
                {
                    state->block_state = ISAL_BLOCK_NEW_HDR;
                    continue;
                }

#else
                if (ret)
                    break;
#endif
			}

			/* Copy valid data from internal buffer into out_buffer */
			if (state->write_overflow_len != 0) {
				store_u32(state->next_out, state->write_overflow_lits);
				state->next_out += state->write_overflow_len;
				state->total_out += state->write_overflow_len;
				state->write_overflow_lits = 0;
				state->write_overflow_len = 0;
			}

			if (state->copy_overflow_length != 0) {
				byte_copy(state->next_out, state->copy_overflow_distance,
					  state->copy_overflow_length);
				state->tmp_out_valid += state->copy_overflow_length;
				state->next_out += state->copy_overflow_length;
				state->total_out += state->copy_overflow_length;
				state->copy_overflow_distance = 0;
				state->copy_overflow_length = 0;
			}

			state->tmp_out_valid = state->next_out - state->tmp_out_buffer;

			/* Setup state for decompressing into out_buffer */
			state->next_out = start_out;
			state->avail_out = avail_out;
		}

		/* Copy data from tmp_out buffer into out_buffer */
		copy_size = state->tmp_out_valid - state->tmp_out_processed;
		if (copy_size > avail_out)
			copy_size = avail_out;

		memcpy(state->next_out,
		       &state->tmp_out_buffer[state->tmp_out_processed], copy_size);

		state->tmp_out_processed += copy_size;
		state->avail_out -= copy_size;
		state->next_out += copy_size;

		if (ret == ISAL_INVALID_LOOKBACK || ret == ISAL_INVALID_BLOCK
		    || ret == ISAL_INVALID_SYMBOL) {
			/* Set total_out to not count data in tmp_out_buffer */
			state->total_out -= state->tmp_out_valid - state->tmp_out_processed;
			if (state->crc_flag)
				update_checksum(state, start_out, state->next_out - start_out);
			return ret;
		}

		/* If all data from tmp_out buffer has been processed, start
		 * decompressing into the out buffer */
		if (state->tmp_out_processed == state->tmp_out_valid) {
#if defined(QPL_LIB)
            while (state->block_state != ISAL_BLOCK_INPUT_DONE && !qpl_break) {
#else
            while (state->block_state != ISAL_BLOCK_INPUT_DONE) {
#endif
				if (state->block_state == ISAL_BLOCK_NEW_HDR
				    || state->block_state == ISAL_BLOCK_HDR) {
					ret = read_header_stateful(state);
					if (ret)
						break;
				}

				if (state->block_state == ISAL_BLOCK_TYPE0)
					ret = decode_literal_block(state);
				else
					ret =
					    decode_huffman_code_block_stateless(state,
										start_out);
				if (ret)
					break;

#ifdef QPL_LIB
                /* If block done and we must stop at any EOB, break */
                if ((state->block_state == ISAL_BLOCK_NEW_HDR) &&
                    ((state->decomp_end_proc == DECOMP_STOP_AND_CHECK_FOR_ANY_EOB) || (state->decomp_end_proc == DECOMP_STOP_ON_ANY_EOB)))
                {
                    qpl_break = 1;
                    break;
                }
                /* If decomp procedure is don't stop on EOB and we have final block done, check for next block */
                if ((state->block_state == ISAL_BLOCK_INPUT_DONE) &&
                    ((state->decomp_end_proc == DECOMP_DONT_STOP_OR_CHECK) || (state->decomp_end_proc == DECOMP_CHECK_FOR_ANY_EOB))
                    && state->avail_in > 0)
                {
                    state->block_state = ISAL_BLOCK_NEW_HDR;
                    continue;
                }
#endif
			}
		}

		if (state->crc_flag)
			update_checksum(state, start_out, state->next_out - start_out);

		if (state->block_state != ISAL_BLOCK_INPUT_DONE
		    || state->copy_overflow_length + state->write_overflow_len +
		    (uint32_t) state->tmp_out_valid > sizeof(state->tmp_out_buffer)) {
			/* Save decompression history in tmp_out buffer */
			if (state->tmp_out_valid == state->tmp_out_processed
			    && avail_out - state->avail_out >= ISAL_DEF_HIST_SIZE) {
				memcpy(state->tmp_out_buffer,
				       state->next_out - ISAL_DEF_HIST_SIZE,
				       ISAL_DEF_HIST_SIZE);
				state->tmp_out_valid = ISAL_DEF_HIST_SIZE;
				state->tmp_out_processed = ISAL_DEF_HIST_SIZE;

			} else if (state->tmp_out_processed >= ISAL_DEF_HIST_SIZE) {
				shift_size = state->tmp_out_valid - ISAL_DEF_HIST_SIZE;
				if (shift_size > state->tmp_out_processed)
					shift_size = state->tmp_out_processed;

				memmove(state->tmp_out_buffer,
					&state->tmp_out_buffer[shift_size],
					state->tmp_out_valid - shift_size);
				state->tmp_out_valid -= shift_size;
				state->tmp_out_processed -= shift_size;

			}
		}

		/* Write overflow data into tmp buffer */
		if (state->write_overflow_len != 0) {
			store_u32(&state->tmp_out_buffer[state->tmp_out_valid],
				  state->write_overflow_lits);
			state->tmp_out_valid += state->write_overflow_len;
			state->total_out += state->write_overflow_len;
			state->write_overflow_lits = 0;
			state->write_overflow_len = 0;
		}

		if (state->copy_overflow_length != 0) {
			byte_copy(&state->tmp_out_buffer[state->tmp_out_valid],
				  state->copy_overflow_distance, state->copy_overflow_length);
			state->tmp_out_valid += state->copy_overflow_length;
			state->total_out += state->copy_overflow_length;
			state->copy_overflow_distance = 0;
			state->copy_overflow_length = 0;
		}

		if (ret == ISAL_INVALID_LOOKBACK || ret == ISAL_INVALID_BLOCK
		    || ret == ISAL_INVALID_SYMBOL) {
			state->total_out -= state->tmp_out_valid - state->tmp_out_processed;
			return ret;
		}

		if (state->block_state == ISAL_BLOCK_INPUT_DONE
		    && state->tmp_out_valid == state->tmp_out_processed) {
			state->block_state = ISAL_BLOCK_FINISH;

			switch (state->crc_flag) {
			case ISAL_ZLIB:
			case ISAL_ZLIB_NO_HDR_VER:
				finalize_adler32(state);
				ret = check_zlib_checksum(state);
				break;

			case ISAL_ZLIB_NO_HDR:
				finalize_adler32(state);
				break;

			case ISAL_GZIP:
			case ISAL_GZIP_NO_HDR_VER:
				ret = check_gzip_checksum(state);
				break;
			}
		}

		state->total_out -= state->tmp_out_valid - state->tmp_out_processed;
	}

	return (ret > 0) ? ISAL_DECOMP_OK : ret;
}
