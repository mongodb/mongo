/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <memory.h>
#include <assert.h>

#if __x86_64__  || __i386__ || _M_X64 || _M_IX86
#ifdef _MSC_VER
# include <intrin.h>
#else
# include <x86intrin.h>
#endif
#endif //__x86_64__  || __i386__ || _M_X64 || _M_IX86

#include "encode_df.h"
#include "bitbuf2.h"

struct deflate_icf *encode_deflate_icf_base(struct deflate_icf *next_in,
					    struct deflate_icf *end_in, struct BitBuf2 *bb,
					    struct hufftables_icf *hufftables)
{
	struct huff_code lsym, dsym;

	while (next_in < end_in && !is_full(bb)) {
		lsym = hufftables->lit_len_table[next_in->lit_len];
		dsym = hufftables->dist_lit_table[next_in->lit_dist];

		// insert ll code, dist_code, and extra_bits
		write_bits_unsafe(bb, lsym.code_and_extra, lsym.length);
		write_bits_unsafe(bb, dsym.code, dsym.length);
		write_bits_unsafe(bb, next_in->dist_extra, dsym.extra_bit_count);
		flush_bits(bb);

		next_in++;
	}

	return next_in;
}
