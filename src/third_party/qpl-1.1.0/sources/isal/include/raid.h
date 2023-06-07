/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#ifndef _RAID_H_
#define _RAID_H_

/**
 *  @file  raid.h
 *  @brief Interface to RAID functions - XOR and P+Q calculation.
 *
 *  This file defines the interface to optimized XOR calculation (RAID5) or P+Q
 *  dual parity (RAID6).  Operations are carried out on an array of pointers to
 *  sources and output arrays.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Multi-binary functions */

/**
 * @brief Generate XOR parity vector from N sources, runs appropriate version.
 *
 * This function determines what instruction sets are enabled and
 * selects the appropriate version at runtime.
 *
 * @param vects   Number of source+dest vectors in array. Must be > 2.
 * @param len     Length of each vector in bytes.
 * @param array   Array of pointers to source and dest. For XOR the dest is
 *                the last pointer. ie array[vects-1]. Src and dest
 *                pointers must be aligned to 32B.
 *
 * @returns 0 pass, other fail
 */

int xor_gen(int vects, int len, void **array);


/**
 * @brief Checks that array has XOR parity sum of 0 across all vectors, runs appropriate version.
 *
 * This function determines what instruction sets are enabled and
 * selects the appropriate version at runtime.
 *
 * @param vects   Number of vectors in array. Must be > 1.
 * @param len     Length of each vector in bytes.
 * @param array   Array of pointers to vectors. Src and dest pointers
 *                must be aligned to 16B.
 *
 * @returns 0 pass, other fail
 */

int xor_check(int vects, int len, void **array);


/**
 * @brief Generate P+Q parity vectors from N sources, runs appropriate version.
 *
 * This function determines what instruction sets are enabled and
 * selects the appropriate version at runtime.
 *
 * @param vects   Number of source+dest vectors in array. Must be > 3.
 * @param len     Length of each vector in bytes. Must be 32B aligned.
 * @param array   Array of pointers to source and dest. For P+Q the dest
 *                is the last two pointers. ie array[vects-2],
 *                array[vects-1].  P and Q parity vectors are
 *                written to these last two pointers. Src and dest
 *                pointers must be aligned to 32B.
 *
 * @returns 0 pass, other fail
 */

int pq_gen(int vects, int len, void **array);


/**
 * @brief Checks that array of N sources, P and Q are consistent across all vectors, runs appropriate version.
 *
 * This function determines what instruction sets are enabled and
 * selects the appropriate version at runtime.
 *
 * @param vects  Number of vectors in array including P&Q. Must be > 3.
 * @param len    Length of each vector in bytes. Must be 16B aligned.
 * @param array  Array of pointers to source and P, Q. P and Q parity
 *               are assumed to be the last two pointers in the array.
 *               All pointers must be aligned to 16B.
 *
 * @returns 0 pass, other fail
 */

int pq_check(int vects, int len, void **array);


/* Arch specific versions */
// x86 only
#if defined(__i386__) || defined(__x86_64__)

/**
 * @brief Generate XOR parity vector from N sources.
 * @requires SSE4.1
 *
 * @param vects   Number of source+dest vectors in array. Must be > 2.
 * @param len     Length of each vector in bytes.
 * @param array   Array of pointers to source and dest. For XOR the dest is
 *                the last pointer. ie array[vects-1]. Src and dest pointers
 *                must be aligned to 16B.
 *
 * @returns 0 pass, other fail
 */

int xor_gen_sse(int vects, int len, void **array);


/**
 * @brief Generate XOR parity vector from N sources.
 * @requires AVX
 *
 * @param vects   Number of source+dest vectors in array. Must be > 2.
 * @param len     Length of each vector in bytes.
 * @param array   Array of pointers to source and dest. For XOR the dest is
 *                the last pointer. ie array[vects-1]. Src and dest pointers
 *                must be aligned to 32B.
 *
 * @returns 0 pass, other fail
 */

int xor_gen_avx(int vects, int len, void **array);


/**
 * @brief Checks that array has XOR parity sum of 0 across all vectors.
 * @requires SSE4.1
 *
 * @param vects   Number of vectors in array. Must be > 1.
 * @param len     Length of each vector in bytes.
 * @param array   Array of pointers to vectors. Src and dest pointers
 *                must be aligned to 16B.
 *
 * @returns 0 pass, other fail
 */

int xor_check_sse(int vects, int len, void **array);


/**
 * @brief Generate P+Q parity vectors from N sources.
 * @requires SSE4.1
 *
 * @param vects   Number of source+dest vectors in array. Must be > 3.
 * @param len     Length of each vector in bytes. Must be 16B aligned.
 * @param array   Array of pointers to source and dest. For P+Q the dest
 *                is the last two pointers. ie array[vects-2],
 *                array[vects-1]. P and Q parity vectors are
 *                written to these last two pointers. Src and dest
 *                pointers must be aligned to 16B.
 *
 * @returns 0 pass, other fail
 */

int pq_gen_sse(int vects, int len, void **array);


/**
 * @brief Generate P+Q parity vectors from N sources.
 * @requires AVX
 *
 * @param vects   Number of source+dest vectors in array. Must be > 3.
 * @param len     Length of each vector in bytes. Must be 16B aligned.
 * @param array   Array of pointers to source and dest. For P+Q the dest
 *                is the last two pointers. ie array[vects-2],
 *                array[vects-1]. P and Q parity vectors are
 *                written to these last two pointers. Src and dest
 *                pointers must be aligned to 16B.
 *
 * @returns 0 pass, other fail
 */

int pq_gen_avx(int vects, int len, void **array);


/**
 * @brief Generate P+Q parity vectors from N sources.
 * @requires AVX2
 *
 * @param vects   Number of source+dest vectors in array. Must be > 3.
 * @param len     Length of each vector in bytes. Must be 32B aligned.
 * @param array   Array of pointers to source and dest. For P+Q the dest
 *                is the last two pointers. ie array[vects-2],
 *                array[vects-1]. P and Q parity vectors are
 *                written to these last two pointers. Src and dest
 *                pointers must be aligned to 32B.
 *
 * @returns 0 pass, other fail
 */

int pq_gen_avx2(int vects, int len, void **array);


/**
 * @brief Checks that array of N sources, P and Q are consistent across all vectors.
 * @requires SSE4.1
 *
 * @param vects  Number of vectors in array including P&Q. Must be > 3.
 * @param len    Length of each vector in bytes. Must be 16B aligned.
 * @param array  Array of pointers to source and P, Q. P and Q parity
                 are assumed to be the last two pointers in the array.
                 All pointers must be aligned to 16B.
 * @returns 0 pass, other fail
 */

int pq_check_sse(int vects, int len, void **array);

#endif

/**
 * @brief Generate P+Q parity vectors from N sources, runs baseline version.
 * @param vects   Number of source+dest vectors in array. Must be > 3.
 * @param len     Length of each vector in bytes. Must be 16B aligned.
 * @param array   Array of pointers to source and dest. For P+Q the dest
 * 		  is the last two pointers. ie array[vects-2],
 * 		  array[vects-1]. P and Q parity vectors are
 * 		  written to these last two pointers. Src and dest pointers
 * 		  must be aligned to 16B.
 *
 * @returns 0 pass, other fail
 */

int pq_gen_base(int vects, int len, void **array);


/**
 * @brief Generate XOR parity vector from N sources, runs baseline version.
 * @param vects   Number of source+dest vectors in array. Must be > 2.
 * @param len     Length of each vector in bytes.
 * @param array   Array of pointers to source and dest. For XOR the dest is
 * 		  the last pointer. ie array[vects-1]. Src and dest pointers
 * 		  must be aligned to 32B.
 *
 * @returns 0 pass, other fail
 */

int xor_gen_base(int vects, int len, void **array);


/**
 * @brief Checks that array has XOR parity sum of 0 across all vectors, runs baseline version.
 *
 * @param vects   Number of vectors in array. Must be > 1.
 * @param len     Length of each vector in bytes.
 * @param array   Array of pointers to vectors. Src and dest pointers
 *                must be aligned to 16B.
 *
 * @returns 0 pass, other fail
 */

int xor_check_base(int vects, int len, void **array);


/**
 * @brief Checks that array of N sources, P and Q are consistent across all vectors, runs baseline version.
 *
 * @param vects  Number of vectors in array including P&Q. Must be > 3.
 * @param len    Length of each vector in bytes. Must be 16B aligned.
 * @param array  Array of pointers to source and P, Q. P and Q parity
 *               are assumed to be the last two pointers in the array.
 *               All pointers must be aligned to 16B.
 *
 * @returns 0 pass, other fail
 */

int pq_check_base(int vects, int len, void **array);

#ifdef __cplusplus
}
#endif

#endif //_RAID_H_
