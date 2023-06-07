/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/


/**
 *  @file  crc64.h
 *  @brief CRC64 functions.
 */


#ifndef _CRC64_H_
#define _CRC64_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif


/* Multi-binary functions */

/**
 * @brief Generate CRC from ECMA-182 standard in reflected format, runs
 * appropriate version.
 *
 * This function determines what instruction sets are enabled and
 * selects the appropriate version at runtime.
 * @returns 64 bit CRC
 */
uint64_t crc64_ecma_refl(
	uint64_t init_crc,        //!< initial CRC value, 64 bits
	const unsigned char *buf, //!< buffer to calculate CRC on
	uint64_t len              //!< buffer length in bytes (64-bit data)
	);

/**
 * @brief Generate CRC from ECMA-182 standard in normal format, runs
 * appropriate version.
 *
 * This function determines what instruction sets are enabled and
 * selects the appropriate version at runtime.
 * @returns 64 bit CRC
 */
uint64_t crc64_ecma_norm(
	uint64_t init_crc,        //!< initial CRC value, 64 bits
	const unsigned char *buf, //!< buffer to calculate CRC on
	uint64_t len              //!< buffer length in bytes (64-bit data)
	);

/**
 * @brief Generate CRC from ISO standard in reflected format, runs
 * appropriate version.
 *
 * This function determines what instruction sets are enabled and
 * selects the appropriate version at runtime.
 * @returns 64 bit CRC
 */
uint64_t crc64_iso_refl(
	uint64_t init_crc,        //!< initial CRC value, 64 bits
	const unsigned char *buf, //!< buffer to calculate CRC on
	uint64_t len              //!< buffer length in bytes (64-bit data)
	);

/**
 * @brief Generate CRC from ISO standard in normal format, runs
 * appropriate version.
 *
 * This function determines what instruction sets are enabled and
 * selects the appropriate version at runtime.
 * @returns 64 bit CRC
 */
uint64_t crc64_iso_norm(
	uint64_t init_crc,        //!< initial CRC value, 64 bits
	const unsigned char *buf, //!< buffer to calculate CRC on
	uint64_t len              //!< buffer length in bytes (64-bit data)
	);

/**
 * @brief Generate CRC from "Jones" coefficients in reflected format, runs
 * appropriate version.
 *
 * This function determines what instruction sets are enabled and
 * selects the appropriate version at runtime.
 * @returns 64 bit CRC
 */
uint64_t crc64_jones_refl(
	uint64_t init_crc,        //!< initial CRC value, 64 bits
	const unsigned char *buf, //!< buffer to calculate CRC on
	uint64_t len              //!< buffer length in bytes (64-bit data)
	);

/**
 * @brief Generate CRC from "Jones" coefficients in normal format, runs
 * appropriate version.
 *
 * This function determines what instruction sets are enabled and
 * selects the appropriate version at runtime.
 * @returns 64 bit CRC
 */
uint64_t crc64_jones_norm(
	uint64_t init_crc,        //!< initial CRC value, 64 bits
	const unsigned char *buf, //!< buffer to calculate CRC on
	uint64_t len              //!< buffer length in bytes (64-bit data)
	);

/* Arch specific versions */

/**
 * @brief Generate CRC from ECMA-182 standard in reflected format.
 * @requires SSE3, CLMUL
 *
 * @returns 64 bit CRC
 */

uint64_t crc64_ecma_refl_by8(
	uint64_t init_crc,        //!< initial CRC value, 64 bits
	const unsigned char *buf, //!< buffer to calculate CRC on
	uint64_t len              //!< buffer length in bytes (64-bit data)
	);

/**
 * @brief Generate CRC from ECMA-182 standard in normal format.
 * @requires SSE3, CLMUL
 *
 * @returns 64 bit CRC
 */

uint64_t crc64_ecma_norm_by8(
	uint64_t init_crc,        //!< initial CRC value, 64 bits
	const unsigned char *buf, //!< buffer to calculate CRC on
	uint64_t len              //!< buffer length in bytes (64-bit data)
	);

/**
 * @brief Generate CRC from ECMA-182 standard in reflected format, runs baseline version
 * @returns 64 bit CRC
 */
uint64_t crc64_ecma_refl_base(
	uint64_t init_crc,        //!< initial CRC value, 64 bits
	const unsigned char *buf, //!< buffer to calculate CRC on
	uint64_t len              //!< buffer length in bytes (64-bit data)
	);

/**
 * @brief Generate CRC from ECMA-182 standard in normal format, runs baseline version
 * @returns 64 bit CRC
 */
uint64_t crc64_ecma_norm_base(
	uint64_t init_crc,        //!< initial CRC value, 64 bits
	const unsigned char *buf, //!< buffer to calculate CRC on
	uint64_t len              //!< buffer length in bytes (64-bit data)
	);

/**
 * @brief Generate CRC from ISO standard in reflected format.
 * @requires SSE3, CLMUL
 *
 * @returns 64 bit CRC
 */

uint64_t crc64_iso_refl_by8(
	uint64_t init_crc,        //!< initial CRC value, 64 bits
	const unsigned char *buf, //!< buffer to calculate CRC on
	uint64_t len              //!< buffer length in bytes (64-bit data)
	);

/**
 * @brief Generate CRC from ISO standard in normal format.
 * @requires SSE3, CLMUL
 *
 * @returns 64 bit CRC
 */

uint64_t crc64_iso_norm_by8(
	uint64_t init_crc,        //!< initial CRC value, 64 bits
	const unsigned char *buf, //!< buffer to calculate CRC on
	uint64_t len              //!< buffer length in bytes (64-bit data)
	);

/**
 * @brief Generate CRC from ISO standard in reflected format, runs baseline version
 * @returns 64 bit CRC
 */
uint64_t crc64_iso_refl_base(
	uint64_t init_crc,        //!< initial CRC value, 64 bits
	const unsigned char *buf, //!< buffer to calculate CRC on
	uint64_t len              //!< buffer length in bytes (64-bit data)
	);

/**
 * @brief Generate CRC from ISO standard in normal format, runs baseline version
 * @returns 64 bit CRC
 */
uint64_t crc64_iso_norm_base(
	uint64_t init_crc,        //!< initial CRC value, 64 bits
	const unsigned char *buf, //!< buffer to calculate CRC on
	uint64_t len              //!< buffer length in bytes (64-bit data)
	);

/**
 * @brief Generate CRC from "Jones" coefficients in reflected format.
 * @requires SSE3, CLMUL
 *
 * @returns 64 bit CRC
 */

uint64_t crc64_jones_refl_by8(
	uint64_t init_crc,        //!< initial CRC value, 64 bits
	const unsigned char *buf, //!< buffer to calculate CRC on
	uint64_t len              //!< buffer length in bytes (64-bit data)
	);

/**
 * @brief Generate CRC from "Jones" coefficients in normal format.
 * @requires SSE3, CLMUL
 *
 * @returns 64 bit CRC
 */

uint64_t crc64_jones_norm_by8(
	uint64_t init_crc,        //!< initial CRC value, 64 bits
	const unsigned char *buf, //!< buffer to calculate CRC on
	uint64_t len              //!< buffer length in bytes (64-bit data)
	);

/**
 * @brief Generate CRC from "Jones" coefficients in reflected format, runs baseline version
 * @returns 64 bit CRC
 */
uint64_t crc64_jones_refl_base(
	uint64_t init_crc,        //!< initial CRC value, 64 bits
	const unsigned char *buf, //!< buffer to calculate CRC on
	uint64_t len              //!< buffer length in bytes (64-bit data)
	);

/**
 * @brief Generate CRC from "Jones" coefficients in normal format, runs baseline version
 * @returns 64 bit CRC
 */
uint64_t crc64_jones_norm_base(
	uint64_t init_crc,        //!< initial CRC value, 64 bits
	const unsigned char *buf, //!< buffer to calculate CRC on
	uint64_t len              //!< buffer length in bytes (64-bit data)
	);

#ifdef __cplusplus
}
#endif

#endif // _CRC64_H_
