/*******************************************************************************
* Copyright (C) 2022 Intel Corporation
*
* SPDX-License-Identifier: MIT
******************************************************************************/

/**
 * @brief Contains Intel® Query Processing Library (Intel® QPL) Core API for checksum
 * @date 03/17/2021
 *
 * @details Function list:
 *          - @ref qplc_crc32_8u
 *          - @ref qplc_crc32_byte_8u
 *          - @ref qplc_crc32_with_polynomial_32u
 *          - @ref qplc_xor_checksum_8u
 *
 */

#include "qplc_defines.h"

#ifndef QPLC_CHECKSUM_H_
#define QPLC_CHECKSUM_H_

#ifdef __cplusplus
extern "C" {
#endif

typedef uint64_t (*qplc_crc64_t_ptr)(const uint8_t *src_ptr,
                                     uint32_t length,
                                     uint64_t polynomial,
                                     uint8_t be_flag,
                                     uint8_t inversion_flag);

typedef uint32_t (*qplc_xor_checksum_t_ptr)(const uint8_t *buf,
                                            uint32_t len,
                                            uint32_t init_xor);

/**
* @brief XOR checksum calculation for data buffer
*
* @param[in]  buf       - pointer to the data buffer
* @param[in]  len       - len of the buffer
* @param[in]  init_xor  - intialization value for Xor checksum
*
* @return XOR checksum value
*/
OWN_QPLC_API(uint32_t, qplc_xor_checksum_8u, (const uint8_t* buf,
        uint32_t len,
        uint32_t init_xor))

/*
 * @brief CRC64 checksum calculation for data buffer
 *
 * @param[in]  src_ptr          - pointer to the data buffer
 * @param[in]  length           - length of the buffer
 * @param[in]  polynomial       - 64-bit CRC polynomial
 * @param[in]  be_flag          - endianness flag:
 *                                  0 - little endian format;
 *                                  1 - big endian format;
 * @param[in]  inversion_flag   - bitwise inversion flag:
 *                                  0 - no inversion;
 *                                  1 - bitwise inversion of the initial and final CRC;
 *
 * @return CRC64 checksum value
 */
OWN_QPLC_API(uint64_t, qplc_crc64, (const uint8_t *src_ptr,
        uint32_t length,
        uint64_t polynomial,
        uint8_t be_flag,
        uint8_t inversion_flag))

#ifdef __cplusplus
}
#endif

#endif // QPLC_CHECKSUM_H_
/** @} */
