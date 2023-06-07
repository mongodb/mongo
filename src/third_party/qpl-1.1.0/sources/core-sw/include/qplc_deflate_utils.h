/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/*
 *  Intel® Query Processing Library (Intel® QPL)
 *  Core API (private C++ API)
 */

#ifndef QPL_SOURCES_CORE_INCLUDE_QPLC_COMPRESSION_H_
#define QPL_SOURCES_CORE_INCLUDE_QPLC_COMPRESSION_H_

#include "../src/compression/include/deflate_hash_table.h"
#include "qplc_compression_consts.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct isal_mod_hist      isal_mod_hist;
typedef struct deflate_icf_stream deflate_icf_stream;

typedef struct isal_hufftables isal_hufftables;
typedef struct BitBuf2         BitBuf2;

typedef uint32_t(*qplc_slow_deflate_body_t_ptr)(uint8_t *current_ptr,
                                                const uint8_t *const lower_bound_ptr,
                                                const uint8_t *const upper_bound_ptr,
                                                deflate_hash_table_t *hash_table_ptr,
                                                struct isal_hufftables *huffman_tables_ptr,
                                                struct BitBuf2 *bit_writer_ptr);

typedef uint32_t(*qplc_slow_deflate_icf_body_t_ptr)(uint8_t *current_ptr,
                                                    const uint8_t *const lower_bound_ptr,
                                                    const uint8_t *const upper_bound_ptr,
                                                    deflate_hash_table_t *hash_table_ptr,
                                                    isal_mod_hist *histogram_ptr,
                                                    deflate_icf_stream *icf_stream_ptr);

typedef void(*qplc_setup_dictionary_t_ptr)(uint8_t *dictionary_ptr,
                                           uint32_t dictionary_size,
                                           deflate_hash_table_t *hash_table_ptr);

#ifdef __cplusplus
}
#endif

#endif //QPL_SOURCES_CORE_INCLUDE_QPLC_COMPRESSION_H_
