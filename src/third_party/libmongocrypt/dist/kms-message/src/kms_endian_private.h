/*
 * Copyright 2021-present MongoDB, Inc.
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

/* This file is copied and modified from libbson's bson-endian.h. */

#ifndef KMS_ENDIAN_PRIVATE_H
#define KMS_ENDIAN_PRIVATE_H

#include <string.h>

#include "kms_message/kms_message_defines.h"

/* Define a fallback for __has_builtin for compatibility with non-clang compilers. */
#ifndef __has_builtin
  #define __has_builtin(x) 0
#endif

#if defined(__clang__) && __has_builtin(__builtin_bswap16) && \
   __has_builtin(__builtin_bswap32) && __has_builtin(__builtin_bswap64)
#define KMS_UINT16_SWAP_LE_BE(v) __builtin_bswap16 (v)
#define KMS_UINT32_SWAP_LE_BE(v) __builtin_bswap32 (v)
#define KMS_UINT64_SWAP_LE_BE(v) __builtin_bswap64 (v)
#elif defined(__GNUC__) && (__GNUC__ >= 4)
#if __GNUC__ > 4 || (defined(__GNUC_MINOR__) && __GNUC_MINOR__ >= 3)
#define KMS_UINT32_SWAP_LE_BE(v) __builtin_bswap32 ((uint32_t) v)
#define KMS_UINT64_SWAP_LE_BE(v) __builtin_bswap64 ((uint64_t) v)
#endif
#if __GNUC__ > 4 || (defined(__GNUC_MINOR__) && __GNUC_MINOR__ >= 8)
#define KMS_UINT16_SWAP_LE_BE(v) __builtin_bswap16 ((uint32_t) v)
#endif
#endif

#ifndef KMS_UINT16_SWAP_LE_BE
#define KMS_UINT16_SWAP_LE_BE(v) __kms_uint16_swap_slow ((uint16_t) v)
#endif

#ifndef KMS_UINT32_SWAP_LE_BE
#define KMS_UINT32_SWAP_LE_BE(v) __kms_uint32_swap_slow ((uint32_t) v)
#endif

#ifndef KMS_UINT64_SWAP_LE_BE
#define KMS_UINT64_SWAP_LE_BE(v) __kms_uint64_swap_slow ((uint64_t) v)
#endif

#if defined(KMS_MESSAGE_LITTLE_ENDIAN)
#define KMS_UINT16_FROM_LE(v) ((uint16_t) v)
#define KMS_UINT16_TO_LE(v) ((uint16_t) v)
#define KMS_UINT16_FROM_BE(v) KMS_UINT16_SWAP_LE_BE (v)
#define KMS_UINT16_TO_BE(v) KMS_UINT16_SWAP_LE_BE (v)
#define KMS_UINT32_FROM_LE(v) ((uint32_t) v)
#define KMS_UINT32_TO_LE(v) ((uint32_t) v)
#define KMS_UINT32_FROM_BE(v) KMS_UINT32_SWAP_LE_BE (v)
#define KMS_UINT32_TO_BE(v) KMS_UINT32_SWAP_LE_BE (v)
#define KMS_UINT64_FROM_LE(v) ((uint64_t) v)
#define KMS_UINT64_TO_LE(v) ((uint64_t) v)
#define KMS_UINT64_FROM_BE(v) KMS_UINT64_SWAP_LE_BE (v)
#define KMS_UINT64_TO_BE(v) KMS_UINT64_SWAP_LE_BE (v)
#elif defined(KMS_MESSAGE_BIG_ENDIAN)
#define KMS_UINT16_FROM_LE(v) KMS_UINT16_SWAP_LE_BE (v)
#define KMS_UINT16_TO_LE(v) KMS_UINT16_SWAP_LE_BE (v)
#define KMS_UINT16_FROM_BE(v) ((uint16_t) v)
#define KMS_UINT16_TO_BE(v) ((uint16_t) v)
#define KMS_UINT32_FROM_LE(v) KMS_UINT32_SWAP_LE_BE (v)
#define KMS_UINT32_TO_LE(v) KMS_UINT32_SWAP_LE_BE (v)
#define KMS_UINT32_FROM_BE(v) ((uint32_t) v)
#define KMS_UINT32_TO_BE(v) ((uint32_t) v)
#define KMS_UINT64_FROM_LE(v) KMS_UINT64_SWAP_LE_BE (v)
#define KMS_UINT64_TO_LE(v) KMS_UINT64_SWAP_LE_BE (v)
#define KMS_UINT64_FROM_BE(v) ((uint64_t) v)
#define KMS_UINT64_TO_BE(v) ((uint64_t) v)
#else
#error "The endianness of target architecture is unknown."
#endif

/*
 *--------------------------------------------------------------------------
 *
 * __kms_uint16_swap_slow --
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

static KMS_MSG_INLINE uint16_t
__kms_uint16_swap_slow (uint16_t v) /* IN */
{
   return (uint16_t) (((v & 0x00FF) << 8) | ((v & 0xFF00) >> 8));
}


/*
 *--------------------------------------------------------------------------
 *
 * __kms_uint32_swap_slow --
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

static KMS_MSG_INLINE uint32_t
__kms_uint32_swap_slow (uint32_t v) /* IN */
{
   return ((v & 0x000000FFU) << 24) | ((v & 0x0000FF00U) << 8) |
          ((v & 0x00FF0000U) >> 8) | ((v & 0xFF000000U) >> 24);
}


/*
 *--------------------------------------------------------------------------
 *
 * __kms_uint64_swap_slow --
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

static KMS_MSG_INLINE uint64_t
__kms_uint64_swap_slow (uint64_t v) /* IN */
{
   return ((v & 0x00000000000000FFULL) << 56) |
          ((v & 0x000000000000FF00ULL) << 40) |
          ((v & 0x0000000000FF0000ULL) << 24) |
          ((v & 0x00000000FF000000ULL) << 8) |
          ((v & 0x000000FF00000000ULL) >> 8) |
          ((v & 0x0000FF0000000000ULL) >> 24) |
          ((v & 0x00FF000000000000ULL) >> 40) |
          ((v & 0xFF00000000000000ULL) >> 56);
}


#endif /* KMS_ENDIAN_PRIVATE_H */
