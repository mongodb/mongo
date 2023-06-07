/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#ifndef IGZIP_LEVEL_BUF_STRUCTS_H
#define IGZIP_LEVEL_BUF_STRUCTS_H

#include "igzip_lib.h"
#include "huff_codes.h"
#include "encode_df.h"

#define MATCH_BUF_SIZE (4 * 1024)

struct hash8k_buf {
	uint16_t hash_table[IGZIP_HASH8K_HASH_SIZE];
};

struct hash_hist_buf {
	uint16_t hash_table[IGZIP_HASH_HIST_SIZE];
};

struct hash_map_buf {
	uint16_t hash_table[IGZIP_HASH_MAP_HASH_SIZE];
	struct deflate_icf *matches_next;
	struct deflate_icf *matches_end;
	struct deflate_icf matches[MATCH_BUF_SIZE];
	struct deflate_icf overflow[ISAL_LOOK_AHEAD];
};

#define MAX_LVL_BUF_SIZE sizeof(struct hash_map_buf)

struct level_buf {
	struct hufftables_icf encode_tables;
	struct isal_mod_hist hist;
	uint32_t deflate_hdr_count;
	uint32_t deflate_hdr_extra_bits;
	uint8_t deflate_hdr[ISAL_DEF_MAX_HDR_SIZE];
	struct deflate_icf *icf_buf_next;
	uint64_t icf_buf_avail_out;
	struct deflate_icf *icf_buf_start;
	union {
		struct hash8k_buf hash8k;
		struct hash_hist_buf hash_hist;
		struct hash_map_buf hash_map;

		struct hash8k_buf lvl1;
		struct hash_hist_buf lvl2;
		struct hash_map_buf lvl3;
	};
};

#endif
