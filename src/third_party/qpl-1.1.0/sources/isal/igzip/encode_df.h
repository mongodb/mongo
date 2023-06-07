/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#ifndef ENCODE_DF_H
#define ENCODE_DF_H

#include <stdint.h>
#include "igzip_lib.h"
#include "huff_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Deflate Intermediate Compression Format */
#define LIT_LEN_BIT_COUNT 10
#define LIT_LEN_MASK ((1 << LIT_LEN_BIT_COUNT) - 1)
#define DIST_LIT_BIT_COUNT 9
#define DIST_LIT_MASK ((1 << DIST_LIT_BIT_COUNT) - 1)
#define ICF_DIST_OFFSET LIT_LEN_BIT_COUNT
#define NULL_DIST_SYM 30

#define LEN_START ISAL_DEF_LIT_SYMBOLS
#define LEN_OFFSET (LEN_START - ISAL_DEF_MIN_MATCH)
#define LEN_MAX (LEN_OFFSET + ISAL_DEF_MAX_MATCH)
#define LIT_START (NULL_DIST_SYM + 1)
#define ICF_CODE_LEN 32

struct deflate_icf {
	uint32_t lit_len:LIT_LEN_BIT_COUNT;
	uint32_t lit_dist:DIST_LIT_BIT_COUNT;
	uint32_t dist_extra:ICF_CODE_LEN - DIST_LIT_BIT_COUNT - ICF_DIST_OFFSET;
};

struct deflate_icf *encode_deflate_icf(struct deflate_icf *next_in, struct deflate_icf *end_in,
			               struct BitBuf2 *bb, struct hufftables_icf * hufftables);
#ifdef __cplusplus
}
#endif
#endif
