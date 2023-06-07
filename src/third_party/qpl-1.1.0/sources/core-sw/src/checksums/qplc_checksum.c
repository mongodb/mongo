/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/**
 * @brief Contains implementation of functions for checksum
 * @date 03/17/2021
 *
 * @details Function list:
 *          - @ref qplc_crc32_8u
 *          - @ref qplc_crc32_byte_8u
 *          - @ref qplc_crc32_with_polynomial_32u
 *          - @ref qplc_xor_checksum_8u
 *
 */

#include "own_qplc_defs.h"
#include "own_qplc_data.h"

#if PLATFORM >= K0

#include "opt/qplc_checksum_k0.h"

#endif

/**
 * @brief XOR checksum calculation for data buffer
 *
 * @param[in]  src_ptr   - pointer to the data buffer
 * @param[in]  length    - len of the buffer
 * @param[in]  init_xor  - intialization value for Xor checksum
 *
 * @return XOR checksum value
 */
OWN_QPLC_FUN(uint32_t, qplc_xor_checksum_8u,
    (const uint8_t* src_ptr, uint32_t length, uint32_t init_xor)) {

#if !(defined _MSC_VER)
#if PLATFORM >= K0
	return CALL_OPT_FUNCTION(k0_qplc_xor_checksum_8u)(src_ptr, length, init_xor);
#else
	uint32_t checksum = init_xor;

	for (uint32_t i = 0; i < (length & ~1); i += 2) {
		checksum ^= (uint32_t)(*(uint16_t*)(src_ptr + i));
	}

	if (length & 1) {
		checksum ^= (uint32_t)src_ptr[length - 1];
	}

	return checksum;
#endif
#else
#if _MSC_VER <= 1916
	uint32_t checksum = init_xor;

	for (uint32_t i = 0; i < (length & ~1); i += 2) {
		checksum ^= (uint32_t)(*(uint16_t*)(src_ptr + i));
	}

	if (length & 1) {
		checksum ^= (uint32_t)src_ptr[length - 1];
	}

	return checksum;
#else
#if PLATFORM >= K0
	return CALL_OPT_FUNCTION(k0_qplc_xor_checksum_8u)(src_ptr, length, init_xor);
#else
	uint32_t checksum = init_xor;

	for (uint32_t i = 0; i < (length & ~1); i += 2) {
		checksum ^= (uint32_t)(*(uint16_t*)(src_ptr + i));
	}

	if (length & 1) {
		checksum ^= (uint32_t)src_ptr[length - 1];
	}

	return checksum;
#endif
#endif
#endif
}

#if PLATFORM < K0

/**
 * @brief helper for bits/bytes reflecting
 */
static uint64_t own_bit_byte_swap_64(uint64_t x) {
    uint64_t y;

    y = bit_reverse_table[x >> 56];
    y |= ((uint64_t)bit_reverse_table[(x >> 48) & 0xFF]) << 8;
    y |= ((uint64_t)bit_reverse_table[(x >> 40) & 0xFF]) << 16;
    y |= ((uint64_t)bit_reverse_table[(x >> 32) & 0xFF]) << 24;
    y |= ((uint64_t)bit_reverse_table[(x >> 24) & 0xFF]) << 32;
    y |= ((uint64_t)bit_reverse_table[(x >> 16) & 0xFF]) << 40;
    y |= ((uint64_t)bit_reverse_table[(x >> 8) & 0xFF]) << 48;
    y |= ((uint64_t)bit_reverse_table[(x >> 0) & 0xFF]) << 56;

    return y;
}

/**
 * @brief table initializer
 */
static void own_crc64_init_lookup_table(uint64_t *table, uint64_t polynomial, uint8_t be_flag) {
    uint64_t crc = 0;
    uint64_t i = 0;
    uint32_t j = 0;

    table[0] = 0;

    if (be_flag) {
        polynomial = own_bit_byte_swap_64(polynomial);
        for (i = 1; i < 256; i++) {
            crc = i;
            for (j = 0; j < 8; j++) {
                if (crc & 0x0000000000000001ULL) {
                    crc = (crc >> 1) ^ polynomial;
                }
                else {
                    crc = (crc >> 1);
                }
            }
            table[i] = crc;
        }
    }
    else {
        for (i = 1; i < 256; i++) {
            crc = i << 56;
            for (j = 0; j < 8; j++) {
                if (crc & 0x8000000000000000ULL) {
                    crc = (crc << 1) ^ polynomial;
                }
                else {
                    crc = (crc << 1);
                }
            }
            table[i] = crc;
        }
    }
}

/**
 * @brief CRC64 initializer
 */
static uint64_t own_crc64_init_crc(uint64_t polynomial, uint8_t be_flag, uint8_t inversion_flag) {
    if (!inversion_flag) {
        return 0;
    }

    polynomial |= (polynomial << 1);
    polynomial |= (polynomial << 2);
    polynomial |= (polynomial << 4);
    polynomial |= (polynomial << 8);
    polynomial |= (polynomial << 16);
    polynomial |= (polynomial << 32);

    if (!be_flag) {
        return polynomial;
    }

    return own_bit_byte_swap_64(polynomial);
}

/**
 * @brief CRC64 calculator
 */
static uint64_t own_crc64_update(uint8_t data, uint64_t *lookup_table, uint64_t crc, uint8_t be_flag) {
    if (be_flag) {
        return lookup_table[data ^ (crc & 0xFF)] ^ (crc >> 8);
    }
    else {
        return lookup_table[data ^ (crc >> 56)] ^ (crc << 8);
    }
}

/**
 * @brief Final CRC64 step
 */
static uint64_t own_crc64_finalize(uint64_t crc, uint64_t polynomial, uint8_t be_flag, uint8_t inversion_flag) {
    if (!inversion_flag) {
        return crc;
    }
    polynomial |= (polynomial << 1);
    polynomial |= (polynomial << 2);
    polynomial |= (polynomial << 4);
    polynomial |= (polynomial << 8);
    polynomial |= (polynomial << 16);
    polynomial |= (polynomial << 32);
    if (!be_flag) {
        return crc ^ polynomial;
    }

    return crc ^ own_bit_byte_swap_64(polynomial);
}

#endif

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
OWN_QPLC_FUN(uint64_t, qplc_crc64, (const uint8_t *src_ptr, 
                                    uint32_t length, 
                                    uint64_t polynomial, 
                                    uint8_t be_flag, 
                                    uint8_t inversion_flag)) {
    uint64_t crc;
#if PLATFORM >= K0
    if (be_flag) {
        crc = CALL_OPT_FUNCTION(k0_qplc_crc64_be)(src_ptr, length, polynomial, inversion_flag);
        return crc;
    }
    else {
        crc = CALL_OPT_FUNCTION(k0_qplc_crc64)(src_ptr, length, polynomial, inversion_flag);
        return crc;
    }
#else
    uint64_t lookup_table[256];

    own_crc64_init_lookup_table(lookup_table, polynomial, be_flag);
    crc = own_crc64_init_crc(polynomial, be_flag, inversion_flag);

    for (uint32_t i = 0; i < length; i++) {
        crc = own_crc64_update(src_ptr[i], lookup_table, crc, be_flag);
    }

    crc = own_crc64_finalize(crc, polynomial, be_flag, inversion_flag);
    return crc;
#endif
}
