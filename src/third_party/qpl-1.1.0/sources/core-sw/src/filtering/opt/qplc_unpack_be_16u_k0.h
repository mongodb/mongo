/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/**
 * @brief Contains implementation of functions for unpacking 9..16-bit BE data to words
 * @date 07/06/2020
 *
 * @details Function list:
 *          - @ref k0_qplc_unpack_be_9u16u
 *          - @ref k0_qplc_unpack_be_10u16u
 *          - @ref k0_qplc_unpack_be_11u16u
 *          - @ref k0_qplc_unpack_be_12u16u
 *          - @ref k0_qplc_unpack_be_13u16u
 *          - @ref k0_qplc_unpack_be_14u16u
 *          - @ref k0_qplc_unpack_be_15u16u
 *          - @ref k0_qplc_unpack_be_16u16u
 *
 */

#include "own_qplc_defs.h"

// ------------------------------------ 9u -----------------------------------------
OWN_ALIGNED_64_ARRAY(static uint16_t permutex_idx_table_9u_0[32]) = {
    0u, 1u, 1u, 2u, 2u, 3u, 3u, 4u, 4u, 5u, 5u, 6u, 6u, 7u, 7u, 8u, 9u, 10u, 10u, 11u, 11u, 12u, 12u, 13u, 13u, 14u, 14u, 15u, 15u, 16u, 16u, 17u};
OWN_ALIGNED_64_ARRAY(static uint16_t permutex_idx_table_9u_1[32]) = {
    0u, 1u, 1u, 2u, 2u, 3u, 3u, 4u, 5u, 6u, 6u, 7u, 7u, 8u, 8u, 9u, 9u, 10u, 10u, 11u, 11u, 12u, 12u, 13u, 14u, 15u, 15u, 16u, 16u, 17u, 17u, 18u};
OWN_ALIGNED_64_ARRAY(static uint32_t shift_table_9u_0[16]) = {
    0u, 2u, 4u, 6u, 8u, 10u, 12u, 14u, 0u, 2u, 4u, 6u, 8u, 10u, 12u, 14u};
OWN_ALIGNED_64_ARRAY(static uint32_t shift_table_9u_1[16]) = {
    7u, 5u, 3u, 1u, 15u, 13u, 11u, 9u, 7u, 5u, 3u, 1u, 15u, 13u, 11u, 9u};

OWN_ALIGNED_64_ARRAY(static uint8_t shuffle_idx_table_9u_0[64]) = {
    1u, 0u, 2u, 1u, 3u, 2u, 4u, 3u, 5u, 4u, 6u, 5u, 7u, 6u, 8u, 7u,
    1u, 0u, 2u, 1u, 3u, 2u, 4u, 3u, 5u, 4u, 6u, 5u, 7u, 6u, 8u, 7u,
    1u, 0u, 2u, 1u, 3u, 2u, 4u, 3u, 5u, 4u, 6u, 5u, 7u, 6u, 8u, 7u,
    1u, 0u, 2u, 1u, 3u, 2u, 4u, 3u, 5u, 4u, 6u, 5u, 7u, 6u, 8u, 7u};
OWN_ALIGNED_64_ARRAY(static uint16_t shift_table_9u_2[32]) = {
    7u, 6u, 5u, 4u, 3u, 2u, 1u, 0u, 7u, 6u, 5u, 4u, 3u, 2u, 1u, 0u, 7u, 6u, 5u, 4u, 3u, 2u, 1u, 0u, 7u, 6u, 5u, 4u, 3u, 2u, 1u, 0u};
OWN_ALIGNED_64_ARRAY(static uint64_t gather_idx_table_9u[8]) = {
    0u, 8u, 9u, 17u, 18u, 26u, 27u, 35u};

// ------------------------------------ 10u -----------------------------------------
OWN_ALIGNED_64_ARRAY(static uint8_t shuffle_idx_table_10u_0[64]) = {
    1u, 0u, 2u, 1u, 3u, 2u, 4u, 3u, 6u, 5u, 7u, 6u, 8u, 7u, 9u, 8u,
    1u, 0u, 2u, 1u, 3u, 2u, 4u, 3u, 6u, 5u, 7u, 6u, 8u, 7u, 9u, 8u,
    1u, 0u, 2u, 1u, 3u, 2u, 4u, 3u, 6u, 5u, 7u, 6u, 8u, 7u, 9u, 8u,
    1u, 0u, 2u, 1u, 3u, 2u, 4u, 3u, 6u, 5u, 7u, 6u, 8u, 7u, 9u, 8u};
OWN_ALIGNED_64_ARRAY(static uint16_t shift_table_10u[32]) = {
    6u, 4u, 2u, 0u, 6u, 4u, 2u, 0u, 6u, 4u, 2u, 0u, 6u, 4u, 2u, 0u, 6u, 4u, 2u, 0u, 6u, 4u, 2u, 0u, 6u, 4u, 2u, 0u, 6u, 4u, 2u, 0u};
OWN_ALIGNED_64_ARRAY(static uint16_t permutex_idx_table_10u[32]) = {
    0u, 1u, 2u, 3u, 4u, 0x0, 0x0, 0x0, 5u, 6u, 7u, 8u, 9u, 0x0, 0x0, 0x0,
    10u, 11u, 12u, 13u, 14u, 0x0, 0x0, 0x0, 15u, 16u, 17u, 18u, 19u, 0x0, 0x0, 0x0};

// ------------------------------------ 11u -----------------------------------------
OWN_ALIGNED_64_ARRAY(static uint16_t permutex_idx_table_11u_0[32]) = {
    0u, 1u, 1u, 2u, 2u, 3u, 4u, 5u, 5u, 6u, 6u, 7u, 8u, 9u, 9u, 10u, 11u, 12u, 12u, 13u, 13u, 14u, 15u, 16u, 16u, 17u, 17u, 18u, 19u, 20u, 20u, 21u};
OWN_ALIGNED_64_ARRAY(static uint16_t permutex_idx_table_11u_1[32]) = {
    0u, 1u, 2u, 3u, 3u, 4u, 4u, 5u, 6u, 7u, 7u, 8u, 8u, 9u, 10u, 11u, 11u, 12u, 13u, 14u, 14u, 15u, 15u, 16u, 17u, 18u, 18u, 19u, 19u, 20u, 21u, 22u};
OWN_ALIGNED_64_ARRAY(static uint32_t shift_table_11u_0[16]) = {
    0u, 6u, 12u, 2u, 8u, 14u, 4u, 10u, 0u, 6u, 12u, 2u, 8u, 14u, 4u, 10u};
OWN_ALIGNED_64_ARRAY(static uint32_t shift_table_11u_1[16]) = {
    5u, 15u, 9u, 3u, 13u, 7u, 1u, 11u, 5u, 15u, 9u, 3u, 13u, 7u, 1u, 11u};

OWN_ALIGNED_64_ARRAY(static uint8_t shuffle_idx_table_11u_0[64]) = {
    3u, 2u, 1u, 0u, 5u, 4u, 3u, 2u, 8u, 7u, 6u, 5u, 11u, 10u, 9u, 8u,
    3u, 2u, 1u, 0u, 5u, 4u, 3u, 2u, 8u, 7u, 6u, 5u, 11u, 10u, 9u, 8u,
    3u, 2u, 1u, 0u, 5u, 4u, 3u, 2u, 8u, 7u, 6u, 5u, 11u, 10u, 9u, 8u,
    3u, 2u, 1u, 0u, 5u, 4u, 3u, 2u, 8u, 7u, 6u, 5u, 11u, 10u, 9u, 8u};
OWN_ALIGNED_64_ARRAY(static uint8_t shuffle_idx_table_11u_1[64]) = {
    3u, 2u, 1u, 0u, 6u, 5u, 4u, 0u, 8u, 7u, 6u, 0u, 11u, 10u, 9u, 0u,
    3u, 2u, 1u, 0u, 6u, 5u, 4u, 0u, 8u, 7u, 6u, 0u, 11u, 10u, 9u, 0u,
    3u, 2u, 1u, 0u, 6u, 5u, 4u, 0u, 8u, 7u, 6u, 0u, 11u, 10u, 9u, 0u,
    3u, 2u, 1u, 0u, 6u, 5u, 4u, 0u, 8u, 7u, 6u, 0u, 11u, 10u, 9u, 0u};
OWN_ALIGNED_64_ARRAY(static uint32_t shift_table_11u_2[16]) = {
    21u, 15u, 17u, 19u, 21u, 15u, 17u, 19u, 21u, 15u, 17u, 19u, 21u, 15u, 17u, 19u};
OWN_ALIGNED_64_ARRAY(static uint32_t shift_table_11u_3[16]) = {
    6u, 4u, 10u, 8u, 6u, 4u, 10u, 8u, 6u, 4u, 10u, 8u, 6u, 4u, 10u, 8u};
OWN_ALIGNED_64_ARRAY(static uint64_t gather_idx_table_11u[8]) = {
    0u, 8u, 11u, 19u, 22u, 30u, 33u, 41u};

// ------------------------------------ 12u -----------------------------------------
OWN_ALIGNED_64_ARRAY(static uint8_t shuffle_idx_table_12u_0[64]) = {
    1u, 0u, 2u, 1u, 4u, 3u, 5u, 4u, 7u, 6u, 8u, 7u, 10u, 9u, 11u, 10u,
    1u, 0u, 2u, 1u, 4u, 3u, 5u, 4u, 7u, 6u, 8u, 7u, 10u, 9u, 11u, 10u,
    1u, 0u, 2u, 1u, 4u, 3u, 5u, 4u, 7u, 6u, 8u, 7u, 10u, 9u, 11u, 10u,
    1u, 0u, 2u, 1u, 4u, 3u, 5u, 4u, 7u, 6u, 8u, 7u, 10u, 9u, 11u, 10u};
OWN_ALIGNED_64_ARRAY(static uint16_t shift_table_12u[32]) = {
    4u, 0u, 4u, 0u, 4u, 0u, 4u, 0u, 4u, 0u, 4u, 0u, 4u, 0u, 4u, 0u, 4u, 0u, 4u, 0u, 4u, 0u, 4u, 0u, 4u, 0u, 4u, 0u, 4u, 0u, 4u, 0u};
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_12u[16]) = {
    0u, 1u, 2u, 0x0, 3u, 4u, 5u, 0x0, 6u, 7u, 8u, 0x0, 9u, 10u, 11u, 0x0};

// ------------------------------------ 13u -----------------------------------------
OWN_ALIGNED_64_ARRAY(static uint16_t permutex_idx_table_13u_0[32]) = {
    0u, 1u, 1u, 2u, 3u, 4u, 4u, 5u, 6u, 7u, 8u, 9u, 9u, 10u, 11u, 12u,
    13u, 14u, 14u, 15u, 16u, 17u, 17u, 18u, 19u, 20u, 21u, 22u, 22u, 23u, 24u, 25u};
OWN_ALIGNED_64_ARRAY(static uint16_t permutex_idx_table_13u_1[32]) = {
    0u, 1u, 2u, 3u, 4u, 5u, 5u, 6u, 7u, 8u, 8u, 9u, 10u, 11u, 12u, 13u,
    13u, 14u, 15u, 16u, 17u, 18u, 18u, 19u, 20u, 21u, 21u, 22u, 23u, 24u, 25u, 26u};
OWN_ALIGNED_64_ARRAY(static uint32_t shift_table_13u_0[16]) = {
    0u, 10u, 4u, 14u, 8u, 2u, 12u, 6u, 0u, 10u, 4u, 14u, 8u, 2u, 12u, 6u};
OWN_ALIGNED_64_ARRAY(static uint32_t shift_table_13u_1[16]) = {
    3u, 9u, 15u, 5u, 11u, 1u, 7u, 13u, 3u, 9u, 15u, 5u, 11u, 1u, 7u, 13u};

OWN_ALIGNED_64_ARRAY(static uint8_t shuffle_idx_table_13u_0[64]) = {
    3u, 2u, 1u, 0u, 6u, 5u, 4u, 3u, 9u, 8u, 7u, 6u, 12u, 11u, 10u, 9u,
    3u, 2u, 1u, 0u, 6u, 5u, 4u, 3u, 9u, 8u, 7u, 6u, 12u, 11u, 10u, 9u,
    3u, 2u, 1u, 0u, 6u, 5u, 4u, 3u, 9u, 8u, 7u, 6u, 12u, 11u, 10u, 9u,
    3u, 2u, 1u, 0u, 6u, 5u, 4u, 3u, 9u, 8u, 7u, 6u, 12u, 11u, 10u, 9u};
OWN_ALIGNED_64_ARRAY(static uint8_t shuffle_idx_table_13u_1[64]) = {
    3u, 2u, 1u, 0u, 6u, 5u, 4u, 0u, 10u, 9u, 8u, 0u, 13u, 12u, 11u, 0u,
    3u, 2u, 1u, 0u, 6u, 5u, 4u, 0u, 10u, 9u, 8u, 0u, 13u, 12u, 11u, 0u,
    3u, 2u, 1u, 0u, 6u, 5u, 4u, 0u, 10u, 9u, 8u, 0u, 13u, 12u, 11u, 0u,
    3u, 2u, 1u, 0u, 6u, 5u, 4u, 0u, 10u, 9u, 8u, 0u, 13u, 12u, 11u, 0u};
OWN_ALIGNED_64_ARRAY(static uint32_t shift_table_13u_2[16]) = {
    19u, 17u, 15u, 13u, 19u, 17u, 15u, 13u, 19u, 17u, 15u, 13u, 19u, 17u, 15u, 13u};
OWN_ALIGNED_64_ARRAY(static uint32_t shift_table_13u_3[16]) = {
    10u, 12u, 6u, 8u, 10u, 12u, 6u, 8u, 10u, 12u, 6u, 8u, 10u, 12u, 6u, 8u};
OWN_ALIGNED_64_ARRAY(static uint64_t gather_idx_table_13u[8]) = {
    0u, 8u, 13u, 21u, 26u, 34u, 39u, 47u};

// ------------------------------------ 14u -----------------------------------------
OWN_ALIGNED_64_ARRAY(static uint8_t shuffle_idx_table_14u_0[64]) = {
    3u, 2u, 1u, 0u, 6u, 5u, 4u, 3u, 10u, 9u, 8u, 7u, 13u, 12u, 11u, 10u,
    3u, 2u, 1u, 0u, 6u, 5u, 4u, 3u, 10u, 9u, 8u, 7u, 13u, 12u, 11u, 10u,
    3u, 2u, 1u, 0u, 6u, 5u, 4u, 3u, 10u, 9u, 8u, 7u, 13u, 12u, 11u, 10u,
    3u, 2u, 1u, 0u, 6u, 5u, 4u, 3u, 10u, 9u, 8u, 7u, 13u, 12u, 11u, 10u};
OWN_ALIGNED_64_ARRAY(static uint8_t shuffle_idx_table_14u_1[64]) = {
    3u, 2u, 1u, 0u, 7u, 6u, 5u, 0u, 10u, 9u, 8u, 0u, 14u, 13u, 12u, 0u,
    3u, 2u, 1u, 0u, 7u, 6u, 5u, 0u, 10u, 9u, 8u, 0u, 14u, 13u, 12u, 0u,
    3u, 2u, 1u, 0u, 7u, 6u, 5u, 0u, 10u, 9u, 8u, 0u, 14u, 13u, 12u, 0u,
    3u, 2u, 1u, 0u, 7u, 6u, 5u, 0u, 10u, 9u, 8u, 0u, 14u, 13u, 12u, 0u};
OWN_ALIGNED_64_ARRAY(static uint32_t shift_table_14u_0[16]) = {
    18u, 14u, 18u, 14u, 18u, 14u, 18u, 14u, 18u, 14u, 18u, 14u, 18u, 14u, 18u, 14u};
OWN_ALIGNED_64_ARRAY(static uint32_t shift_table_14u_1[16]) = {
    12u, 8u, 12u, 8u, 12u, 8u, 12u, 8u, 12u, 8u, 12u, 8u, 12u, 8u, 12u, 8u};
OWN_ALIGNED_64_ARRAY(static uint16_t permutex_idx_table_14u[32]) = {
    0u, 1u, 2u, 3u, 4u, 5u, 6u, 0x0, 7u, 8u, 9u, 10u, 11u, 12u, 13u, 0x0,
    14u, 15u, 16u, 17u, 18u, 19u, 20u, 0x0, 21u, 22u, 23u, 24u, 25u, 26u, 27u, 0x0};

// ------------------------------------ 15u -----------------------------------------
OWN_ALIGNED_64_ARRAY(static uint16_t permutex_idx_table_15u_0[32]) = {
    0u, 1u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u, 12u, 13u, 14u, 
    15u, 16u, 16u, 17u, 18u, 19u, 20u, 21u, 22u, 23u, 24u, 25u, 26u, 27u, 28u, 29u};
OWN_ALIGNED_64_ARRAY(static uint16_t permutex_idx_table_15u_1[32]) = {
    0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u, 12u, 13u, 14u, 15u, 
    15u, 16u, 17u, 18u, 19u, 20u, 21u, 22u, 23u, 24u, 25u, 26u, 27u, 28u, 29u, 30u};
OWN_ALIGNED_64_ARRAY(static uint32_t shift_table_15u_0[16]) = {
    0u, 14u, 12u, 10u, 8u, 6u, 4u, 2u, 0u, 14u, 12u, 10u, 8u, 6u, 4u, 2u};
OWN_ALIGNED_64_ARRAY(static uint32_t shift_table_15u_1[16]) = {
    1u, 3u, 5u, 7u, 9u, 11u, 13u, 15u, 1u, 3u, 5u, 7u, 9u, 11u, 13u, 15u};

OWN_ALIGNED_64_ARRAY(static uint8_t shuffle_idx_table_15u_0[64]) = {
    3u, 2u, 1u, 0u, 6u, 5u, 4u, 3u, 10u, 9u, 8u, 7u, 14u, 13u, 12u, 11u,
    3u, 2u, 1u, 0u, 6u, 5u, 4u, 3u, 10u, 9u, 8u, 7u, 14u, 13u, 12u, 11u,
    3u, 2u, 1u, 0u, 6u, 5u, 4u, 3u, 10u, 9u, 8u, 7u, 14u, 13u, 12u, 11u,
    3u, 2u, 1u, 0u, 6u, 5u, 4u, 3u, 10u, 9u, 8u, 7u, 14u, 13u, 12u, 11u};
OWN_ALIGNED_64_ARRAY(static uint8_t shuffle_idx_table_15u_1[64]) = {
    3u, 2u, 1u, 0u, 7u, 6u, 5u, 0u, 11u, 10u, 9u, 0u, 15u, 14u, 13u, 0u,
    3u, 2u, 1u, 0u, 7u, 6u, 5u, 0u, 11u, 10u, 9u, 0u, 15u, 14u, 13u, 0u,
    3u, 2u, 1u, 0u, 7u, 6u, 5u, 0u, 11u, 10u, 9u, 0u, 15u, 14u, 13u, 0u,
    3u, 2u, 1u, 0u, 7u, 6u, 5u, 0u, 11u, 10u, 9u, 0u, 15u, 14u, 13u, 0u};
OWN_ALIGNED_64_ARRAY(static uint32_t shift_table_15u_2[16]) = {
    17u, 11u, 13u, 15u, 17u, 11u, 13u, 15u, 17u, 11u, 13u, 15u, 17u, 11u, 13u, 15u};
OWN_ALIGNED_64_ARRAY(static uint32_t shift_table_15u_3[16]) = {
    14u, 12u, 10u, 8u, 14u, 12u, 10u, 8u, 14u, 12u, 10u, 8u, 14u, 12u, 10u, 8u};
OWN_ALIGNED_64_ARRAY(static uint64_t gather_idx_table_15u[8]) = {
    0u, 8u, 15u, 23u, 30u, 38u, 45u, 53u};

/*
0 -> 00
1 -> 08
2 -> 04
3 -> 0C
4 -> 02
5 -> 0A
6 -> 06
7 -> 0E
8 -> 01
9 -> 09
A -> 05
B -> 0D
C -> 03
D -> 0B
E -> 07
F -> 0F
*/
OWN_ALIGNED_64_ARRAY(static uint64_t nibble_reverse_table[8]) = {
        0x0E060A020C040800,
        0x0F070B030D050901,
        0x0E060A020C040800,
        0x0F070B030D050901,
        0x0E060A020C040800,
        0x0F070B030D050901,
        0x0E060A020C040800,
        0x0F070B030D050901
};

OWN_ALIGNED_64_ARRAY(static uint64_t reverse_mask_table_16u[8]) = {
        0x0607040502030001,
        0x0E0F0C0D0A0B0809,
        0x1617141512131011,
        0x1E1F1C1D1A1B1819,
        0x2627242522232021,
        0x2E2F2C2D2A2B2829,
        0x3637343532333031,
        0x3E3F3C3D3A3B3839
};

OWN_QPLC_INLINE(uint32_t, own_get_align, (uint32_t start_bit, uint32_t base, uint32_t bitsize)) {
    uint32_t remnant = bitsize - start_bit;
    for (uint32_t i = 0u; i < bitsize; ++i) {
        uint32_t test_value = (i * base) % bitsize;
        if (test_value == remnant) {
            return i;
        }
    }
    return 0xFFFFFFFF;
}

// For BE start_bit is bit index from the top of a byte
OWN_QPLC_INLINE(void, px_qplc_unpack_be_Nu16u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint32_t bit_width,
        uint8_t *dst_ptr)) {
    uint16_t *src16u_ptr = (uint16_t *) src_ptr;
    uint16_t *dst16u_ptr = (uint16_t *) dst_ptr;
    uint32_t bits_in_buf = OWN_WORD_WIDTH - start_bit;
    uint32_t shift       = OWN_DWORD_WIDTH - bit_width;
    uint32_t src         = ((uint32_t) qplc_swap_bytes_16u(*src16u_ptr)) << (OWN_DWORD_WIDTH - bits_in_buf);
    uint32_t next_word;

    src16u_ptr++;

    while (1u < num_elements) {
        if (bit_width > bits_in_buf) {
            next_word = (uint32_t) qplc_swap_bytes_16u(*src16u_ptr);
            src16u_ptr++;
            next_word = next_word << (OWN_WORD_WIDTH - bits_in_buf);
            src       = src | next_word;
            bits_in_buf += OWN_WORD_WIDTH;
        }
        *dst16u_ptr = (uint16_t) (src >> shift);
        src = src << bit_width;
        bits_in_buf -= bit_width;
        dst16u_ptr++;
        num_elements--;
    }

    uint8_t *src8u_ptr = (uint8_t *) src16u_ptr;
    if (bit_width > bits_in_buf) {
        uint32_t bytes_to_read = OWN_BITS_2_BYTE(bit_width - bits_in_buf);
        if (bytes_to_read == 2u) {
            next_word = *((uint16_t *) src8u_ptr);
            src8u_ptr += 2;
        } else {
            next_word = *src8u_ptr;
            src8u_ptr++;
        }
        next_word              = (uint32_t) qplc_swap_bytes_16u(next_word);
        next_word              = next_word << (OWN_WORD_WIDTH - bits_in_buf);
        src                    = src | next_word;
        bits_in_buf += OWN_WORD_WIDTH;
    }
    *dst16u_ptr = (uint16_t) (src >> shift);
    src = src << bit_width;
    bits_in_buf -= bit_width;
    dst16u_ptr++;
    num_elements--;
}

// ********************** 9u ****************************** //

OWN_OPT_FUN(void, k0_qplc_unpack_be_9u16u, (const uint8_t *src_ptr,
    uint32_t num_elements,
    uint32_t start_bit,
    uint8_t *dst_ptr)) {
    if (start_bit > 0u) {
        uint32_t align = own_get_align(start_bit, 9u, 16u);
        if (align > num_elements) {
            align = num_elements;
        }
        px_qplc_unpack_be_Nu16u(src_ptr, align, start_bit, 9u, dst_ptr);
        src_ptr += ((align * 9u) + start_bit) >> 3;
        dst_ptr += align * 2;
        num_elements -= align;
    }

    if (num_elements >= 32u) {
        __mmask32 read_mask = OWN_BIT_MASK(OWN_BITS_2_WORD(9u * OWN_DWORD_WIDTH));
        __m512i   parse_mask0 = _mm512_set1_epi16(OWN_BIT_MASK(9u));
        __m512i   nibble_reversemm = _mm512_load_si512(nibble_reverse_table);
        __m512i   reverse_mask_16u = _mm512_load_si512(reverse_mask_table_16u);
        __m512i   maskmm = _mm512_set1_epi8(0x0F);

        __m512i   shuffle_idx_ptr = _mm512_load_si512(shuffle_idx_table_9u_0);

        __m512i   permutex_idx_ptr[2];
        permutex_idx_ptr[0] = _mm512_load_si512(permutex_idx_table_9u_0);
        permutex_idx_ptr[1] = _mm512_load_si512(permutex_idx_table_9u_1);

        __m512i   shift_mask_ptr[3];
        shift_mask_ptr[0] = _mm512_load_si512(shift_table_9u_0);
        shift_mask_ptr[1] = _mm512_load_si512(shift_table_9u_1);
        shift_mask_ptr[2] = _mm512_load_si512(shift_table_9u_2);

        __m512i   gather_idxmm = _mm512_load_si512(gather_idx_table_9u);

        while (num_elements >= 64u) {
            __m512i srcmm, zmm[2];

            srcmm = _mm512_i64gather_epi64(gather_idxmm, src_ptr, 1u);

            zmm[0] = _mm512_shuffle_epi8(srcmm, shuffle_idx_ptr);

            // shifting elements so they start from the start of the word
            zmm[0] = _mm512_srlv_epi16(zmm[0], shift_mask_ptr[2]);
            zmm[0] = _mm512_and_si512(zmm[0], parse_mask0);

            _mm512_storeu_si512(dst_ptr, zmm[0]);

            src_ptr += 4u * 9u;
            dst_ptr += 64u;
            num_elements -= 32u;
        }
        if (num_elements >= 32u) {
            __m512i srcmm, zmm[2];

            srcmm = _mm512_maskz_loadu_epi16(read_mask, src_ptr);

            __m512i low_nibblemm = _mm512_and_si512(srcmm, maskmm);
            __m512i high_nibblemm = _mm512_srli_epi16(srcmm, 4u);
            high_nibblemm = _mm512_and_si512(high_nibblemm, maskmm);

            low_nibblemm = _mm512_shuffle_epi8(nibble_reversemm, low_nibblemm);
            high_nibblemm = _mm512_shuffle_epi8(nibble_reversemm, high_nibblemm);
            low_nibblemm = _mm512_slli_epi16(low_nibblemm, 4u);

            srcmm = _mm512_or_si512(low_nibblemm, high_nibblemm);

            // permuting so in zmm[0] will be elements with even indexes and in zmm[1] - with odd ones
            zmm[0] = _mm512_permutexvar_epi16(permutex_idx_ptr[0], srcmm);
            zmm[1] = _mm512_permutexvar_epi16(permutex_idx_ptr[1], srcmm);

            // shifting elements so they start from the start of the word
            zmm[0] = _mm512_srlv_epi32(zmm[0], shift_mask_ptr[0]);
            zmm[1] = _mm512_sllv_epi32(zmm[1], shift_mask_ptr[1]);

            // gathering even and odd elements together
            zmm[0] = _mm512_mask_mov_epi16(zmm[0], 0xAAAAAAAA, zmm[1]);
            zmm[0] = _mm512_and_si512(zmm[0], parse_mask0);

            zmm[0] = _mm512_slli_epi16(zmm[0], 7u);

            low_nibblemm = _mm512_and_si512(zmm[0], maskmm);
            high_nibblemm = _mm512_srli_epi16(zmm[0], 4u);
            high_nibblemm = _mm512_and_si512(high_nibblemm, maskmm);

            low_nibblemm = _mm512_shuffle_epi8(nibble_reversemm, low_nibblemm);
            high_nibblemm = _mm512_shuffle_epi8(nibble_reversemm, high_nibblemm);
            low_nibblemm = _mm512_slli_epi16(low_nibblemm, 4u);

            zmm[0] = _mm512_or_si512(low_nibblemm, high_nibblemm);
            zmm[0] = _mm512_shuffle_epi8(zmm[0], reverse_mask_16u);

            _mm512_storeu_si512(dst_ptr, zmm[0]);

            src_ptr += 4u * 9u;
            dst_ptr += 64u;
            num_elements -= 32u;
        }
    }

    if (num_elements > 0) {
        px_qplc_unpack_be_Nu16u(src_ptr, num_elements, 0u, 9u, dst_ptr);
    }
}

// ********************** 10u ****************************** //

OWN_OPT_FUN(void, k0_qplc_unpack_be_10u16u, (const uint8_t *src_ptr,
    uint32_t num_elements,
    uint32_t start_bit,
    uint8_t *dst_ptr)) {
    if (start_bit > 0u) {
        uint32_t align = own_get_align(start_bit, 10u, 16u);
        if (align > num_elements) {
            align = num_elements;
        }
        px_qplc_unpack_be_Nu16u(src_ptr, align, start_bit, 10u, dst_ptr);
        src_ptr += ((align * 10u) + start_bit) >> 3;
        dst_ptr += align * 2;
        num_elements -= align;
    }

    if (num_elements >= 32u) {
        __mmask32 read_mask = OWN_BIT_MASK(OWN_BITS_2_WORD(10u * OWN_DWORD_WIDTH));
        __m512i   parse_mask0 = _mm512_set1_epi16(OWN_BIT_MASK(10u));

        __m512i   shuffle_idx_ptr = _mm512_load_si512(shuffle_idx_table_10u_0);
        __m512i   permutex_idx = _mm512_load_si512(permutex_idx_table_10u);
        __m512i   shift_mask = _mm512_load_si512(shift_table_10u);

        while (num_elements >= 32u) {
            __m512i srcmm, zmm;

            srcmm = _mm512_maskz_loadu_epi16(read_mask, src_ptr);

            zmm = _mm512_permutexvar_epi16(permutex_idx, srcmm);
            zmm = _mm512_shuffle_epi8(zmm, shuffle_idx_ptr);

            // shifting elements so they start from the start of the word
            zmm = _mm512_srlv_epi16(zmm, shift_mask);
            zmm = _mm512_and_si512(zmm, parse_mask0);

            _mm512_storeu_si512(dst_ptr, zmm);

            src_ptr += 4u * 10u;
            dst_ptr += 64u;
            num_elements -= 32u;
        }
    }

    if (num_elements > 0) {
        px_qplc_unpack_be_Nu16u(src_ptr, num_elements, 0u, 10u, dst_ptr);
    }
}

// ********************** 11u ****************************** //

OWN_OPT_FUN(void, k0_qplc_unpack_be_11u16u, (const uint8_t *src_ptr,
    uint32_t num_elements,
    uint32_t start_bit,
    uint8_t *dst_ptr)) {
    if (start_bit > 0u) {
        uint32_t align = own_get_align(start_bit, 11u, 16u);
        if (align > num_elements) {
            align = num_elements;
        }
        px_qplc_unpack_be_Nu16u(src_ptr, align, start_bit, 11u, dst_ptr);
        src_ptr += ((align * 11u) + start_bit) >> 3;
        dst_ptr += align * 2;
        num_elements -= align;
    }

    if (num_elements >= 32u) {
        __mmask32 read_mask = OWN_BIT_MASK(OWN_BITS_2_WORD(11u * OWN_DWORD_WIDTH));
        __m512i   parse_mask0 = _mm512_set1_epi16(OWN_BIT_MASK(11u));
        __m512i   nibble_reversemm = _mm512_load_si512(nibble_reverse_table);
        __m512i   reverse_mask_16u = _mm512_load_si512(reverse_mask_table_16u);
        __m512i   maskmm = _mm512_set1_epi8(0x0F);

        __m512i   shuffle_idx_ptr[2];
        shuffle_idx_ptr[0] = _mm512_load_si512(shuffle_idx_table_11u_0);
        shuffle_idx_ptr[1] = _mm512_load_si512(shuffle_idx_table_11u_1);

        __m512i   permutex_idx_ptr[2];
        permutex_idx_ptr[0] = _mm512_load_si512(permutex_idx_table_11u_0);
        permutex_idx_ptr[1] = _mm512_load_si512(permutex_idx_table_11u_1);

        __m512i   shift_mask_ptr[4];
        shift_mask_ptr[0] = _mm512_load_si512(shift_table_11u_0);
        shift_mask_ptr[1] = _mm512_load_si512(shift_table_11u_1);
        shift_mask_ptr[2] = _mm512_load_si512(shift_table_11u_2);
        shift_mask_ptr[3] = _mm512_load_si512(shift_table_11u_3);

        __m512i   gather_idxmm = _mm512_load_si512(gather_idx_table_11u);

        while (num_elements >= 64u) {
            __m512i srcmm, zmm[2];

            srcmm = _mm512_i64gather_epi64(gather_idxmm, src_ptr, 1u);

            // shuffling so in zmm[0] will be elements with even indexes and in zmm[1] - with odd ones
            zmm[0] = _mm512_shuffle_epi8(srcmm, shuffle_idx_ptr[0]);
            zmm[1] = _mm512_shuffle_epi8(srcmm, shuffle_idx_ptr[1]);

            // shifting elements so they start from the start of the word
            zmm[0] = _mm512_srlv_epi32(zmm[0], shift_mask_ptr[2]);
            zmm[1] = _mm512_sllv_epi32(zmm[1], shift_mask_ptr[3]);

            // gathering even and odd elements together
            zmm[0] = _mm512_mask_mov_epi16(zmm[0], 0xAAAAAAAA, zmm[1]);
            zmm[0] = _mm512_and_si512(zmm[0], parse_mask0);

            _mm512_storeu_si512(dst_ptr, zmm[0]);

            src_ptr += 4u * 11u;
            dst_ptr += 64u;
            num_elements -= 32u;
        }
        if (num_elements >= 32u) {
            __m512i srcmm, zmm[2];

            srcmm = _mm512_maskz_loadu_epi16(read_mask, src_ptr);

            __m512i low_nibblemm = _mm512_and_si512(srcmm, maskmm);
            __m512i high_nibblemm = _mm512_srli_epi16(srcmm, 4u);
            high_nibblemm = _mm512_and_si512(high_nibblemm, maskmm);

            low_nibblemm = _mm512_shuffle_epi8(nibble_reversemm, low_nibblemm);
            high_nibblemm = _mm512_shuffle_epi8(nibble_reversemm, high_nibblemm);
            low_nibblemm = _mm512_slli_epi16(low_nibblemm, 4u);

            srcmm = _mm512_or_si512(low_nibblemm, high_nibblemm);

            // permuting so in zmm[0] will be elements with even indexes and in zmm[1] - with odd ones
            zmm[0] = _mm512_permutexvar_epi16(permutex_idx_ptr[0], srcmm);
            zmm[1] = _mm512_permutexvar_epi16(permutex_idx_ptr[1], srcmm);

            // shifting elements so they start from the start of the word
            zmm[0] = _mm512_srlv_epi32(zmm[0], shift_mask_ptr[0]);
            zmm[1] = _mm512_sllv_epi32(zmm[1], shift_mask_ptr[1]);

            // gathering even and odd elements together
            zmm[0] = _mm512_mask_mov_epi16(zmm[0], 0xAAAAAAAA, zmm[1]);
            zmm[0] = _mm512_and_si512(zmm[0], parse_mask0);

            zmm[0] = _mm512_slli_epi16(zmm[0], 5u);

            low_nibblemm = _mm512_and_si512(zmm[0], maskmm);
            high_nibblemm = _mm512_srli_epi16(zmm[0], 4u);
            high_nibblemm = _mm512_and_si512(high_nibblemm, maskmm);

            low_nibblemm = _mm512_shuffle_epi8(nibble_reversemm, low_nibblemm);
            high_nibblemm = _mm512_shuffle_epi8(nibble_reversemm, high_nibblemm);
            low_nibblemm = _mm512_slli_epi16(low_nibblemm, 4u);

            zmm[0] = _mm512_or_si512(low_nibblemm, high_nibblemm);
            zmm[0] = _mm512_shuffle_epi8(zmm[0], reverse_mask_16u);

            _mm512_storeu_si512(dst_ptr, zmm[0]);

            src_ptr += 4u * 11u;
            dst_ptr += 64u;
            num_elements -= 32u;
        }
    }

    if (num_elements > 0) {
        px_qplc_unpack_be_Nu16u(src_ptr, num_elements, 0u, 11u, dst_ptr);
    }
}

// ********************** 12u ****************************** //

OWN_OPT_FUN(void, k0_qplc_unpack_be_12u16u, (const uint8_t *src_ptr,
    uint32_t num_elements,
    uint32_t start_bit,
    uint8_t *dst_ptr)) {
    if (start_bit > 0u) {
        uint32_t align = own_get_align(start_bit, 12u, 16u);
        if (align > num_elements) {
            align = num_elements;
        }
        px_qplc_unpack_be_Nu16u(src_ptr, align, start_bit, 12u, dst_ptr);
        src_ptr += ((align * 12u) + start_bit) >> 3;
        dst_ptr += align * 2;
        num_elements -= align;
    }

    if (num_elements >= 32u) {
        __mmask32 read_mask = OWN_BIT_MASK(OWN_BITS_2_WORD(12u * OWN_DWORD_WIDTH));
        __m512i   parse_mask0 = _mm512_set1_epi16(OWN_BIT_MASK(12u));

        __m512i   shuffle_idx_ptr = _mm512_load_si512(shuffle_idx_table_12u_0);
        __m512i   permutex_idx = _mm512_load_si512(permutex_idx_table_12u);
        __m512i   shift_mask = _mm512_load_si512(shift_table_12u);

        while (num_elements >= 32u) {
            __m512i srcmm, zmm;

            srcmm = _mm512_maskz_loadu_epi16(read_mask, src_ptr);

            zmm = _mm512_permutexvar_epi32(permutex_idx, srcmm);
            zmm = _mm512_shuffle_epi8(zmm, shuffle_idx_ptr);

            // shifting elements so they start from the start of the word
            zmm = _mm512_srlv_epi16(zmm, shift_mask);
            zmm = _mm512_and_si512(zmm, parse_mask0);

            _mm512_storeu_si512(dst_ptr, zmm);

            src_ptr += 4u * 12u;
            dst_ptr += 64u;
            num_elements -= 32u;
        }
    }

    if (num_elements > 0) {
        px_qplc_unpack_be_Nu16u(src_ptr, num_elements, 0u, 12u, dst_ptr);
    }
}

// ********************** 13u ****************************** //

OWN_OPT_FUN(void, k0_qplc_unpack_be_13u16u, (const uint8_t *src_ptr,
    uint32_t num_elements,
    uint32_t start_bit,
    uint8_t *dst_ptr)) {
    if (start_bit > 0u) {
        uint32_t align = own_get_align(start_bit, 13u, 16u);
        if (align > num_elements) {
            align = num_elements;
        }
        px_qplc_unpack_be_Nu16u(src_ptr, align, start_bit, 13u, dst_ptr);
        src_ptr += ((align * 13u) + start_bit) >> 3;
        dst_ptr += align * 2;
        num_elements -= align;
    }

    if (num_elements >= 32u) {
        __mmask32 read_mask = OWN_BIT_MASK(OWN_BITS_2_WORD(13u * OWN_DWORD_WIDTH));
        __m512i   parse_mask0 = _mm512_set1_epi16(OWN_BIT_MASK(13u));
        __m512i   nibble_reversemm = _mm512_load_si512(nibble_reverse_table);
        __m512i   reverse_mask_16u = _mm512_load_si512(reverse_mask_table_16u);
        __m512i   maskmm = _mm512_set1_epi8(0x0F);

        __m512i   shuffle_idx_ptr[2];
        shuffle_idx_ptr[0] = _mm512_load_si512(shuffle_idx_table_13u_0);
        shuffle_idx_ptr[1] = _mm512_load_si512(shuffle_idx_table_13u_1);

        __m512i   permutex_idx_ptr[2];
        permutex_idx_ptr[0] = _mm512_load_si512(permutex_idx_table_13u_0);
        permutex_idx_ptr[1] = _mm512_load_si512(permutex_idx_table_13u_1);

        __m512i   shift_mask_ptr[4];
        shift_mask_ptr[0] = _mm512_load_si512(shift_table_13u_0);
        shift_mask_ptr[1] = _mm512_load_si512(shift_table_13u_1);
        shift_mask_ptr[2] = _mm512_load_si512(shift_table_13u_2);
        shift_mask_ptr[3] = _mm512_load_si512(shift_table_13u_3);

        __m512i   gather_idxmm = _mm512_load_si512(gather_idx_table_13u);

        while (num_elements >= 64u) {
            __m512i srcmm, zmm[2];

            srcmm = _mm512_i64gather_epi64(gather_idxmm, src_ptr, 1u);

            // shuffling so in zmm[0] will be elements with even indexes and in zmm[1] - with odd ones
            zmm[0] = _mm512_shuffle_epi8(srcmm, shuffle_idx_ptr[0]);
            zmm[1] = _mm512_shuffle_epi8(srcmm, shuffle_idx_ptr[1]);

            // shifting elements so they start from the start of the word
            zmm[0] = _mm512_srlv_epi32(zmm[0], shift_mask_ptr[2]);
            zmm[1] = _mm512_sllv_epi32(zmm[1], shift_mask_ptr[3]);

            // gathering even and odd elements together
            zmm[0] = _mm512_mask_mov_epi16(zmm[0], 0xAAAAAAAA, zmm[1]);
            zmm[0] = _mm512_and_si512(zmm[0], parse_mask0);

            _mm512_storeu_si512(dst_ptr, zmm[0]);

            src_ptr += 4u * 13u;
            dst_ptr += 64u;
            num_elements -= 32u;
        }
        if (num_elements >= 32u) {
            __m512i srcmm, zmm[2];

            srcmm = _mm512_maskz_loadu_epi16(read_mask, src_ptr);

            __m512i low_nibblemm = _mm512_and_si512(srcmm, maskmm);
            __m512i high_nibblemm = _mm512_srli_epi16(srcmm, 4u);
            high_nibblemm = _mm512_and_si512(high_nibblemm, maskmm);

            low_nibblemm = _mm512_shuffle_epi8(nibble_reversemm, low_nibblemm);
            high_nibblemm = _mm512_shuffle_epi8(nibble_reversemm, high_nibblemm);
            low_nibblemm = _mm512_slli_epi16(low_nibblemm, 4u);

            srcmm = _mm512_or_si512(low_nibblemm, high_nibblemm);

            // permuting so in zmm[0] will be elements with even indexes and in zmm[1] - with odd ones
            zmm[0] = _mm512_permutexvar_epi16(permutex_idx_ptr[0], srcmm);
            zmm[1] = _mm512_permutexvar_epi16(permutex_idx_ptr[1], srcmm);

            // shifting elements so they start from the start of the word
            zmm[0] = _mm512_srlv_epi32(zmm[0], shift_mask_ptr[0]);
            zmm[1] = _mm512_sllv_epi32(zmm[1], shift_mask_ptr[1]);

            // gathering even and odd elements together
            zmm[0] = _mm512_mask_mov_epi16(zmm[0], 0xAAAAAAAA, zmm[1]);
            zmm[0] = _mm512_and_si512(zmm[0], parse_mask0);

            zmm[0] = _mm512_slli_epi16(zmm[0], 3u);

            low_nibblemm = _mm512_and_si512(zmm[0], maskmm);
            high_nibblemm = _mm512_srli_epi16(zmm[0], 4u);
            high_nibblemm = _mm512_and_si512(high_nibblemm, maskmm);

            low_nibblemm = _mm512_shuffle_epi8(nibble_reversemm, low_nibblemm);
            high_nibblemm = _mm512_shuffle_epi8(nibble_reversemm, high_nibblemm);
            low_nibblemm = _mm512_slli_epi16(low_nibblemm, 4u);

            zmm[0] = _mm512_or_si512(low_nibblemm, high_nibblemm);
            zmm[0] = _mm512_shuffle_epi8(zmm[0], reverse_mask_16u);

            _mm512_storeu_si512(dst_ptr, zmm[0]);

            src_ptr += 4u * 13u;
            dst_ptr += 64u;
            num_elements -= 32u;
        }
    }

    if (num_elements > 0) {
        px_qplc_unpack_be_Nu16u(src_ptr, num_elements, 0u, 13u, dst_ptr);
    }
}

// ********************** 14u ****************************** //

OWN_OPT_FUN(void, k0_qplc_unpack_be_14u16u, (const uint8_t *src_ptr,
    uint32_t num_elements,
    uint32_t start_bit,
    uint8_t *dst_ptr)) {
    if (start_bit > 0u) {
        uint32_t align = own_get_align(start_bit, 14u, 16u);
        if (align > num_elements) {
            align = num_elements;
        }
        px_qplc_unpack_be_Nu16u(src_ptr, align, start_bit, 14u, dst_ptr);
        src_ptr += ((align * 14u) + start_bit) >> 3;
        dst_ptr += align * 2;
        num_elements -= align;
    }

    if (num_elements >= 32u) {
        __mmask32 read_mask = OWN_BIT_MASK(OWN_BITS_2_WORD(14u * OWN_DWORD_WIDTH));
        __m512i   parse_mask0 = _mm512_set1_epi16(OWN_BIT_MASK(14u));

        __m512i   shuffle_idx_ptr[2];
        shuffle_idx_ptr[0] = _mm512_load_si512(shuffle_idx_table_14u_0);
        shuffle_idx_ptr[1] = _mm512_load_si512(shuffle_idx_table_14u_1);

        __m512i   permutex_idx = _mm512_load_si512(permutex_idx_table_14u);

        __m512i   shift_mask_ptr[2];
        shift_mask_ptr[0] = _mm512_load_si512(shift_table_14u_0);
        shift_mask_ptr[1] = _mm512_load_si512(shift_table_14u_1);

        while (num_elements >= 32u) {
            __m512i srcmm, zmm[2];

            srcmm = _mm512_maskz_loadu_epi16(read_mask, src_ptr);
            srcmm = _mm512_permutexvar_epi16(permutex_idx, srcmm);

            // shuffling so in zmm[0] will be elements with even indexes and in zmm[1] - with odd ones
            zmm[0] = _mm512_shuffle_epi8(srcmm, shuffle_idx_ptr[0]);
            zmm[1] = _mm512_shuffle_epi8(srcmm, shuffle_idx_ptr[1]);

            // shifting elements so they start from the start of the word
            zmm[0] = _mm512_srlv_epi32(zmm[0], shift_mask_ptr[0]);
            zmm[1] = _mm512_sllv_epi32(zmm[1], shift_mask_ptr[1]);

            // gathering even and odd elements together
            zmm[0] = _mm512_mask_mov_epi16(zmm[0], 0xAAAAAAAA, zmm[1]);
            zmm[0] = _mm512_and_si512(zmm[0], parse_mask0);

            _mm512_storeu_si512(dst_ptr, zmm[0]);

            src_ptr += 4u * 14u;
            dst_ptr += 64u;
            num_elements -= 32u;
        }
    }

    if (num_elements > 0) {
        px_qplc_unpack_be_Nu16u(src_ptr, num_elements, 0u, 14u, dst_ptr);
    }
}

// ********************** 15u ****************************** //

OWN_OPT_FUN(void, k0_qplc_unpack_be_15u16u, (const uint8_t *src_ptr,
    uint32_t num_elements,
    uint32_t start_bit,
    uint8_t *dst_ptr)) {
    if (start_bit > 0u) {
        uint32_t align = own_get_align(start_bit, 15u, 16u);
        if (align > num_elements) {
            align = num_elements;
        }
        px_qplc_unpack_be_Nu16u(src_ptr, align, start_bit, 15u, dst_ptr);
        src_ptr += ((align * 15u) + start_bit) >> 3;
        dst_ptr += align * 2;
        num_elements -= align;
    }

    if (num_elements >= 32u) {
        __mmask32 read_mask = OWN_BIT_MASK(OWN_BITS_2_WORD(15u * OWN_DWORD_WIDTH));
        __m512i   parse_mask0 = _mm512_set1_epi16(OWN_BIT_MASK(15u));
        __m512i   nibble_reversemm = _mm512_load_si512(nibble_reverse_table);
        __m512i   reverse_mask_16u = _mm512_load_si512(reverse_mask_table_16u);
        __m512i   maskmm = _mm512_set1_epi8(0x0F);

        __m512i   shuffle_idx_ptr[2];
        shuffle_idx_ptr[0] = _mm512_load_si512(shuffle_idx_table_15u_0);
        shuffle_idx_ptr[1] = _mm512_load_si512(shuffle_idx_table_15u_1);

        __m512i   permutex_idx_ptr[2];
        permutex_idx_ptr[0] = _mm512_load_si512(permutex_idx_table_15u_0);
        permutex_idx_ptr[1] = _mm512_load_si512(permutex_idx_table_15u_1);

        __m512i   shift_mask_ptr[4];
        shift_mask_ptr[0] = _mm512_load_si512(shift_table_15u_0);
        shift_mask_ptr[1] = _mm512_load_si512(shift_table_15u_1);
        shift_mask_ptr[2] = _mm512_load_si512(shift_table_15u_2);
        shift_mask_ptr[3] = _mm512_load_si512(shift_table_15u_3);

        __m512i   gather_idxmm = _mm512_load_si512(gather_idx_table_15u);

        while (num_elements >= 64u) {
            __m512i srcmm, zmm[2];

            srcmm = _mm512_i64gather_epi64(gather_idxmm, src_ptr, 1u);

            // shuffling so in zmm[0] will be elements with even indexes and in zmm[1] - with odd ones
            zmm[0] = _mm512_shuffle_epi8(srcmm, shuffle_idx_ptr[0]);
            zmm[1] = _mm512_shuffle_epi8(srcmm, shuffle_idx_ptr[1]);

            // shifting elements so they start from the start of the word
            zmm[0] = _mm512_srlv_epi32(zmm[0], shift_mask_ptr[2]);
            zmm[1] = _mm512_sllv_epi32(zmm[1], shift_mask_ptr[3]);

            // gathering even and odd elements together
            zmm[0] = _mm512_mask_mov_epi16(zmm[0], 0xAAAAAAAA, zmm[1]);
            zmm[0] = _mm512_and_si512(zmm[0], parse_mask0);

            _mm512_storeu_si512(dst_ptr, zmm[0]);

            src_ptr += 4u * 15u;
            dst_ptr += 64u;
            num_elements -= 32u;
        }
        if (num_elements >= 32u) {
            __m512i srcmm, zmm[2];

            srcmm = _mm512_maskz_loadu_epi16(read_mask, src_ptr);

            __m512i low_nibblemm = _mm512_and_si512(srcmm, maskmm);
            __m512i high_nibblemm = _mm512_srli_epi16(srcmm, 4u);
            high_nibblemm = _mm512_and_si512(high_nibblemm, maskmm);

            low_nibblemm = _mm512_shuffle_epi8(nibble_reversemm, low_nibblemm);
            high_nibblemm = _mm512_shuffle_epi8(nibble_reversemm, high_nibblemm);
            low_nibblemm = _mm512_slli_epi16(low_nibblemm, 4u);

            srcmm = _mm512_or_si512(low_nibblemm, high_nibblemm);

            // permuting so in zmm[0] will be elements with even indexes and in zmm[1] - with odd ones
            zmm[0] = _mm512_permutexvar_epi16(permutex_idx_ptr[0], srcmm);
            zmm[1] = _mm512_permutexvar_epi16(permutex_idx_ptr[1], srcmm);

            // shifting elements so they start from the start of the word
            zmm[0] = _mm512_srlv_epi32(zmm[0], shift_mask_ptr[0]);
            zmm[1] = _mm512_sllv_epi32(zmm[1], shift_mask_ptr[1]);

            // gathering even and odd elements together
            zmm[0] = _mm512_mask_mov_epi16(zmm[0], 0xAAAAAAAA, zmm[1]);
            zmm[0] = _mm512_and_si512(zmm[0], parse_mask0);

            zmm[0] = _mm512_slli_epi16(zmm[0], 1u);

            low_nibblemm = _mm512_and_si512(zmm[0], maskmm);
            high_nibblemm = _mm512_srli_epi16(zmm[0], 4u);
            high_nibblemm = _mm512_and_si512(high_nibblemm, maskmm);

            low_nibblemm = _mm512_shuffle_epi8(nibble_reversemm, low_nibblemm);
            high_nibblemm = _mm512_shuffle_epi8(nibble_reversemm, high_nibblemm);
            low_nibblemm = _mm512_slli_epi16(low_nibblemm, 4u);

            zmm[0] = _mm512_or_si512(low_nibblemm, high_nibblemm);
            zmm[0] = _mm512_shuffle_epi8(zmm[0], reverse_mask_16u);

            _mm512_storeu_si512(dst_ptr, zmm[0]);

            src_ptr += 4u * 15u;
            dst_ptr += 64u;
            num_elements -= 32u;
        }
    }

    if (num_elements > 0) {
        px_qplc_unpack_be_Nu16u(src_ptr, num_elements, 0u, 15u, dst_ptr);
    }
}

// ********************** 16u ****************************** //

OWN_OPT_FUN(void, k0_qplc_unpack_be_16u16u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr)) {
    if (num_elements >= 32u) {
        __m512i reverse_mask_16u = _mm512_load_si512(reverse_mask_table_16u);
        while (num_elements >= 32u) {
            __m512i srcmm = _mm512_loadu_si512(src_ptr);
            srcmm = _mm512_shuffle_epi8(srcmm, reverse_mask_16u);
            _mm512_storeu_si512(dst_ptr, srcmm);

            src_ptr += 4u * 16u;
            dst_ptr += 64u;
            num_elements -= 32u;
        }
    }

    uint16_t *src16u_ptr = (uint16_t *) src_ptr;
    uint16_t *dst16u_ptr = (uint16_t *) dst_ptr;

    for (uint32_t i = 0u; i < num_elements; i++) {
        dst16u_ptr[i] = qplc_swap_bytes_16u(src16u_ptr[i]);
    }
}
