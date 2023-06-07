/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#include "qplc_defines.h"
#include "qplc_scan.h"
#include "qplc_extract.h"
#include "qplc_select.h"
#include "qplc_expand.h"
#include "qplc_unpack.h"
#include "qplc_pack.h"
#include "qplc_memop.h"
#include "qplc_aggregates.h"
#include "qplc_checksum.h"

#ifndef OWN_QPL_CORE_API_H_
#define OWN_QPL_CORE_API_H_

#ifdef __cplusplus
extern "C" {
#endif

/*------- qplc_api.h -------*/

/**
 * @date 07/06/2020
 *
 * @brief Public header of  all core functionality.
 *
 * @details Core APIs implement the following functionalities:
 *      -   Unpacking n-bit integers' vector to 8u, 16u or 32u integers;
 *      -   Unpacking input data in PRLE format to 8u, 16u or 32u integers;
 *      -   Unpacking n-bit integers' vector in BE format to 8u, 16u or 32u integers;
 *      -   Scan analytics operation in-place & out-of-place kernels for 8u, 16u and 32u input data and 8u output;
 *      -   Extract analytics operation in-place & out-of-place kernels for 8u, 16u and 32u input data;
 *      -   Find Unique analytics operation out-of-place kernels for 8u, 16u and 32u input data;
 *      -   Set Membership analytics operation in-place kernels for 8u, 16u and 32u input data;
 *      -   Select analytics operation in-place & out-of-place kernels for 8u, 16u and 32u input data;
 *      -   Aggregates calculation kernel for 8u input data and for nominal bit vector output;
 *      -   Packing kernels for 8u, 16u and 32u input data and 1..32u output data;
 *      -   Packing kernels for 8u, 16u and 32u input data and 1..32u output data in BE format;
 *      -   Packing kernels for 8u input data and index output data in 8u, 16u or 32u representation;
 *      -   Packing kernels for 8u input data and index output data in 8u, 16u or 32u representation in BE format.
 *
 */

/**
 * @defgroup SW_KERNELS_PUBLIC_API Core API
 * @ingroup SW_KERNELS_PUBLIC_API
 * @{
 */

/**
 * @brief Unpacking input data in format of any-bit-width, LE or BE, to vector of 8u, 16u or 32u integers.
 *
 * @param[in]   src_ptr       pointer to source vector in packed any-bit-width integers format
 * @param[in]   num_elements  number of n-bit integers to unpack
 * @param[in]   start_bit     bit position in the first byte to start from
 * @param[out]  dst_ptr       pointer to unpacked data in 8u, 16u or 32u format (depends on bit width)
 *
 * @note Parameters:  (uint8_t *src_ptr, uint32_t num_elements, uint32_t start_bit, uint8_t *dst_ptr)
 * @note Unpack function table contains 64 entries - starts from 1-32 bit-width for LE, then 1-32 for BE input
 * @note Index calculation: inputBeShift = (qpl_job_ptr->parser == qpl_p_be_packed_array) ? 32u : 0u;
 * @note                    unpackIndex = inputBeShift + bit_width - 1u;
 *
 * @return
 *      - n/a (void).
 */
static const qplc_unpack_bits_t_ptr qplc_unpack_bits_array[] = {
        CALL_CORE_FUN(qplc_unpack_1u8u),
        CALL_CORE_FUN(qplc_unpack_2u8u),
        CALL_CORE_FUN(qplc_unpack_3u8u),
        CALL_CORE_FUN(qplc_unpack_4u8u),
        CALL_CORE_FUN(qplc_unpack_5u8u),
        CALL_CORE_FUN(qplc_unpack_6u8u),
        CALL_CORE_FUN(qplc_unpack_7u8u),
        CALL_CORE_FUN(qplc_unpack_8u8u),
        CALL_CORE_FUN(qplc_unpack_9u16u),
        CALL_CORE_FUN(qplc_unpack_10u16u),
        CALL_CORE_FUN(qplc_unpack_11u16u),
        CALL_CORE_FUN(qplc_unpack_12u16u),
        CALL_CORE_FUN(qplc_unpack_13u16u),
        CALL_CORE_FUN(qplc_unpack_14u16u),
        CALL_CORE_FUN(qplc_unpack_15u16u),
        CALL_CORE_FUN(qplc_unpack_16u16u),
        CALL_CORE_FUN(qplc_unpack_17u32u),
        CALL_CORE_FUN(qplc_unpack_18u32u),
        CALL_CORE_FUN(qplc_unpack_19u32u),
        CALL_CORE_FUN(qplc_unpack_20u32u),
        CALL_CORE_FUN(qplc_unpack_21u32u),
        CALL_CORE_FUN(qplc_unpack_22u32u),
        CALL_CORE_FUN(qplc_unpack_23u32u),
        CALL_CORE_FUN(qplc_unpack_24u32u),
        CALL_CORE_FUN(qplc_unpack_25u32u),
        CALL_CORE_FUN(qplc_unpack_26u32u),
        CALL_CORE_FUN(qplc_unpack_27u32u),
        CALL_CORE_FUN(qplc_unpack_28u32u),
        CALL_CORE_FUN(qplc_unpack_29u32u),
        CALL_CORE_FUN(qplc_unpack_30u32u),
        CALL_CORE_FUN(qplc_unpack_31u32u),
        CALL_CORE_FUN(qplc_unpack_32u32u),
        CALL_CORE_FUN(qplc_unpack_be_1u8u),
        CALL_CORE_FUN(qplc_unpack_be_2u8u),
        CALL_CORE_FUN(qplc_unpack_be_3u8u),
        CALL_CORE_FUN(qplc_unpack_be_4u8u),
        CALL_CORE_FUN(qplc_unpack_be_5u8u),
        CALL_CORE_FUN(qplc_unpack_be_6u8u),
        CALL_CORE_FUN(qplc_unpack_be_7u8u),
        CALL_CORE_FUN(qplc_unpack_be_8u8u),
        CALL_CORE_FUN(qplc_unpack_be_9u16u),
        CALL_CORE_FUN(qplc_unpack_be_10u16u),
        CALL_CORE_FUN(qplc_unpack_be_11u16u),
        CALL_CORE_FUN(qplc_unpack_be_12u16u),
        CALL_CORE_FUN(qplc_unpack_be_13u16u),
        CALL_CORE_FUN(qplc_unpack_be_14u16u),
        CALL_CORE_FUN(qplc_unpack_be_15u16u),
        CALL_CORE_FUN(qplc_unpack_be_16u16u),
        CALL_CORE_FUN(qplc_unpack_be_17u32u),
        CALL_CORE_FUN(qplc_unpack_be_18u32u),
        CALL_CORE_FUN(qplc_unpack_be_19u32u),
        CALL_CORE_FUN(qplc_unpack_be_20u32u),
        CALL_CORE_FUN(qplc_unpack_be_21u32u),
        CALL_CORE_FUN(qplc_unpack_be_22u32u),
        CALL_CORE_FUN(qplc_unpack_be_23u32u),
        CALL_CORE_FUN(qplc_unpack_be_24u32u),
        CALL_CORE_FUN(qplc_unpack_be_25u32u),
        CALL_CORE_FUN(qplc_unpack_be_26u32u),
        CALL_CORE_FUN(qplc_unpack_be_27u32u),
        CALL_CORE_FUN(qplc_unpack_be_28u32u),
        CALL_CORE_FUN(qplc_unpack_be_29u32u),
        CALL_CORE_FUN(qplc_unpack_be_30u32u),
        CALL_CORE_FUN(qplc_unpack_be_31u32u),
        CALL_CORE_FUN(qplc_unpack_be_32u32u)
};

/**
 * @brief Packing input data in 8u, 16u or 32u integers format to integers of any-bit-width, LE or BE.
 *
 * @param[in]   src_ptr       pointer to source vector in 8u, 16u or 32u integers format
 * @param[in]   num_elements  number of source integers to pack
 * @param[out]  dst_ptr       pointer to packed data in any-bit-width format (LE or BE)
 * @param[in]   start_bit     bit position in the first byte of destination to start from
 *
 * @note Parameters:  (uint8_t *src_ptr, uint32_t num_elements, uint8_t *dst_ptr, uint32_t start_bit)
 * @note Pack function table contains 70 (2 * 35) entries - starts from 1-32 bit-width for LE + [8u16u|8u32u|16u32u],
 *                                                          then the same for BE output
 * @note Index calculation: outputBeShift = (QPL_FLAG_OUT_BE & qpl_job_ptr->flags) ? 32u : 0u;
 * @note                    packIndex = outputBeShift + bit_width - 1u;
 *
 * @return
 *      - n/a (void).
 */
static const qplc_pack_bits_t_ptr qplc_pack_bits_array[] = {
        CALL_CORE_FUN(qplc_pack_8u1u),
        CALL_CORE_FUN(qplc_pack_8u2u),
        CALL_CORE_FUN(qplc_pack_8u3u),
        CALL_CORE_FUN(qplc_pack_8u4u),
        CALL_CORE_FUN(qplc_pack_8u5u),
        CALL_CORE_FUN(qplc_pack_8u6u),
        CALL_CORE_FUN(qplc_pack_8u7u),
        CALL_CORE_FUN(qplc_pack_8u8u),
        CALL_CORE_FUN(qplc_pack_16u9u),
        CALL_CORE_FUN(qplc_pack_16u10u),
        CALL_CORE_FUN(qplc_pack_16u11u),
        CALL_CORE_FUN(qplc_pack_16u12u),
        CALL_CORE_FUN(qplc_pack_16u13u),
        CALL_CORE_FUN(qplc_pack_16u14u),
        CALL_CORE_FUN(qplc_pack_16u15u),
        CALL_CORE_FUN(qplc_pack_16u16u),
        CALL_CORE_FUN(qplc_pack_32u17u),
        CALL_CORE_FUN(qplc_pack_32u18u),
        CALL_CORE_FUN(qplc_pack_32u19u),
        CALL_CORE_FUN(qplc_pack_32u20u),
        CALL_CORE_FUN(qplc_pack_32u21u),
        CALL_CORE_FUN(qplc_pack_32u22u),
        CALL_CORE_FUN(qplc_pack_32u23u),
        CALL_CORE_FUN(qplc_pack_32u24u),
        CALL_CORE_FUN(qplc_pack_32u25u),
        CALL_CORE_FUN(qplc_pack_32u26u),
        CALL_CORE_FUN(qplc_pack_32u27u),
        CALL_CORE_FUN(qplc_pack_32u28u),
        CALL_CORE_FUN(qplc_pack_32u29u),
        CALL_CORE_FUN(qplc_pack_32u30u),
        CALL_CORE_FUN(qplc_pack_32u31u),
        CALL_CORE_FUN(qplc_pack_32u32u),
        CALL_CORE_FUN(qplc_pack_8u16u),
        CALL_CORE_FUN(qplc_pack_8u32u),
        CALL_CORE_FUN(qplc_pack_16u32u),
        // BE starts here
        CALL_CORE_FUN(qplc_pack_be_8u1u),
        CALL_CORE_FUN(qplc_pack_be_8u2u),
        CALL_CORE_FUN(qplc_pack_be_8u3u),
        CALL_CORE_FUN(qplc_pack_be_8u4u),
        CALL_CORE_FUN(qplc_pack_be_8u5u),
        CALL_CORE_FUN(qplc_pack_be_8u6u),
        CALL_CORE_FUN(qplc_pack_be_8u7u),
        CALL_CORE_FUN(qplc_pack_be_8u8u),
        CALL_CORE_FUN(qplc_pack_be_16u9u),
        CALL_CORE_FUN(qplc_pack_be_16u10u),
        CALL_CORE_FUN(qplc_pack_be_16u11u),
        CALL_CORE_FUN(qplc_pack_be_16u12u),
        CALL_CORE_FUN(qplc_pack_be_16u13u),
        CALL_CORE_FUN(qplc_pack_be_16u14u),
        CALL_CORE_FUN(qplc_pack_be_16u15u),
        CALL_CORE_FUN(qplc_pack_be_16u16u),
        CALL_CORE_FUN(qplc_pack_be_32u17u),
        CALL_CORE_FUN(qplc_pack_be_32u18u),
        CALL_CORE_FUN(qplc_pack_be_32u19u),
        CALL_CORE_FUN(qplc_pack_be_32u20u),
        CALL_CORE_FUN(qplc_pack_be_32u21u),
        CALL_CORE_FUN(qplc_pack_be_32u22u),
        CALL_CORE_FUN(qplc_pack_be_32u23u),
        CALL_CORE_FUN(qplc_pack_be_32u24u),
        CALL_CORE_FUN(qplc_pack_be_32u25u),
        CALL_CORE_FUN(qplc_pack_be_32u26u),
        CALL_CORE_FUN(qplc_pack_be_32u27u),
        CALL_CORE_FUN(qplc_pack_be_32u28u),
        CALL_CORE_FUN(qplc_pack_be_32u29u),
        CALL_CORE_FUN(qplc_pack_be_32u30u),
        CALL_CORE_FUN(qplc_pack_be_32u31u),
        CALL_CORE_FUN(qplc_pack_be_32u32u),
        CALL_CORE_FUN(qplc_pack_be_8u16u),
        CALL_CORE_FUN(qplc_pack_be_8u32u),
        CALL_CORE_FUN(qplc_pack_be_16u32u)
};

/*------- End qplc_api.h -------*/

#ifdef __cplusplus
}
#endif

#endif //OWN_QPL_CORE_API_H_
/** @} */
