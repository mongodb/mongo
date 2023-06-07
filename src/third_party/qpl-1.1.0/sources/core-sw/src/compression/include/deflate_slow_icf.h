/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#ifndef QPLC_DEFLATE_SLOW_ICF_H_
#define QPLC_DEFLATE_SLOW_ICF_H_

#include "huff_codes.h"
#include "bitbuf2.h"

#include "igzip_level_buf_structs.h"

#include "deflate_hash_table.h"
#include "deflate_defs.h"

#include "own_qplc_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct isal_mod_hist isal_mod_hist;

OWN_QPLC_FUN(uint32_t, slow_deflate_icf_body, (uint8_t* current_ptr,
    const uint8_t* const lower_bound_ptr,
    const uint8_t        * const upper_bound_ptr,
    deflate_hash_table_t * hash_table_ptr,
    isal_mod_hist        * histogram_ptr,
    deflate_icf_stream* icf_stream_ptr));

#ifdef __cplusplus
}
#endif

#endif // QPLC_DEFLATE_SLOW_ICF_H_
