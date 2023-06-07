/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#ifndef QPLC_DEFLATE_SLOW_UTILS_H_
#define QPLC_DEFLATE_SLOW_UTILS_H_

#include "stdint.h"

#include "igzip_lib.h"
#include "encode_df.h"

#include "deflate_hash_table.h"
#include "deflate_defs.h"

#include "own_qplc_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct isal_mod_hist isal_mod_hist;

uint32_t update_missed_literals(uint8_t *current_ptr,
                                const uint8_t *upper_bound_ptr,
                                const uint8_t *lower_bound_ptr,
                                deflate_hash_table_t *hash_table_ptr);

void update_histogram_for_literal(isal_mod_hist *const histogram_ptr, const uint8_t literal);

void update_histogram_for_match(isal_mod_hist *const histogram_ptr, const deflate_match_t match);

void get_match_length_code(const struct isal_hufftables *const huffman_table_ptr,
                           const uint32_t match_length,
                           uint64_t *const code_ptr,
                           uint32_t *const code_length_ptr);

void get_offset_code(const struct isal_hufftables *const huffman_table_ptr,
                     uint32_t offset,
                     uint64_t *const code_ptr,
                     uint32_t *const code_length_ptr);

void get_literal_code(const struct isal_hufftables *const huffman_table_ptr,
                      const uint32_t literal,
                      uint64_t *const code_ptr,
                      uint32_t *const code_length_ptr);

void get_distance_icf_code(uint32_t distance, uint32_t *code, uint32_t *extra_bits);

void write_deflate_icf(struct deflate_icf *icf,
                       uint32_t lit_len,
				       uint32_t lit_dist,
                       uint32_t extra_bits);

OWN_QPLC_FUN(void, setup_dictionary, (uint8_t *dictionary_ptr,
                                      uint32_t dictionary_size,
        deflate_hash_table_t * hash_table_ptr));

#ifdef __cplusplus
}
#endif

#endif // QPLC_DEFLATE_SLOW_UTILS_H_
