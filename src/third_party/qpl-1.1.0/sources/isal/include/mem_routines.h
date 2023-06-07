/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#include <stddef.h>

/**
 *  @file mem_routines.h
 *  @brief Interface to storage mem operations
 *
 *  Defines the interface for vector versions of common memory functions.
 */


#ifndef _MEM_ROUTINES_H_
#define _MEM_ROUTINES_H_

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Detect if a memory region is all zero
 *
 * Zero detect function with optimizations for large blocks > 128 bytes
 *
 * @param    mem   Pointer to memory region to test
 * @param    len   Length of region in bytes
 * @returns  0     - region is all zeros
 *           other - region has non zero bytes
 */
int isal_zero_detect(void *mem, size_t len);

#ifdef __cplusplus
}
#endif

#endif // _MEM_ROUTINES_H_

