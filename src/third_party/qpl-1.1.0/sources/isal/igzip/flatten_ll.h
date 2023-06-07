/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#ifndef FLATTEN_LL_H
#define FLATTEN_LL_H

#include <stdint.h>

#ifdef QPL_LIB
#ifdef __cplusplus
extern "C" {
#endif
#endif

void flatten_ll(uint32_t *ll_hist);

#ifdef QPL_LIB
#ifdef __cplusplus
}; // extern "C"
#endif
#endif

#endif // FLATTEN_LL_H
