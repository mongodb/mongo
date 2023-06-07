/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#ifndef QPLC_DEFLATE_SLOW_H_
#define QPLC_DEFLATE_SLOW_H_

#include "bitbuf2.h"
#include "igzip_lib.h"

#include "deflate_hash_table.h"
#include "deflate_defs.h"

#include "own_qplc_defs.h"


#ifdef __cplusplus
extern "C" {
#endif


OWN_QPLC_FUN(uint32_t, slow_deflate_body, (uint8_t *current_ptr,
                           const uint8_t *const lower_bound_ptr,
                           const uint8_t *const upper_bound_ptr,
                           deflate_hash_table_t   *hash_table_ptr,
                           struct isal_hufftables *huffman_tables_ptr,
                           struct BitBuf2 *bit_writer_ptr));

#ifdef __cplusplus
}
#endif

#endif // QPLC_DEFLATE_SLOW_H_
