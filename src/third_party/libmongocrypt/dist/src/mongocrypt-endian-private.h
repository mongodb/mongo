/*
 * Copyright 2022-present MongoDB, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* This file is copied and modified from kms_message kms_endian.h. */

#ifndef MONGOCRYPT_ENDIAN_PRIVATE_H
#define MONGOCRYPT_ENDIAN_PRIVATE_H

#include <stdint.h>
#include <string.h>

/* Define a fallback for __has_builtin for compatibility with non-clang
 * compilers. */
#ifndef __has_builtin
#define __has_builtin(x) 0
#endif

#if defined(__clang__) && __has_builtin(__builtin_bswap16) && __has_builtin(__builtin_bswap32)                         \
    && __has_builtin(__builtin_bswap64)
#define MONGOCRYPT_UINT16_SWAP_LE_BE(v) __builtin_bswap16(v)
#define MONGOCRYPT_UINT32_SWAP_LE_BE(v) __builtin_bswap32(v)
#define MONGOCRYPT_UINT64_SWAP_LE_BE(v) __builtin_bswap64(v)
#elif defined(__GNUC__) && (__GNUC__ >= 4)
#if __GNUC__ > 4 || (defined(__GNUC_MINOR__) && __GNUC_MINOR__ >= 3)
#define MONGOCRYPT_UINT32_SWAP_LE_BE(v) __builtin_bswap32((uint32_t)v)
#define MONGOCRYPT_UINT64_SWAP_LE_BE(v) __builtin_bswap64((uint64_t)v)
#endif
#if __GNUC__ > 4 || (defined(__GNUC_MINOR__) && __GNUC_MINOR__ >= 8)
#define MONGOCRYPT_UINT16_SWAP_LE_BE(v) __builtin_bswap16((uint32_t)v)
#endif
#endif

#ifndef MONGOCRYPT_UINT16_SWAP_LE_BE
#define MONGOCRYPT_UINT16_SWAP_LE_BE(v) __mongocrypt_uint16_swap_slow((uint16_t)v)
#endif

#ifndef MONGOCRYPT_UINT32_SWAP_LE_BE
#define MONGOCRYPT_UINT32_SWAP_LE_BE(v) __mongocrypt_uint32_swap_slow((uint32_t)v)
#endif

#ifndef MONGOCRYPT_UINT64_SWAP_LE_BE
#define MONGOCRYPT_UINT64_SWAP_LE_BE(v) __mongocrypt_uint64_swap_slow((uint64_t)v)
#endif

#if defined(MONGOCRYPT_LITTLE_ENDIAN)
#define MONGOCRYPT_UINT16_FROM_LE(v) ((uint16_t)v)
#define MONGOCRYPT_UINT16_TO_LE(v) ((uint16_t)v)
#define MONGOCRYPT_UINT16_FROM_BE(v) MONGOCRYPT_UINT16_SWAP_LE_BE(v)
#define MONGOCRYPT_UINT16_TO_BE(v) MONGOCRYPT_UINT16_SWAP_LE_BE(v)
#define MONGOCRYPT_UINT32_FROM_LE(v) ((uint32_t)v)
#define MONGOCRYPT_UINT32_TO_LE(v) ((uint32_t)v)
#define MONGOCRYPT_UINT32_FROM_BE(v) MONGOCRYPT_UINT32_SWAP_LE_BE(v)
#define MONGOCRYPT_UINT32_TO_BE(v) MONGOCRYPT_UINT32_SWAP_LE_BE(v)
#define MONGOCRYPT_UINT64_FROM_LE(v) ((uint64_t)v)
#define MONGOCRYPT_UINT64_TO_LE(v) ((uint64_t)v)
#define MONGOCRYPT_UINT64_FROM_BE(v) MONGOCRYPT_UINT64_SWAP_LE_BE(v)
#define MONGOCRYPT_UINT64_TO_BE(v) MONGOCRYPT_UINT64_SWAP_LE_BE(v)
#elif defined(MONGOCRYPT_BIG_ENDIAN)
#define MONGOCRYPT_UINT16_FROM_LE(v) MONGOCRYPT_UINT16_SWAP_LE_BE(v)
#define MONGOCRYPT_UINT16_TO_LE(v) MONGOCRYPT_UINT16_SWAP_LE_BE(v)
#define MONGOCRYPT_UINT16_FROM_BE(v) ((uint16_t)v)
#define MONGOCRYPT_UINT16_TO_BE(v) ((uint16_t)v)
#define MONGOCRYPT_UINT32_FROM_LE(v) MONGOCRYPT_UINT32_SWAP_LE_BE(v)
#define MONGOCRYPT_UINT32_TO_LE(v) MONGOCRYPT_UINT32_SWAP_LE_BE(v)
#define MONGOCRYPT_UINT32_FROM_BE(v) ((uint32_t)v)
#define MONGOCRYPT_UINT32_TO_BE(v) ((uint32_t)v)
#define MONGOCRYPT_UINT64_FROM_LE(v) MONGOCRYPT_UINT64_SWAP_LE_BE(v)
#define MONGOCRYPT_UINT64_TO_LE(v) MONGOCRYPT_UINT64_SWAP_LE_BE(v)
#define MONGOCRYPT_UINT64_FROM_BE(v) ((uint64_t)v)
#define MONGOCRYPT_UINT64_TO_BE(v) ((uint64_t)v)
#else
#error "The endianness of target architecture is unknown."
#endif

/*
 *--------------------------------------------------------------------------
 *
 * __mongocrypt_uint16_swap_slow --
 *
 *       Fallback endianness conversion for 16-bit integers.
 *
 * Returns:
 *       The endian swapped version.
 *
 * Side effects:
 *       None.
 *
 *--------------------------------------------------------------------------
 */

static inline uint16_t __mongocrypt_uint16_swap_slow(uint16_t v) /* IN */
{
    return (uint16_t)((v & 0x00FF) << 8) | (uint16_t)((v & 0xFF00) >> 8);
}

/*
 *--------------------------------------------------------------------------
 *
 * __mongocrypt_uint32_swap_slow --
 *
 *       Fallback endianness conversion for 32-bit integers.
 *
 * Returns:
 *       The endian swapped version.
 *
 * Side effects:
 *       None.
 *
 *--------------------------------------------------------------------------
 */

static inline uint32_t __mongocrypt_uint32_swap_slow(uint32_t v) /* IN */
{
    return ((v & 0x000000FFU) << 24) | ((v & 0x0000FF00U) << 8) | ((v & 0x00FF0000U) >> 8) | ((v & 0xFF000000U) >> 24);
}

/*
 *--------------------------------------------------------------------------
 *
 * __mongocrypt_uint64_swap_slow --
 *
 *       Fallback endianness conversion for 64-bit integers.
 *
 * Returns:
 *       The endian swapped version.
 *
 * Side effects:
 *       None.
 *
 *--------------------------------------------------------------------------
 */

static inline uint64_t __mongocrypt_uint64_swap_slow(uint64_t v) /* IN */
{
    return ((v & 0x00000000000000FFULL) << 56) | ((v & 0x000000000000FF00ULL) << 40)
         | ((v & 0x0000000000FF0000ULL) << 24) | ((v & 0x00000000FF000000ULL) << 8) | ((v & 0x000000FF00000000ULL) >> 8)
         | ((v & 0x0000FF0000000000ULL) >> 24) | ((v & 0x00FF000000000000ULL) >> 40)
         | ((v & 0xFF00000000000000ULL) >> 56);
}

#endif /* MONGOCRYPT_ENDIAN_PRIVATE_H */
