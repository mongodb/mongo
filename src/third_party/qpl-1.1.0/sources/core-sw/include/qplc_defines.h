/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/*
 *  Intel® Query Processing Library (Intel® QPL)
 *  Core API (private C++ API)
 */

#ifndef QPL_SOURCES_CORE_INCLUDE_QPLC_DEFINES_H_
#define QPL_SOURCES_CORE_INCLUDE_QPLC_DEFINES_H_

#include "stdint.h"

#ifdef __cplusplus
extern "C" {
#endif

#if defined( _WIN32 ) || defined ( _WIN64 )
#define QPL_CORE_STDCALL  __stdcall    /**< Set calling convention (ABI) for Windows (for public API) */
#else
#define QPL_CORE_STDCALL               /**< Stdcall is default ABI for Linux */
#endif

#define K0 2
#define L9 1
#define PX 0

#if PLATFORM >= K0
#define ARCH avx512_
#elif PLATFORM == L9
#define ARCH avx2_
#else
#define ARCH px_
#endif

#define CORE_FUN_NAME_HELPER(a, b) a##b
#define CORE_FUN_NAME(a, b) CORE_FUN_NAME_HELPER(a, b)

/**
 * @brief Defines public Intel QPL core function API declaration
 */
#if !defined(QPL_CORE_API)
#define QPL_CORE_API(type, name, arg) type QPL_CORE_STDCALL name arg;
#endif

/**
 * @brief Defines internal Intel QPL core function API declaration
 */
#if !defined(OWN_QPLC_API)
#define OWN_QPLC_API(type, name, arg) extern type CORE_FUN_NAME(ARCH, name) arg;
#endif

#define CALL_CORE_FUN(name) CORE_FUN_NAME(ARCH, name)

#define OWN_BYTE_BIT_MASK 7u                    /**< Mask for max bit index in a byte */
#define OWN_BITS_2_BYTE(x) (((x) + 7u) >> 3u)   /**< Convert a number of bits to a number of bytes */
#define OWN_BITS_2_BYTE_TRUNCATE(x) ((x) >> 3u) /**< Convert a number of bits to a number of bytes with truncation */

/**
 * @brief Re-definition of Intel QPL status type for internal core needs
 */
typedef enum {
    QPLC_STS_OK                      = 0u,
    QPLC_STS_OUTPUT_OVERFLOW_ERR     = 221u,
    QPLC_STS_DST_IS_SHORT_ERR        = 225u,
    QPLC_STS_SRC_IS_SHORT_ERR        = 232u,
    QPLC_STS_INVALID_ZERO_DECOMP_HDR = 234,
} qplc_status_t;

#ifdef __cplusplus
}
#endif

#endif //QPL_SOURCES_CORE_INCLUDE_QPLC_DEFINES_H_
