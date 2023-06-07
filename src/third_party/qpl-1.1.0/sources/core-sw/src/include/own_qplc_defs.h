/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/*------- qplc_own_defs.h -------*/

/**
 * @date 07/06/2020
 *
 * @defgroup SW_KERNELS_PRIVATE_API Private
 * @ingroup  SW_KERNELS_API
 * @{
 *
 * @brief Contains common macro and definitions for Intel® Query Processing Library (Intel® QPL) core functionality
 *
 *
 */

#include <stdint.h>
#include "qplc_defines.h"
#include "immintrin.h"

#ifndef QPLC_OWN_CORE_DEFS_H_
#define QPLC_OWN_CORE_DEFS_H_

#ifdef __cplusplus
extern "C" {
#endif

#define OPT_PREFIX opt_

#define OPT_FUNCTION_NAME(name) CORE_FUN_NAME(OPT_PREFIX, name)

#define CALL_OPT_FUNCTION(name) OPT_FUNCTION_NAME(name)

/* ------ Macro ------ */
/**
 * @brief Defines public Intel QPL core function implementation
 */
#define QPL_CORE_FUN(type, name, arg) extern type QPL_CORE_STDCALL name arg

#define OWN_OPT_FUN(type, name, arg) type OPT_FUNCTION_NAME(name) arg

/**
 * @brief Defines internal Intel QPL core function implementation
 */
#define OWN_QPLC_FUN(type, name, arg) extern type CORE_FUN_NAME(ARCH, name) arg

/**
 * @brief Defines internal inline Intel QPL core function
 */
#define OWN_QPLC_INLINE(type, name, arg) static inline type name arg

#if defined(_MSC_VER)
#define OWN_ALIGNED_ARRAY(array_declaration, alignment) __declspec(align(alignment)) array_declaration
#elif defined(__GNUC__)
#define OWN_ALIGNED_ARRAY(array_declaration, alignment) array_declaration __attribute__((aligned(alignment)))
#endif

#define OWN_ALIGNED_64_ARRAY(array_declaration) OWN_ALIGNED_ARRAY(array_declaration, 64u)

#define QPL_MAX(a, b) (((a) > (b)) ? (a) : (b))    /**< Simple minimal value idiom */
#define QPL_MIN(a, b) (((a) < (b)) ? (a) : (b))    /**< Simple maximal value idiom */

/**
 * @brief Internal Intel QPL core definitions
 */

#define QPL_ONE_64U       (1ULL)
#define OWN_MAX_16U       0xFFFF                      /**< Max value for uint16_t */
#define OWN_MAX_32U       0xFFFFFFFF                  /**< Max value for uint32_t */
#define OWN_1_BIT_MASK    1u                          /**< Mask for 1-bit integer */
#define OWN_2_BIT_MASK    3u                          /**< Mask for 2-bit integer */
#define OWN_3_BIT_MASK    7u                          /**< Mask for 3-bit integer */
#define OWN_4_BIT_MASK    0xfu                        /**< Mask for 4-bit integer */
#define OWN_5_BIT_MASK    0x1fu                       /**< Mask for 5-bit integer */
#define OWN_6_BIT_MASK    0x3fu                       /**< Mask for 6-bit integer */
#define OWN_7_BIT_MASK    0x7fu                       /**< Mask for 7-bit integer */
#define OWN_HIGH_BIT_MASK 0x80u                       /**< Mask for most significant bit in a byte */
#define OWN_LOW_BIT_MASK  1u                          /**< Mask for least significant bit in a byte */
#define OWN_BYTE_WIDTH    8u                          /**< Byte width in bits */
#define OWN_WORD_WIDTH    16u                         /**< Word width in bits */
#define OWN_3_BYTE_WIDTH  24u                         /**< 3-byte width in bits */
#define OWN_DWORD_WIDTH   32u                         /**< Dword width in bits */
#define OWN_6_BYTE_WIDTH  48u                         /**< 6-byte width in bits */
#define OWN_7_BYTE_WIDTH  56u                         /**< 7-byte width in bits */
#define OWN_QWORD_WIDTH   64u                         /**< Qword width in bits */
#define OWN_BIT_MASK(x) ((QPL_ONE_64U << (x)) - 1u)   /**< Bit mask below bit position */
#define OWN_PARQUET_WIDTH 8u                          /**< Parquet size in elements (PRLE format) */
#define OWN_LITERAL_OCTA_GROUP 1u                     /**< PRLE format description */
#define OWN_VARINT_BYTE_1(x) (((x) & OWN_7_BIT_MASK) << 6u)   /**< 1st byte extraction for varint format */
#define OWN_VARINT_BYTE_2(x) (((x) & OWN_7_BIT_MASK) << 13u)  /**< 2nd byte extraction for varint format */
#define OWN_VARINT_BYTE_3(x) (((x) & OWN_7_BIT_MASK) << 20u)  /**< 3rd byte extraction for varint format */
#define OWN_VARINT_BYTE_4(x) (((x) & OWN_5_BIT_MASK) << 27u)  /**< 4th byte extraction for varint format */
#define OWN_PRLE_COUNT(x) (((x) & OWN_7_BIT_MASK) >> 1u)      /**< PRLE count field extraction */
#define OWN_MAX(a, b) (((a) > (b)) ? (a) : (b))               /**< Maximum from 2 values */
#define OWN_MIN(a, b) (((a) < (b)) ? (a) : (b))               /**< Minimum from 2 values */
#ifndef UNREFERENCED_PARAMETER
#ifdef __GNUC__
#define UNREFERENCED_PARAMETER(p) p __attribute__((unused)) /**< Unreferenced parameter - warning removal */
#else
#define UNREFERENCED_PARAMETER(p) p                         /**< Unreferenced parameter - warning removal */
#endif
#endif

/**
 * Convert a number of bits to index in {8u, 16u, 32u}
 */
#define OWN_BITS_2_DATA_TYPE_INDEX(x) \
                            (QPL_MIN((((x) - 1u) >> 3u), 2u))
#define OWN_BITS_2_WORD(x) (((x) + 15u) >> 4u)           /**< Convert a number of bits to a number of words */
#define OWN_BITS_2_DWORD(x) (((x) + 31u) >> 5u)          /**< Convert a number of bits to a number of double words */

/**
 * Checks if input bit width corresponds to standard data type
 */
#define OWN_STANDARD_TYPE(x, y) (((x) == 8u) || ((((x) == 16u)\
                                    || ((x) == 32u)) && (y)))
#define OWN_CONDITION_BREAK(x) if ((x)) {break;}              /**< Break loop if condition is met */

/**
 * @brief Macro for cheching error situation
 */
#define OWN_RETURN_ERROR(expression, error_code) { if (expression) { return (error_code); }}

/**
 * @brief 64-bit union for simplifying arbitrary bit-width integers conversions to/from standard types
 */
typedef union {
    uint64_t bit_buf;
    uint8_t  byte_buf[8];
}                    qplc_bit_byte_pool64_t;

/**
 * @brief 32-bit union for simplifying arbitrary bit-width integers conversions to/from standard types
 */
typedef union {
    uint32_t bit_buf;
    uint16_t word_buf[2];
    uint8_t  byte_buf[4];
}                    qplc_bit_byte_pool32_t;

/**
 * @brief 16-bit union for simplifying arbitrary bit-width integers conversions to/from standard types
 */
typedef union {
    uint16_t bit_buf;
    uint8_t  byte_buf[2];
}                    qplc_bit_byte_pool16_t;

/**
 * @brief 48-bit union for simplifying arbitrary bit-width integers conversions to/from standard types
 */
typedef union {
    uint8_t  byte_buf[8];
    uint32_t dw_buf[2];
    uint16_t word_buf[4];
    uint64_t bit_buf;
}                    qplc_bit_byte_pool48_t;

/**
 * @brief Inline 16u function for LE<->BE format conversions
 */
OWN_QPLC_INLINE(uint16_t, qplc_swap_bytes_16u, (uint16_t value)) {
    qplc_bit_byte_pool16_t in_value;
    qplc_bit_byte_pool16_t out_value;

    in_value.bit_buf = value;
    out_value.byte_buf[0] = in_value.byte_buf[1];
    out_value.byte_buf[1] = in_value.byte_buf[0];
    return out_value.bit_buf;
}

/**
 * @brief Inline 32u function for LE<->BE format conversions
 */
OWN_QPLC_INLINE(uint32_t, qplc_swap_bytes_32u, (uint32_t value)) {
    qplc_bit_byte_pool32_t in_value;
    qplc_bit_byte_pool32_t out_value;

    in_value.bit_buf = value;
    out_value.byte_buf[0] = in_value.byte_buf[3];
    out_value.byte_buf[1] = in_value.byte_buf[2];
    out_value.byte_buf[2] = in_value.byte_buf[1];
    out_value.byte_buf[3] = in_value.byte_buf[0];
    return out_value.bit_buf;
}

/**
 * @brief Inline helper to convert pointer to integer
 */
OWN_QPLC_INLINE(uint64_t, OWN_QPLC_UINT_PTR, (const void *ptr)) {
    union {
        void     *ptr;
        uint64_t cardinal;
    } dd;
    dd.ptr = (void *) ptr;
    return dd.cardinal;
}

#define OWN_QPLC_PACK_BE_INDEX_SHIFT 35u

/**
 * @brief Helper for calculating input bit width from pack index.
 *
 * @param[in]  pack_index  from qplc_get_pack_bits_index function output
 *
 * @return
 *      - bit_width;
 */
OWN_QPLC_INLINE(uint32_t, own_get_bit_width_from_index, (uint32_t pack_index)) {
    uint32_t bit_width = pack_index + 1u;
    bit_width = (33u == bit_width) ? OWN_WORD_WIDTH : bit_width;
    bit_width = (33u < bit_width) ? OWN_DWORD_WIDTH : bit_width;
    return bit_width;
}

#ifdef __cplusplus
}
#endif

#endif // QPLC_OWN_CORE_DEFS_H_
/**
 * @}
 */
