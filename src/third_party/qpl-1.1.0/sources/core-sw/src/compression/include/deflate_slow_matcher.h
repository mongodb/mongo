/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#ifndef QPLC_DEFLATE_SLOW_MATCHER_H_
#define QPLC_DEFLATE_SLOW_MATCHER_H_

#include "deflate_defs.h"

#include "stdint.h"

#ifdef __cplusplus
extern "C" {
#endif

deflate_match_t get_lazy_best_match(const deflate_hash_table_t *const hash_table_ptr,
                                    const uint8_t *const lower_bound_ptr,
                                    const uint8_t *const string_ptr,
                                    const uint8_t *const upper_bound_ptr);

#ifdef __cplusplus
}
#endif

#endif // QPLC_DEFLATE_SLOW_MATCHER_H_
