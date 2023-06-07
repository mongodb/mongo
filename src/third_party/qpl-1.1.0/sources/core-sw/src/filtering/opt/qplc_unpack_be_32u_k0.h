/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/**
 * @brief Contains implementation of functions for unpacking 17..32-bit BE data to dwords
 * @date 07/06/2020
 *
 * @details Function list:
 *          - @ref qplc_unpack_be_17u32u
 *          - @ref k0_qplc_unpack_be_18u32u
 *          - @ref k0_qplc_unpack_be_19u32u
 *          - @ref k0_qplc_unpack_be_20u32u
 *          - @ref k0_qplc_unpack_be_21u32u
 *          - @ref k0_qplc_unpack_be_22u32u
 *          - @ref k0_qplc_unpack_be_23u32u
 *          - @ref k0_qplc_unpack_be_24u32u
 *          - @ref k0_qplc_unpack_be_25u32u
 *          - @ref k0_qplc_unpack_be_26u32u
 *          - @ref k0_qplc_unpack_be_27u32u
 *          - @ref k0_qplc_unpack_be_28u32u
 *          - @ref k0_qplc_unpack_be_29u32u
 *          - @ref k0_qplc_unpack_be_30u32u
 *          - @ref k0_qplc_unpack_be_31u32u
 *          - @ref k0_qplc_unpack_be_32u32u
 *
 */

#include "own_qplc_defs.h"

// ------------------------------------ 17u -----------------------------------------
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_17u_0[16]) = {
    0u, 1u, 1u, 2u, 2u, 3u, 3u, 4u, 4u, 5u, 5u, 6u, 6u, 7u, 7u, 8u};
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_17u_1[16]) = {
    0u, 1u, 1u, 2u, 2u, 3u, 3u, 4u, 4u, 5u, 5u, 6u, 6u, 7u, 7u, 8u};
OWN_ALIGNED_64_ARRAY(static uint64_t shift_table_17u_0[8]) = {
    0u, 2u, 4u, 6u, 8u, 10u, 12u, 14u};
OWN_ALIGNED_64_ARRAY(static uint64_t shift_table_17u_1[8]) = {
    15u, 13u, 11u, 9u, 7u, 5u, 3u, 1u};

OWN_ALIGNED_64_ARRAY(static uint8_t shuffle_idx_table_17u_0[64]) = {
    3u, 2u, 1u, 0u, 5u, 4u, 3u, 2u, 7u, 6u, 5u, 4u, 9u, 8u, 7u, 6u, 3u, 2u, 1u, 0u, 5u, 4u, 3u, 2u, 7u, 6u, 5u, 4u, 9u, 8u, 7u, 6u,
    3u, 2u, 1u, 0u, 5u, 4u, 3u, 2u, 7u, 6u, 5u, 4u, 9u, 8u, 7u, 6u, 3u, 2u, 1u, 0u, 5u, 4u, 3u, 2u, 7u, 6u, 5u, 4u, 9u, 8u, 7u, 6u};
OWN_ALIGNED_64_ARRAY(static uint32_t shift_table_17u_2[16]) = {
    15u, 14u, 13u, 12u, 11u, 10u, 9u, 8u, 15u, 14u, 13u, 12u, 11u, 10u, 9u, 8u};
OWN_ALIGNED_64_ARRAY(static uint64_t gather_idx_table_17u[8]) = {
    0u, 8u, 8u, 16u, 17u, 25u, 25u, 33u};

// ------------------------------------ 18u -----------------------------------------
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_18u_0[16]) = {
    0u, 1u, 1u, 2u, 2u, 3u, 3u, 4u, 4u, 5u, 5u, 6u, 6u, 7u, 7u, 8u};
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_18u_1[16]) = {
    0u, 1u, 1u, 2u, 2u, 3u, 3u, 4u, 5u, 6u, 6u, 7u, 7u, 8u, 8u, 9u};
OWN_ALIGNED_64_ARRAY(static uint64_t shift_table_18u_0[8]) = {
    0u, 4u, 8u, 12u, 16u, 20u, 24u, 28u};
OWN_ALIGNED_64_ARRAY(static uint64_t shift_table_18u_1[8]) = {
    14u, 10u, 6u, 2u, 30u, 26u, 22u, 18u};

OWN_ALIGNED_64_ARRAY(static uint8_t shuffle_idx_table_18u_0[64]) = {
    3u, 2u, 1u, 0u, 5u, 4u, 3u, 2u, 7u, 6u, 5u, 4u, 9u, 8u, 7u, 6u, 3u, 2u, 1u, 0u, 5u, 4u, 3u, 2u, 7u, 6u, 5u, 4u, 9u, 8u, 7u, 6u,
    3u, 2u, 1u, 0u, 5u, 4u, 3u, 2u, 7u, 6u, 5u, 4u, 9u, 8u, 7u, 6u, 3u, 2u, 1u, 0u, 5u, 4u, 3u, 2u, 7u, 6u, 5u, 4u, 9u, 8u, 7u, 6u};
OWN_ALIGNED_64_ARRAY(static uint32_t shift_table_18u_2[16]) = {
    14u, 12u, 10u, 8u, 14u, 12u, 10u, 8u, 14u, 12u, 10u, 8u, 14u, 12u, 10u, 8u};
OWN_ALIGNED_64_ARRAY(static uint64_t gather_idx_table_18u[8]) = {
    0u, 8u, 9u, 17u, 18u, 26u, 27u, 35u};

// ------------------------------------ 19u -----------------------------------------
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_19u_0[16]) = {
    0u, 1u, 1u, 2u, 2u, 3u, 3u, 4u, 4u, 5u, 5u, 6u, 7u, 8u, 8u, 9u};
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_19u_1[16]) = {
    0u, 1u, 1u, 2u, 2u, 3u, 4u, 5u, 5u, 6u, 6u, 7u, 7u, 8u, 8u, 9u};
OWN_ALIGNED_64_ARRAY(static uint64_t shift_table_19u_0[8]) = {
    0u, 6u, 12u, 18u, 24u, 30u, 4u, 10u};
OWN_ALIGNED_64_ARRAY(static uint64_t shift_table_19u_1[8]) = {
    13u, 7u, 1u, 27u, 21u, 15u, 9u, 3u};

OWN_ALIGNED_64_ARRAY(static uint8_t shuffle_idx_table_19u_0[64]) = {
    3u, 2u, 1u, 0u, 5u, 4u, 3u, 2u, 7u, 6u, 5u, 4u, 10u, 9u, 8u, 7u, 3u, 2u, 1u, 0u, 5u, 4u, 3u, 2u, 8u, 7u, 6u, 5u, 10u, 9u, 8u, 7u,
    3u, 2u, 1u, 0u, 5u, 4u, 3u, 2u, 7u, 6u, 5u, 4u, 10u, 9u, 8u, 7u, 3u, 2u, 1u, 0u, 5u, 4u, 3u, 2u, 8u, 7u, 6u, 5u, 10u, 9u, 8u, 7u};
OWN_ALIGNED_64_ARRAY(static uint32_t shift_table_19u_2[16]) = {
    13u, 10u, 7u, 12u, 9u, 6u, 11u, 8u, 13u, 10u, 7u, 12u, 9u, 6u, 11u, 8u};
OWN_ALIGNED_64_ARRAY(static uint64_t gather_idx_table_19u[8]) = {
    0u, 8u, 9u, 17u, 19u, 27u, 28u, 36u};

// ------------------------------------ 20u -----------------------------------------
OWN_ALIGNED_64_ARRAY(static uint8_t shuffle_idx_table_20u_0[64]) = {
    3u, 2u, 1u, 0u, 5u, 4u, 3u, 2u, 8u, 7u, 6u, 5u, 10u, 9u, 8u, 7u, 3u, 2u, 1u, 0u, 5u, 4u, 3u, 2u, 8u, 7u, 6u, 5u, 10u, 9u, 8u, 7u,
    3u, 2u, 1u, 0u, 5u, 4u, 3u, 2u, 8u, 7u, 6u, 5u, 10u, 9u, 8u, 7u, 3u, 2u, 1u, 0u, 5u, 4u, 3u, 2u, 8u, 7u, 6u, 5u, 10u, 9u, 8u, 7u};
OWN_ALIGNED_64_ARRAY(static uint32_t shift_table_20u[16]) = {
    12u, 8u, 12u, 8u, 12u, 8u, 12u, 8u, 12u, 8u, 12u, 8u, 12u, 8u, 12u, 8u};
OWN_ALIGNED_64_ARRAY(static uint16_t permutex_idx_table_20u[32]) = {
    0u, 1u, 2u, 3u, 4u, 0x0, 0x0, 0x0, 5u, 6u, 7u, 8u, 9u, 0x0, 0x0, 0x0, 
    10u, 11u, 12u, 13u, 14u, 0x0, 0x0, 0x0, 15u, 16u, 17u, 18u, 19u, 0x0, 0x0, 0x0};

// ------------------------------------ 21u -----------------------------------------
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_21u_0[16]) = {
    0u, 1u, 1u, 2u, 2u, 3u, 3u, 4u, 5u, 6u, 6u, 7u, 7u, 8u, 9u, 10u};
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_21u_1[16]) = {
    0u, 1u, 1u, 2u, 3u, 4u, 4u, 5u, 5u, 6u, 7u, 8u, 8u, 9u, 9u, 10u};
OWN_ALIGNED_64_ARRAY(static uint64_t shift_table_21u_0[8]) = {
    0u, 10u, 20u, 30u, 8u, 18u, 28u, 6u};
OWN_ALIGNED_64_ARRAY(static uint64_t shift_table_21u_1[8]) = {
    11u, 1u, 23u, 13u, 3u, 25u, 15u, 5u};

OWN_ALIGNED_64_ARRAY(static uint8_t shuffle_idx_table_21u_0[64]) = {
    3u, 2u, 1u, 0u, 5u, 4u, 3u, 2u, 8u, 7u, 6u, 5u, 10u, 9u, 8u, 7u, 3u, 2u, 1u, 0u, 6u, 5u, 4u, 3u, 8u, 7u, 6u, 5u, 11u, 10u, 9u, 8u,
    3u, 2u, 1u, 0u, 5u, 4u, 3u, 2u, 8u, 7u, 6u, 5u, 10u, 9u, 8u, 7u, 3u, 2u, 1u, 0u, 6u, 5u, 4u, 3u, 8u, 7u, 6u, 5u, 11u, 10u, 9u, 8u};
OWN_ALIGNED_64_ARRAY(static uint32_t shift_table_21u_2[16]) = {
    11u, 6u, 9u, 4u, 7u, 10u, 5u, 8u, 11u, 6u, 9u, 4u, 7u, 10u, 5u, 8u};
OWN_ALIGNED_64_ARRAY(static uint64_t gather_idx_table_21u[8]) = {
    0u, 8u, 10u, 18u, 21u, 29u, 31u, 39u};

// ------------------------------------ 22u -----------------------------------------
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_22u_0[16]) = {
    0u, 1u, 1u, 2u, 2u, 3u, 4u, 5u, 5u, 6u, 6u, 7u, 8u, 9u, 9u, 10u};
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_22u_1[16]) = {
    0u, 1u, 2u, 3u, 3u, 4u, 4u, 5u, 6u, 7u, 7u, 8u, 8u, 9u, 10u, 11u};
OWN_ALIGNED_64_ARRAY(static uint64_t shift_table_22u_0[8]) = {
    0u, 12u, 24u, 4u, 16u, 28u, 8u, 20u};
OWN_ALIGNED_64_ARRAY(static uint64_t shift_table_22u_1[8]) = {
    10u, 30u, 18u, 6u, 26u, 14u, 2u, 22u};

OWN_ALIGNED_64_ARRAY(static uint8_t shuffle_idx_table_22u_0[64]) = {
    3u, 2u, 1u, 0u, 5u, 4u, 3u, 2u, 8u, 7u, 6u, 5u, 11u, 10u, 9u, 8u, 3u, 2u, 1u, 0u, 5u, 4u, 3u, 2u, 8u, 7u, 6u, 5u, 11u, 10u, 9u, 8u,
    3u, 2u, 1u, 0u, 5u, 4u, 3u, 2u, 8u, 7u, 6u, 5u, 11u, 10u, 9u, 8u, 3u, 2u, 1u, 0u, 5u, 4u, 3u, 2u, 8u, 7u, 6u, 5u, 11u, 10u, 9u, 8u};
OWN_ALIGNED_64_ARRAY(static uint32_t shift_table_22u_2[16]) = {
    10u, 4u, 6u, 8u, 10u, 4u, 6u, 8u, 10u, 4u, 6u, 8u, 10u, 4u, 6u, 8u};
OWN_ALIGNED_64_ARRAY(static uint64_t gather_idx_table_22u[8]) = {
    0u, 8u, 11u, 19u, 22u, 30u, 33u, 41u};

// ------------------------------------ 23u -----------------------------------------
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_23u_0[16]) = {
    0u, 1u, 1u, 2u, 2u, 3u, 4u, 5u, 5u, 6u, 7u, 8u, 8u, 9u, 10u, 11u};
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_23u_1[16]) = {
    0u, 1u, 2u, 3u, 3u, 4u, 5u, 6u, 6u, 7u, 7u, 8u, 9u, 10u, 10u, 11u};
OWN_ALIGNED_64_ARRAY(static uint64_t shift_table_23u_0[8]) = {
    0u, 14u, 28u, 10u, 24u, 6u, 20u, 2u};
OWN_ALIGNED_64_ARRAY(static uint64_t shift_table_23u_1[8]) = {
    9u, 27u, 13u, 31u, 17u, 3u, 21u, 7u};

OWN_ALIGNED_64_ARRAY(static uint8_t shuffle_idx_table_23u_0[64]) = {
    3u, 2u, 1u, 0u, 5u, 4u, 3u, 2u, 8u, 7u, 6u, 5u, 11u, 10u, 9u, 8u, 3u, 2u, 1u, 0u, 6u, 5u, 4u, 3u, 9u, 8u, 7u, 6u, 12u, 11u, 10u, 9u,
    3u, 2u, 1u, 0u, 5u, 4u, 3u, 2u, 8u, 7u, 6u, 5u, 11u, 10u, 9u, 8u, 3u, 2u, 1u, 0u, 6u, 5u, 4u, 3u, 9u, 8u, 7u, 6u, 12u, 11u, 10u, 9u};
OWN_ALIGNED_64_ARRAY(static uint32_t shift_table_23u_2[16]) = {
    9u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 2u, 3u, 4u, 5u, 6u, 7u, 8u};
OWN_ALIGNED_64_ARRAY(static uint64_t gather_idx_table_23u[8]) = {
    0u, 8u, 11u, 19u, 23u, 31u, 34u, 42u};

// ------------------------------------ 24u -----------------------------------------
OWN_ALIGNED_64_ARRAY(static uint8_t shuffle_idx_table_24u_0[64]) = {
    2u, 1u, 0u, 0xFF, 5u, 4u, 3u, 0xFF, 8u, 7u, 6u, 0xFF, 11u, 10u, 9u, 0xFF, 2u, 1u, 0u, 0xFF, 5u, 4u, 3u, 0xFF, 8u, 7u, 6u, 0xFF, 11u, 10u, 9u, 0xFF,
    2u, 1u, 0u, 0xFF, 5u, 4u, 3u, 0xFF, 8u, 7u, 6u, 0xFF, 11u, 10u, 9u, 0xFF, 2u, 1u, 0u, 0xFF, 5u, 4u, 3u, 0xFF, 8u, 7u, 6u, 0xFF, 11u, 10u, 9u, 0xFF};
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_24u[16]) = {
    0u, 1u, 2u, 0x0, 3u, 4u, 5u, 0x0, 6u, 7u, 8u, 0x0, 9u, 10u, 11u, 0x0};

// ------------------------------------ 25u -----------------------------------------
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_25u_0[16]) = {
    0u, 1u, 1u, 2u, 3u, 4u, 4u, 5u, 6u, 7u, 7u, 8u, 9u, 10u, 10u, 11u};
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_25u_1[16]) = {
    0u, 1u, 2u, 3u, 3u, 4u, 5u, 6u, 7u, 8u, 8u, 9u, 10u, 11u, 11u, 12u};
OWN_ALIGNED_64_ARRAY(static uint64_t shift_table_25u_0[8]) = {
    0u, 18u, 4u, 22u, 8u, 26u, 12u, 30u};
OWN_ALIGNED_64_ARRAY(static uint64_t shift_table_25u_1[8]) = {
    7u, 21u, 3u, 17u, 31u, 13u, 27u, 9u};

OWN_ALIGNED_64_ARRAY(static uint8_t shuffle_idx_table_25u_0[64]) = {
    3u, 2u, 1u, 0u, 6u, 5u, 4u, 3u, 9u, 8u, 7u, 6u, 12u, 11u, 10u, 9u, 3u, 2u, 1u, 0u, 6u, 5u, 4u, 3u, 9u, 8u, 7u, 6u, 12u, 11u, 10u, 9u,
    3u, 2u, 1u, 0u, 6u, 5u, 4u, 3u, 9u, 8u, 7u, 6u, 12u, 11u, 10u, 9u, 3u, 2u, 1u, 0u, 6u, 5u, 4u, 3u, 9u, 8u, 7u, 6u, 12u, 11u, 10u, 9u};
OWN_ALIGNED_64_ARRAY(static uint32_t shift_table_25u_2[16]) = {
    7u, 6u, 5u, 4u, 3u, 2u, 1u, 0u, 7u, 6u, 5u, 4u, 3u, 2u, 1u, 0u};
OWN_ALIGNED_64_ARRAY(static uint64_t gather_idx_table_25u[8]) = {
    0u, 8u, 12u, 20u, 25u, 33u, 37u, 45u};

// ------------------------------------ 26u -----------------------------------------
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_26u_0[16]) = {
    0u, 1u, 1u, 2u, 3u, 4u, 4u, 5u, 6u, 7u, 8u, 9u, 9u, 10u, 11u, 12u};
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_26u_1[16]) = {
    0u, 1u, 2u, 3u, 4u, 5u, 5u, 6u, 7u, 8u, 8u, 9u, 10u, 11u, 12u, 13u};
OWN_ALIGNED_64_ARRAY(static uint64_t shift_table_26u_0[8]) = {
    0u, 20u, 8u, 28u, 16u, 4u, 24u, 12u};
OWN_ALIGNED_64_ARRAY(static uint64_t shift_table_26u_1[8]) = {
    6u, 18u, 30u, 10u, 22u, 2u, 14u, 26u};

OWN_ALIGNED_64_ARRAY(static uint8_t shuffle_idx_table_26u_0[64]) = {
    3u, 2u, 1u, 0u, 6u, 5u, 4u, 3u, 9u, 8u, 7u, 6u, 12u, 11u, 10u, 9u, 3u, 2u, 1u, 0u, 6u, 5u, 4u, 3u, 9u, 8u, 7u, 6u, 12u, 11u, 10u, 9u,
    3u, 2u, 1u, 0u, 6u, 5u, 4u, 3u, 9u, 8u, 7u, 6u, 12u, 11u, 10u, 9u, 3u, 2u, 1u, 0u, 6u, 5u, 4u, 3u, 9u, 8u, 7u, 6u, 12u, 11u, 10u, 9u};
OWN_ALIGNED_64_ARRAY(static uint32_t shift_table_26u_2[16]) = {
    6u, 4u, 2u, 0u, 6u, 4u, 2u, 0u, 6u, 4u, 2u, 0u, 6u, 4u, 2u, 0u};
OWN_ALIGNED_64_ARRAY(static uint64_t gather_idx_table_26u[8]) = {
    0u, 8u, 13u, 21u, 26u, 34u, 39u, 47u};

// ------------------------------------ 27u -----------------------------------------
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_27u_0[16]) = {
    0u, 1u, 1u, 2u, 3u, 4u, 5u, 6u, 6u, 7u, 8u, 9u, 10u, 11u, 11u, 12u};
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_27u_1[16]) = {
    0u, 1u, 2u, 3u, 4u, 5u, 5u, 6u, 7u, 8u, 9u, 10u, 10u, 11u, 12u, 13u};
OWN_ALIGNED_64_ARRAY(static uint64_t shift_table_27u_0[8]) = {
    0u, 22u, 12u, 2u, 24u, 14u, 4u, 26u};
OWN_ALIGNED_64_ARRAY(static uint64_t shift_table_27u_1[8]) = {
    5u, 15u, 25u, 3u, 13u, 23u, 1u, 11u};

OWN_ALIGNED_64_ARRAY(static uint8_t shuffle_idx_table_27u_0[64]) = {
    0u, 0u, 0u, 4u, 3u, 2u, 1u, 0u, 0u, 0u, 0u, 10u, 9u, 8u, 7u, 6u, 0u, 0u, 0u, 4u, 3u, 2u, 1u, 0u, 0u, 0u, 0u, 11u, 10u, 9u, 8u, 7u,
    0u, 0u, 0u, 4u, 3u, 2u, 1u, 0u, 0u, 0u, 0u, 10u, 9u, 8u, 7u, 6u, 0u, 0u, 0u, 4u, 3u, 2u, 1u, 0u, 0u, 0u, 0u, 11u, 10u, 9u, 8u, 7u};
OWN_ALIGNED_64_ARRAY(static uint8_t shuffle_idx_table_27u_1[64]) = {
    7u, 6u, 5u, 4u, 3u, 0u, 0u, 0u, 14u, 13u, 12u, 11u, 10u, 0u, 0u, 0u, 7u, 6u, 5u, 4u, 3u, 0u, 0u, 0u, 14u, 13u, 12u, 11u, 10u, 0u, 0u, 0u,
    7u, 6u, 5u, 4u, 3u, 0u, 0u, 0u, 14u, 13u, 12u, 11u, 10u, 0u, 0u, 0u, 7u, 6u, 5u, 4u, 3u, 0u, 0u, 0u, 14u, 13u, 12u, 11u, 10u, 0u, 0u, 0u};
OWN_ALIGNED_64_ARRAY(static uint64_t shift_table_27u_2[8]) = {
    37u, 31u, 33u, 35u, 37u, 31u, 33u, 35u};
OWN_ALIGNED_64_ARRAY(static uint64_t shift_table_27u_3[8]) = {
    22u, 20u, 26u, 24u, 22u, 20u, 26u, 24u};
OWN_ALIGNED_64_ARRAY(static uint64_t gather_idx_table_27u[8]) = {
    0u, 8u, 13u, 21u, 27u, 35u, 40u, 48u};

// ------------------------------------ 28u -----------------------------------------
OWN_ALIGNED_64_ARRAY(static uint8_t shuffle_idx_table_28u_0[64]) = {
    3u, 2u, 1u, 0u, 6u, 5u, 4u, 3u, 10u, 9u, 8u, 7u, 13u, 12u, 11u, 10u, 3u, 2u, 1u, 0u, 6u, 5u, 4u, 3u, 10u, 9u, 8u, 7u, 13u, 12u, 11u, 10u,
    3u, 2u, 1u, 0u, 6u, 5u, 4u, 3u, 10u, 9u, 8u, 7u, 13u, 12u, 11u, 10u, 3u, 2u, 1u, 0u, 6u, 5u, 4u, 3u, 10u, 9u, 8u, 7u, 13u, 12u, 11u, 10u};
OWN_ALIGNED_64_ARRAY(static uint32_t shift_table_28u[16]) = {
    4u, 0u, 4u, 0u, 4u, 0u, 4u, 0u, 4u, 0u, 4u, 0u, 4u, 0u, 4u, 0u};
OWN_ALIGNED_64_ARRAY(static uint16_t permutex_idx_table_28u[32]) = {
    0u, 1u, 2u, 3u, 4u, 5u, 6u, 0x0, 7u, 8u, 9u, 10u, 11u, 12u, 13u, 0x0, 
    14u, 15u, 16u, 17u, 18u, 19u, 20u, 0x0, 21u, 22u, 23u, 24u, 25u, 26u, 27u, 0x0};

// ------------------------------------ 29u -----------------------------------------
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_29u_0[16]) = {
    0u, 1u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 10u, 11u, 12u, 13u};
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_29u_1[16]) = {
    0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 9u, 10u, 11u, 12u, 13u, 14u};
OWN_ALIGNED_64_ARRAY(static uint64_t shift_table_29u_0[8]) = {
    0u, 26u, 20u, 14u, 8u, 2u, 28u, 22u};
OWN_ALIGNED_64_ARRAY(static uint64_t shift_table_29u_1[8]) = {
    3u, 9u, 15u, 21u, 27u, 1u, 7u, 13u};

OWN_ALIGNED_64_ARRAY(static uint8_t shuffle_idx_table_29u_0[64]) = {
    0u, 0u, 0u, 4u, 3u, 2u, 1u, 0u, 0u, 0u, 0u, 11u, 10u, 9u, 8u, 7u, 0u, 0u, 0u, 4u, 3u, 2u, 1u, 0u, 0u, 0u, 0u, 11u, 10u, 9u, 8u, 7u,
    0u, 0u, 0u, 4u, 3u, 2u, 1u, 0u, 0u, 0u, 0u, 11u, 10u, 9u, 8u, 7u, 0u, 0u, 0u, 4u, 3u, 2u, 1u, 0u, 0u, 0u, 0u, 11u, 10u, 9u, 8u, 7u};
OWN_ALIGNED_64_ARRAY(static uint8_t shuffle_idx_table_29u_1[64]) = {
    7u, 6u, 5u, 4u, 3u, 0u, 0u, 0u, 14u, 13u, 12u, 11u, 10u, 0u, 0u, 0u, 8u, 7u, 6u, 5u, 4u, 0u, 0u, 0u, 15u, 14u, 13u, 12u, 11u, 0u, 0u, 0u,
    7u, 6u, 5u, 4u, 3u, 0u, 0u, 0u, 14u, 13u, 12u, 11u, 10u, 0u, 0u, 0u, 8u, 7u, 6u, 5u, 4u, 0u, 0u, 0u, 15u, 14u, 13u, 12u, 11u, 0u, 0u, 0u};
OWN_ALIGNED_64_ARRAY(static uint64_t shift_table_29u_2[8]) = {
    35u, 33u, 31u, 29u, 35u, 33u, 31u, 29u};
OWN_ALIGNED_64_ARRAY(static uint64_t shift_table_29u_3[8]) = {
    26u, 28u, 22u, 24u, 26u, 28u, 22u, 24u};
OWN_ALIGNED_64_ARRAY(static uint64_t gather_idx_table_29u[8]) = {
    0u, 8u, 14u, 22u, 29u, 37u, 43u, 51u};

// ------------------------------------ 30u -----------------------------------------
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_30u_0[16]) = {
    0u, 1u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u, 12u, 13u, 14u};
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_30u_1[16]) = {
    0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u, 12u, 13u, 14u, 15u};
OWN_ALIGNED_64_ARRAY(static uint64_t shift_table_30u_0[8]) = {
    0u, 28u, 24u, 20u, 16u, 12u, 8u, 4u};
OWN_ALIGNED_64_ARRAY(static uint64_t shift_table_30u_1[8]) = {
    2u, 6u, 10u, 14u, 18u, 22u, 26u, 30u};

OWN_ALIGNED_64_ARRAY(static uint8_t shuffle_idx_table_30u_0[64]) = {
    0u, 0u, 0u, 4u, 3u, 2u, 1u, 0u, 0u, 0u, 0u, 11u, 10u, 9u, 8u, 7u, 0u, 0u, 0u, 4u, 3u, 2u, 1u, 0u, 0u, 0u, 0u, 11u, 10u, 9u, 8u, 7u,
    0u, 0u, 0u, 4u, 3u, 2u, 1u, 0u, 0u, 0u, 0u, 11u, 10u, 9u, 8u, 7u, 0u, 0u, 0u, 4u, 3u, 2u, 1u, 0u, 0u, 0u, 0u, 11u, 10u, 9u, 8u, 7u};
OWN_ALIGNED_64_ARRAY(static uint8_t shuffle_idx_table_30u_1[64]) = {
    7u, 6u, 5u, 4u, 3u, 0u, 0u, 0u, 15u, 14u, 13u, 12u, 11u, 0u, 0u, 0u, 7u, 6u, 5u, 4u, 3u, 0u, 0u, 0u, 15u, 14u, 13u, 12u, 11u, 0u, 0u, 0u,
    7u, 6u, 5u, 4u, 3u, 0u, 0u, 0u, 15u, 14u, 13u, 12u, 11u, 0u, 0u, 0u, 7u, 6u, 5u, 4u, 3u, 0u, 0u, 0u, 15u, 14u, 13u, 12u, 11u, 0u, 0u, 0u};
OWN_ALIGNED_64_ARRAY(static uint64_t shift_table_30u_2[8]) = {
    34u, 30u, 34u, 30u, 34u, 30u, 34u, 30u};
OWN_ALIGNED_64_ARRAY(static uint64_t shift_table_30u_3[8]) = {
    28u, 24u, 28u, 24u, 28u, 24u, 28u, 24u};
OWN_ALIGNED_64_ARRAY(static uint64_t gather_idx_table_30u[8]) = {
    0u, 8u, 15u, 23u, 30u, 38u, 45u, 53u};

// ------------------------------------ 31u -----------------------------------------
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_31u_0[16]) = {
    0u, 1u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u, 12u, 13u, 14u};
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_31u_1[16]) = {
    0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u, 12u, 13u, 14u, 15u};
OWN_ALIGNED_64_ARRAY(static uint64_t shift_table_31u_0[8]) = {
    0u, 30u, 28u, 26u, 24u, 22u, 20u, 18u};
OWN_ALIGNED_64_ARRAY(static uint64_t shift_table_31u_1[8]) = {
    1u, 3u, 5u, 7u, 9u, 11u, 13u, 15u};

OWN_ALIGNED_64_ARRAY(static uint8_t shuffle_idx_table_31u_0[64]) = {
    0u, 0u, 0u, 4u, 3u, 2u, 1u, 0u, 0u, 0u, 0u, 11u, 10u, 9u, 8u, 7u, 0u, 0u, 0u, 4u, 3u, 2u, 1u, 0u, 0u, 0u, 0u, 12u, 11u, 10u, 9u, 8u,
    0u, 0u, 0u, 4u, 3u, 2u, 1u, 0u, 0u, 0u, 0u, 11u, 10u, 9u, 8u, 7u, 0u, 0u, 0u, 4u, 3u, 2u, 1u, 0u, 0u, 0u, 0u, 12u, 11u, 10u, 9u, 8u};
OWN_ALIGNED_64_ARRAY(static uint8_t shuffle_idx_table_31u_1[64]) = {
    7u, 6u, 5u, 4u, 3u, 0u, 0u, 0u, 15u, 14u, 13u, 12u, 11u, 0u, 0u, 0u, 8u, 7u, 6u, 5u, 4u, 0u, 0u, 0u, 16u, 15u, 14u, 13u, 12u, 0u, 0u, 0u,
    7u, 6u, 5u, 4u, 3u, 0u, 0u, 0u, 15u, 14u, 13u, 12u, 11u, 0u, 0u, 0u, 8u, 7u, 6u, 5u, 4u, 0u, 0u, 0u, 16u, 15u, 14u, 13u, 12u, 0u, 0u, 0u};
OWN_ALIGNED_64_ARRAY(static uint64_t shift_table_31u_2[8]) = {
    33u, 27u, 29u, 31u, 33u, 27u, 29u, 31u};
OWN_ALIGNED_64_ARRAY(static uint64_t shift_table_31u_3[8]) = {
    30u, 28u, 26u, 24u, 30u, 28u, 26u, 24u};
OWN_ALIGNED_64_ARRAY(static uint64_t gather_idx_table_31u[8]) = {
    0u, 8u, 15u, 23u, 31u, 39u, 46u, 54u};

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

OWN_ALIGNED_64_ARRAY(static uint64_t reverse_mask_table_32u[8]) = {
        0x0405060700010203,
        0x0C0D0E0F08090A0B,
        0x1415161710111213,
        0x1C1D1E1F18191A1B,
        0x2425262720212223,
        0x2C2D2E2F28292A2B,
        0x3435363730313233,
        0x3C3D3E3F38393A3B
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
OWN_QPLC_INLINE(void, px_qplc_unpack_be_Nu32u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint32_t bit_width,
        uint8_t *dst_ptr)) {
    uint32_t *src32u_ptr = (uint32_t *) src_ptr;
    uint8_t  *src8u_ptr  = (uint8_t *) src_ptr;
    uint32_t *dst32u_ptr = (uint32_t *) dst_ptr;
    uint32_t shift       = OWN_QWORD_WIDTH - bit_width;
    uint32_t bits_in_buf = 0u;
    uint64_t src         = 0u;
    uint64_t next_dword;
    uint32_t bytes_to_read = OWN_BITS_2_BYTE(num_elements * bit_width + start_bit);

    if (sizeof(uint32_t) <= bytes_to_read) {
        bits_in_buf = OWN_DWORD_WIDTH - start_bit;
        src         = ((uint64_t) qplc_swap_bytes_32u(*src32u_ptr)) << (OWN_QWORD_WIDTH - bits_in_buf);
        
        src32u_ptr++;

        while (2u < num_elements) {
            if (bit_width > bits_in_buf) {
                next_dword = (uint64_t) qplc_swap_bytes_32u(*src32u_ptr);
                src32u_ptr++;
                next_dword = next_dword << (OWN_DWORD_WIDTH - bits_in_buf);
                src        = src | next_dword;
                bits_in_buf += OWN_DWORD_WIDTH;
            }
            *dst32u_ptr = (uint32_t) (src >> shift);
            src = src << bit_width;
            bits_in_buf -= bit_width;
            dst32u_ptr++;
            num_elements--;
        }

        bytes_to_read = OWN_BITS_2_BYTE(num_elements * bit_width > bits_in_buf ?
                                        num_elements * bit_width - bits_in_buf : 0u);

        if (bytes_to_read > 3u) {
            next_dword = (uint64_t) qplc_swap_bytes_32u(*src32u_ptr);
            src32u_ptr++;
            next_dword = next_dword << (OWN_DWORD_WIDTH - bits_in_buf);
            src        = src | next_dword;
            bits_in_buf += OWN_DWORD_WIDTH;
            bytes_to_read -= 4u;
        }

        src8u_ptr = (uint8_t *) src32u_ptr;
    } else {
        next_dword    = 0u;
        for (uint32_t byte_to_read = 0u; byte_to_read < bytes_to_read; byte_to_read++) {
            next_dword |= ((uint64_t) (*src8u_ptr)) << (byte_to_read * OWN_BYTE_WIDTH);
            src8u_ptr++;
        }
        next_dword   = (uint64_t) qplc_swap_bytes_32u((uint32_t) next_dword);
        bits_in_buf  = OWN_DWORD_WIDTH - start_bit;
        next_dword   = next_dword << (OWN_QWORD_WIDTH - bits_in_buf);
        src          = next_dword;
        *dst32u_ptr  = (uint32_t) (src >> shift);
        return;
    }

    while (0u < num_elements) {
        if (bit_width > bits_in_buf) {
            next_dword = 0u;
            for (uint32_t byte_to_read = 0u; byte_to_read < bytes_to_read; byte_to_read++) {
                next_dword |= ((uint64_t) (*src8u_ptr)) << (byte_to_read * OWN_BYTE_WIDTH);
                src8u_ptr++;
            }
            next_dword  = (uint64_t) qplc_swap_bytes_32u((uint32_t) next_dword);
            next_dword  = next_dword << (OWN_DWORD_WIDTH - bits_in_buf);
            src         = src | next_dword;
            bits_in_buf += OWN_DWORD_WIDTH;
        }
        *dst32u_ptr = (uint32_t) (src >> shift);
        src = src << bit_width;
        bits_in_buf -= bit_width;
        dst32u_ptr++;
        num_elements--;
    }
}

// ********************** 17u ****************************** //

OWN_OPT_FUN(void, k0_qplc_unpack_be_17u32u, (const uint8_t *src_ptr,
    uint32_t num_elements,
    uint32_t start_bit,
    uint8_t *dst_ptr)) {
    if (start_bit > 0u) {
        uint32_t align = own_get_align(start_bit, 17u, 32u);
        if (align > num_elements) {
            align = num_elements;
        }
        px_qplc_unpack_be_Nu32u(src_ptr, align, start_bit, 17u, dst_ptr);
        src_ptr += ((align * 17u) + start_bit) >> 3;
        dst_ptr += align * 4;
        num_elements -= align;
    }

    if (num_elements >= 16u) {
        __mmask32 read_mask = OWN_BIT_MASK(17u);
        __m512i   parse_mask0 = _mm512_set1_epi32(OWN_BIT_MASK(17u));
        __m512i   nibble_reversemm = _mm512_load_si512(nibble_reverse_table);
        __m512i   reverse_mask_32u = _mm512_load_si512(reverse_mask_table_32u);
        __m512i   maskmm = _mm512_set1_epi8(0x0F);

        __m512i   shuffle_idx_ptr = _mm512_load_si512(shuffle_idx_table_17u_0);

        __m512i   permutex_idx_ptr[2];
        permutex_idx_ptr[0] = _mm512_load_si512(permutex_idx_table_17u_0);
        permutex_idx_ptr[1] = _mm512_load_si512(permutex_idx_table_17u_1);

        __m512i   shift_mask_ptr[3];
        shift_mask_ptr[0] = _mm512_load_si512(shift_table_17u_0);
        shift_mask_ptr[1] = _mm512_load_si512(shift_table_17u_1);
        shift_mask_ptr[2] = _mm512_load_si512(shift_table_17u_2);

        __m512i   gather_idxmm = _mm512_load_si512(gather_idx_table_17u);

        while (num_elements >= 32u) {
            __m512i srcmm, zmm[2];

            srcmm = _mm512_i64gather_epi64(gather_idxmm, src_ptr, 1u);

            zmm[0] = _mm512_shuffle_epi8(srcmm, shuffle_idx_ptr);

            // shifting elements so they start from the start of the word
            zmm[0] = _mm512_srlv_epi32(zmm[0], shift_mask_ptr[2]);
            zmm[0] = _mm512_and_si512(zmm[0], parse_mask0);

            _mm512_storeu_si512(dst_ptr, zmm[0]);

            src_ptr += 2u * 17u;
            dst_ptr += 64u;
            num_elements -= 16u;
        }

        if (num_elements >= 16u) {
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
            zmm[0] = _mm512_permutexvar_epi32(permutex_idx_ptr[0], srcmm);
            zmm[1] = _mm512_permutexvar_epi32(permutex_idx_ptr[1], srcmm);

            // shifting elements so they start from the start of the word
            zmm[0] = _mm512_srlv_epi64(zmm[0], shift_mask_ptr[0]);
            zmm[1] = _mm512_sllv_epi64(zmm[1], shift_mask_ptr[1]);

            // gathering even and odd elements together
            zmm[0] = _mm512_mask_mov_epi32(zmm[0], 0xAAAA, zmm[1]);
            zmm[0] = _mm512_and_si512(zmm[0], parse_mask0);

            zmm[0] = _mm512_slli_epi32(zmm[0], 15u);
            low_nibblemm = _mm512_and_si512(zmm[0], maskmm);
            high_nibblemm = _mm512_srli_epi16(zmm[0], 4u);
            high_nibblemm = _mm512_and_si512(high_nibblemm, maskmm);

            low_nibblemm = _mm512_shuffle_epi8(nibble_reversemm, low_nibblemm);
            high_nibblemm = _mm512_shuffle_epi8(nibble_reversemm, high_nibblemm);
            low_nibblemm = _mm512_slli_epi16(low_nibblemm, 4u);

            zmm[0] = _mm512_or_si512(low_nibblemm, high_nibblemm);
            zmm[0] = _mm512_shuffle_epi8(zmm[0], reverse_mask_32u);

            _mm512_storeu_si512(dst_ptr, zmm[0]);

            src_ptr += 2u * 17u;
            dst_ptr += 64u;
            num_elements -= 16u;
        }
    }

    if (num_elements > 0) {
        px_qplc_unpack_be_Nu32u(src_ptr, num_elements, 0u, 17u, dst_ptr);
    }
}

// ********************** 18u ****************************** //

OWN_OPT_FUN(void, k0_qplc_unpack_be_18u32u, (const uint8_t *src_ptr,
    uint32_t num_elements,
    uint32_t start_bit,
    uint8_t *dst_ptr)) {
    if (start_bit > 0u) {
        uint32_t align = own_get_align(start_bit, 18u, 32u);
        if (align > num_elements) {
            align = num_elements;
        }
        px_qplc_unpack_be_Nu32u(src_ptr, align, start_bit, 18u, dst_ptr);
        src_ptr += ((align * 18u) + start_bit) >> 3;
        dst_ptr += align * 4;
        num_elements -= align;
    }

    if (num_elements >= 16u) {
        __mmask16 read_mask = OWN_BIT_MASK(OWN_BITS_2_DWORD(18u * OWN_WORD_WIDTH));
        __m512i   parse_mask0 = _mm512_set1_epi32(OWN_BIT_MASK(18u));
        __m512i   nibble_reversemm = _mm512_load_si512(nibble_reverse_table);
        __m512i   reverse_mask_32u = _mm512_load_si512(reverse_mask_table_32u);
        __m512i   maskmm = _mm512_set1_epi8(0x0F);

        __m512i   shuffle_idx_ptr = _mm512_load_si512(shuffle_idx_table_18u_0);

        __m512i   permutex_idx_ptr[2];
        permutex_idx_ptr[0] = _mm512_load_si512(permutex_idx_table_18u_0);
        permutex_idx_ptr[1] = _mm512_load_si512(permutex_idx_table_18u_1);

        __m512i   shift_mask_ptr[3];
        shift_mask_ptr[0] = _mm512_load_si512(shift_table_18u_0);
        shift_mask_ptr[1] = _mm512_load_si512(shift_table_18u_1);
        shift_mask_ptr[2] = _mm512_load_si512(shift_table_18u_2);

        __m512i   gather_idxmm = _mm512_load_si512(gather_idx_table_18u);

        while (num_elements >= 32u) {
            __m512i srcmm, zmm[2];

            srcmm = _mm512_i64gather_epi64(gather_idxmm, src_ptr, 1u);

            zmm[0] = _mm512_shuffle_epi8(srcmm, shuffle_idx_ptr);

            // shifting elements so they start from the start of the word
            zmm[0] = _mm512_srlv_epi32(zmm[0], shift_mask_ptr[2]);
            zmm[0] = _mm512_and_si512(zmm[0], parse_mask0);

            _mm512_storeu_si512(dst_ptr, zmm[0]);

            src_ptr += 2u * 18u;
            dst_ptr += 64u;
            num_elements -= 16u;
        }

        if (num_elements >= 16u) {
            __m512i srcmm, zmm[2];

            srcmm = _mm512_maskz_loadu_epi32(read_mask, src_ptr);

            __m512i low_nibblemm = _mm512_and_si512(srcmm, maskmm);
            __m512i high_nibblemm = _mm512_srli_epi16(srcmm, 4u);
            high_nibblemm = _mm512_and_si512(high_nibblemm, maskmm);

            low_nibblemm = _mm512_shuffle_epi8(nibble_reversemm, low_nibblemm);
            high_nibblemm = _mm512_shuffle_epi8(nibble_reversemm, high_nibblemm);
            low_nibblemm = _mm512_slli_epi16(low_nibblemm, 4u);

            srcmm = _mm512_or_si512(low_nibblemm, high_nibblemm);

            // permuting so in zmm[0] will be elements with even indexes and in zmm[1] - with odd ones
            zmm[0] = _mm512_permutexvar_epi32(permutex_idx_ptr[0], srcmm);
            zmm[1] = _mm512_permutexvar_epi32(permutex_idx_ptr[1], srcmm);

            // shifting elements so they start from the start of the word
            zmm[0] = _mm512_srlv_epi64(zmm[0], shift_mask_ptr[0]);
            zmm[1] = _mm512_sllv_epi64(zmm[1], shift_mask_ptr[1]);

            // gathering even and odd elements together
            zmm[0] = _mm512_mask_mov_epi32(zmm[0], 0xAAAA, zmm[1]);
            zmm[0] = _mm512_and_si512(zmm[0], parse_mask0);

            zmm[0] = _mm512_slli_epi32(zmm[0], 14u);
            low_nibblemm = _mm512_and_si512(zmm[0], maskmm);
            high_nibblemm = _mm512_srli_epi16(zmm[0], 4u);
            high_nibblemm = _mm512_and_si512(high_nibblemm, maskmm);

            low_nibblemm = _mm512_shuffle_epi8(nibble_reversemm, low_nibblemm);
            high_nibblemm = _mm512_shuffle_epi8(nibble_reversemm, high_nibblemm);
            low_nibblemm = _mm512_slli_epi16(low_nibblemm, 4u);

            zmm[0] = _mm512_or_si512(low_nibblemm, high_nibblemm);
            zmm[0] = _mm512_shuffle_epi8(zmm[0], reverse_mask_32u);

            _mm512_storeu_si512(dst_ptr, zmm[0]);

            src_ptr += 2u * 18u;
            dst_ptr += 64u;
            num_elements -= 16u;
        }
    }

    if (num_elements > 0) {
        px_qplc_unpack_be_Nu32u(src_ptr, num_elements, 0u, 18u, dst_ptr);
    }
}

// ********************** 19u ****************************** //

OWN_OPT_FUN(void, k0_qplc_unpack_be_19u32u, (const uint8_t *src_ptr,
    uint32_t num_elements,
    uint32_t start_bit,
    uint8_t *dst_ptr)) {
    if (start_bit > 0u) {
        uint32_t align = own_get_align(start_bit, 19u, 32u);
        if (align > num_elements) {
            align = num_elements;
        }
        px_qplc_unpack_be_Nu32u(src_ptr, align, start_bit, 19u, dst_ptr);
        src_ptr += ((align * 19u) + start_bit) >> 3;
        dst_ptr += align * 4;
        num_elements -= align;
    }

    if (num_elements >= 16u) {
        __mmask32 read_mask = OWN_BIT_MASK(19u);
        __m512i   parse_mask0 = _mm512_set1_epi32(OWN_BIT_MASK(19u));
        __m512i   nibble_reversemm = _mm512_load_si512(nibble_reverse_table);
        __m512i   reverse_mask_32u = _mm512_load_si512(reverse_mask_table_32u);
        __m512i   maskmm = _mm512_set1_epi8(0x0F);

        __m512i   shuffle_idx_ptr = _mm512_load_si512(shuffle_idx_table_19u_0);

        __m512i   permutex_idx_ptr[2];
        permutex_idx_ptr[0] = _mm512_load_si512(permutex_idx_table_19u_0);
        permutex_idx_ptr[1] = _mm512_load_si512(permutex_idx_table_19u_1);

        __m512i   shift_mask_ptr[3];
        shift_mask_ptr[0] = _mm512_load_si512(shift_table_19u_0);
        shift_mask_ptr[1] = _mm512_load_si512(shift_table_19u_1);
        shift_mask_ptr[2] = _mm512_load_si512(shift_table_19u_2);

        __m512i   gather_idxmm = _mm512_load_si512(gather_idx_table_19u);

        while (num_elements >= 32u) {
            __m512i srcmm, zmm[2];

            srcmm = _mm512_i64gather_epi64(gather_idxmm, src_ptr, 1u);

            zmm[0] = _mm512_shuffle_epi8(srcmm, shuffle_idx_ptr);

            // shifting elements so they start from the start of the word
            zmm[0] = _mm512_srlv_epi32(zmm[0], shift_mask_ptr[2]);
            zmm[0] = _mm512_and_si512(zmm[0], parse_mask0);

            _mm512_storeu_si512(dst_ptr, zmm[0]);

            src_ptr += 2u * 19u;
            dst_ptr += 64u;
            num_elements -= 16u;
        }

        if (num_elements >= 16u) {
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
            zmm[0] = _mm512_permutexvar_epi32(permutex_idx_ptr[0], srcmm);
            zmm[1] = _mm512_permutexvar_epi32(permutex_idx_ptr[1], srcmm);

            // shifting elements so they start from the start of the word
            zmm[0] = _mm512_srlv_epi64(zmm[0], shift_mask_ptr[0]);
            zmm[1] = _mm512_sllv_epi64(zmm[1], shift_mask_ptr[1]);

            // gathering even and odd elements together
            zmm[0] = _mm512_mask_mov_epi32(zmm[0], 0xAAAA, zmm[1]);
            zmm[0] = _mm512_and_si512(zmm[0], parse_mask0);

            zmm[0] = _mm512_slli_epi32(zmm[0], 13u);
            low_nibblemm = _mm512_and_si512(zmm[0], maskmm);
            high_nibblemm = _mm512_srli_epi16(zmm[0], 4u);
            high_nibblemm = _mm512_and_si512(high_nibblemm, maskmm);

            low_nibblemm = _mm512_shuffle_epi8(nibble_reversemm, low_nibblemm);
            high_nibblemm = _mm512_shuffle_epi8(nibble_reversemm, high_nibblemm);
            low_nibblemm = _mm512_slli_epi16(low_nibblemm, 4u);

            zmm[0] = _mm512_or_si512(low_nibblemm, high_nibblemm);
            zmm[0] = _mm512_shuffle_epi8(zmm[0], reverse_mask_32u);

            _mm512_storeu_si512(dst_ptr, zmm[0]);

            src_ptr += 2u * 19u;
            dst_ptr += 64u;
            num_elements -= 16u;
        }
    }

    if (num_elements > 0) {
        px_qplc_unpack_be_Nu32u(src_ptr, num_elements, 0u, 19u, dst_ptr);
    }
}

// ********************** 20u ****************************** //

OWN_OPT_FUN(void, k0_qplc_unpack_be_20u32u, (const uint8_t *src_ptr,
    uint32_t num_elements,
    uint32_t start_bit,
    uint8_t *dst_ptr)) {
    if (start_bit > 0u) {
        uint32_t align = own_get_align(start_bit, 20u, 32u);
        if (align > num_elements) {
            align = num_elements;
        }
        px_qplc_unpack_be_Nu32u(src_ptr, align, start_bit, 20u, dst_ptr);
        src_ptr += ((align * 20u) + start_bit) >> 3;
        dst_ptr += align * 4;
        num_elements -= align;
    }

    if (num_elements >= 16u) {
        __mmask16 read_mask = OWN_BIT_MASK(OWN_BITS_2_DWORD(20u * OWN_WORD_WIDTH));
        __m512i   parse_mask0 = _mm512_set1_epi32(OWN_BIT_MASK(20u));

        __m512i   shuffle_idx_ptr = _mm512_load_si512(shuffle_idx_table_20u_0);
        __m512i   permutex_idx = _mm512_load_si512(permutex_idx_table_20u);
        __m512i   shift_mask = _mm512_load_si512(shift_table_20u);

        while (num_elements >= 16u) {
            __m512i srcmm, zmm;

            srcmm = _mm512_maskz_loadu_epi32(read_mask, src_ptr);

            zmm = _mm512_permutexvar_epi16(permutex_idx, srcmm);
            zmm = _mm512_shuffle_epi8(zmm, shuffle_idx_ptr);

            // shifting elements so they start from the start of the word
            zmm = _mm512_srlv_epi32(zmm, shift_mask);
            zmm = _mm512_and_si512(zmm, parse_mask0);

            _mm512_storeu_si512(dst_ptr, zmm);

            src_ptr += 2u * 20u;
            dst_ptr += 64u;
            num_elements -= 16u;
        }
    }

    if (num_elements > 0) {
        px_qplc_unpack_be_Nu32u(src_ptr, num_elements, 0u, 20u, dst_ptr);
    }
}

// ********************** 21u ****************************** //

OWN_OPT_FUN(void, k0_qplc_unpack_be_21u32u, (const uint8_t *src_ptr,
    uint32_t num_elements,
    uint32_t start_bit,
    uint8_t *dst_ptr)) {
    if (start_bit > 0u) {
        uint32_t align = own_get_align(start_bit, 21u, 32u);
        if (align > num_elements) {
            align = num_elements;
        }
        px_qplc_unpack_be_Nu32u(src_ptr, align, start_bit, 21u, dst_ptr);
        src_ptr += ((align * 21u) + start_bit) >> 3;
        dst_ptr += align * 4;
        num_elements -= align;
    }

    if (num_elements >= 16u) {
        __mmask32 read_mask = OWN_BIT_MASK(21u);
        __m512i   parse_mask0 = _mm512_set1_epi32(OWN_BIT_MASK(21u));
        __m512i   nibble_reversemm = _mm512_load_si512(nibble_reverse_table);
        __m512i   reverse_mask_32u = _mm512_load_si512(reverse_mask_table_32u);
        __m512i   maskmm = _mm512_set1_epi8(0x0F);

        __m512i   shuffle_idx_ptr = _mm512_load_si512(shuffle_idx_table_21u_0);

        __m512i   permutex_idx_ptr[2];
        permutex_idx_ptr[0] = _mm512_load_si512(permutex_idx_table_21u_0);
        permutex_idx_ptr[1] = _mm512_load_si512(permutex_idx_table_21u_1);

        __m512i   shift_mask_ptr[3];
        shift_mask_ptr[0] = _mm512_load_si512(shift_table_21u_0);
        shift_mask_ptr[1] = _mm512_load_si512(shift_table_21u_1);
        shift_mask_ptr[2] = _mm512_load_si512(shift_table_21u_2);

        __m512i   gather_idxmm = _mm512_load_si512(gather_idx_table_21u);

        while (num_elements >= 32u) {
            __m512i srcmm, zmm[2];

            srcmm = _mm512_i64gather_epi64(gather_idxmm, src_ptr, 1u);

            zmm[0] = _mm512_shuffle_epi8(srcmm, shuffle_idx_ptr);

            // shifting elements so they start from the start of the word
            zmm[0] = _mm512_srlv_epi32(zmm[0], shift_mask_ptr[2]);
            zmm[0] = _mm512_and_si512(zmm[0], parse_mask0);

            _mm512_storeu_si512(dst_ptr, zmm[0]);

            src_ptr += 2u * 21u;
            dst_ptr += 64u;
            num_elements -= 16u;
        }

        if (num_elements >= 16u) {
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
            zmm[0] = _mm512_permutexvar_epi32(permutex_idx_ptr[0], srcmm);
            zmm[1] = _mm512_permutexvar_epi32(permutex_idx_ptr[1], srcmm);

            // shifting elements so they start from the start of the word
            zmm[0] = _mm512_srlv_epi64(zmm[0], shift_mask_ptr[0]);
            zmm[1] = _mm512_sllv_epi64(zmm[1], shift_mask_ptr[1]);

            // gathering even and odd elements together
            zmm[0] = _mm512_mask_mov_epi32(zmm[0], 0xAAAA, zmm[1]);
            zmm[0] = _mm512_and_si512(zmm[0], parse_mask0);

            zmm[0] = _mm512_slli_epi32(zmm[0], 11u);
            low_nibblemm = _mm512_and_si512(zmm[0], maskmm);
            high_nibblemm = _mm512_srli_epi16(zmm[0], 4u);
            high_nibblemm = _mm512_and_si512(high_nibblemm, maskmm);

            low_nibblemm = _mm512_shuffle_epi8(nibble_reversemm, low_nibblemm);
            high_nibblemm = _mm512_shuffle_epi8(nibble_reversemm, high_nibblemm);
            low_nibblemm = _mm512_slli_epi16(low_nibblemm, 4u);

            zmm[0] = _mm512_or_si512(low_nibblemm, high_nibblemm);
            zmm[0] = _mm512_shuffle_epi8(zmm[0], reverse_mask_32u);

            _mm512_storeu_si512(dst_ptr, zmm[0]);

            src_ptr += 2u * 21u;
            dst_ptr += 64u;
            num_elements -= 16u;
        }
    }

    if (num_elements > 0) {
        px_qplc_unpack_be_Nu32u(src_ptr, num_elements, 0u, 21u, dst_ptr);
    }
}

// ********************** 22u ****************************** //

OWN_OPT_FUN(void, k0_qplc_unpack_be_22u32u, (const uint8_t *src_ptr,
    uint32_t num_elements,
    uint32_t start_bit,
    uint8_t *dst_ptr)) {
    if (start_bit > 0u) {
        uint32_t align = own_get_align(start_bit, 22u, 32u);
        if (align > num_elements) {
            align = num_elements;
        }
        px_qplc_unpack_be_Nu32u(src_ptr, align, start_bit, 22u, dst_ptr);
        src_ptr += ((align * 22u) + start_bit) >> 3;
        dst_ptr += align * 4;
        num_elements -= align;
    }

    if (num_elements >= 16u) {
        __mmask16 read_mask = OWN_BIT_MASK(OWN_BITS_2_DWORD(22u * OWN_WORD_WIDTH));
        __m512i   parse_mask0 = _mm512_set1_epi32(OWN_BIT_MASK(22u));
        __m512i   nibble_reversemm = _mm512_load_si512(nibble_reverse_table);
        __m512i   reverse_mask_32u = _mm512_load_si512(reverse_mask_table_32u);
        __m512i   maskmm = _mm512_set1_epi8(0x0F);

        __m512i   shuffle_idx_ptr = _mm512_load_si512(shuffle_idx_table_22u_0);

        __m512i   permutex_idx_ptr[2];
        permutex_idx_ptr[0] = _mm512_load_si512(permutex_idx_table_22u_0);
        permutex_idx_ptr[1] = _mm512_load_si512(permutex_idx_table_22u_1);

        __m512i   shift_mask_ptr[3];
        shift_mask_ptr[0] = _mm512_load_si512(shift_table_22u_0);
        shift_mask_ptr[1] = _mm512_load_si512(shift_table_22u_1);
        shift_mask_ptr[2] = _mm512_load_si512(shift_table_22u_2);

        __m512i   gather_idxmm = _mm512_load_si512(gather_idx_table_22u);

        while (num_elements >= 32u) {
            __m512i srcmm, zmm[2];

            srcmm = _mm512_i64gather_epi64(gather_idxmm, src_ptr, 1u);

            zmm[0] = _mm512_shuffle_epi8(srcmm, shuffle_idx_ptr);

            // shifting elements so they start from the start of the word
            zmm[0] = _mm512_srlv_epi32(zmm[0], shift_mask_ptr[2]);
            zmm[0] = _mm512_and_si512(zmm[0], parse_mask0);

            _mm512_storeu_si512(dst_ptr, zmm[0]);

            src_ptr += 2u * 22u;
            dst_ptr += 64u;
            num_elements -= 16u;
        }

        if (num_elements >= 16u) {
            __m512i srcmm, zmm[2];

            srcmm = _mm512_maskz_loadu_epi32(read_mask, src_ptr);

            __m512i low_nibblemm = _mm512_and_si512(srcmm, maskmm);
            __m512i high_nibblemm = _mm512_srli_epi16(srcmm, 4u);
            high_nibblemm = _mm512_and_si512(high_nibblemm, maskmm);

            low_nibblemm = _mm512_shuffle_epi8(nibble_reversemm, low_nibblemm);
            high_nibblemm = _mm512_shuffle_epi8(nibble_reversemm, high_nibblemm);
            low_nibblemm = _mm512_slli_epi16(low_nibblemm, 4u);

            srcmm = _mm512_or_si512(low_nibblemm, high_nibblemm);

            // permuting so in zmm[0] will be elements with even indexes and in zmm[1] - with odd ones
            zmm[0] = _mm512_permutexvar_epi32(permutex_idx_ptr[0], srcmm);
            zmm[1] = _mm512_permutexvar_epi32(permutex_idx_ptr[1], srcmm);

            // shifting elements so they start from the start of the word
            zmm[0] = _mm512_srlv_epi64(zmm[0], shift_mask_ptr[0]);
            zmm[1] = _mm512_sllv_epi64(zmm[1], shift_mask_ptr[1]);

            // gathering even and odd elements together
            zmm[0] = _mm512_mask_mov_epi32(zmm[0], 0xAAAA, zmm[1]);
            zmm[0] = _mm512_and_si512(zmm[0], parse_mask0);

            zmm[0] = _mm512_slli_epi32(zmm[0], 10u);
            low_nibblemm = _mm512_and_si512(zmm[0], maskmm);
            high_nibblemm = _mm512_srli_epi16(zmm[0], 4u);
            high_nibblemm = _mm512_and_si512(high_nibblemm, maskmm);

            low_nibblemm = _mm512_shuffle_epi8(nibble_reversemm, low_nibblemm);
            high_nibblemm = _mm512_shuffle_epi8(nibble_reversemm, high_nibblemm);
            low_nibblemm = _mm512_slli_epi16(low_nibblemm, 4u);

            zmm[0] = _mm512_or_si512(low_nibblemm, high_nibblemm);
            zmm[0] = _mm512_shuffle_epi8(zmm[0], reverse_mask_32u);

            _mm512_storeu_si512(dst_ptr, zmm[0]);

            src_ptr += 2u * 22u;
            dst_ptr += 64u;
            num_elements -= 16u;
        }
    }

    if (num_elements > 0) {
        px_qplc_unpack_be_Nu32u(src_ptr, num_elements, 0u, 22u, dst_ptr);
    }
}

// ********************** 23u ****************************** //

OWN_OPT_FUN(void, k0_qplc_unpack_be_23u32u, (const uint8_t *src_ptr,
    uint32_t num_elements,
    uint32_t start_bit,
    uint8_t *dst_ptr)) {
    if (start_bit > 0u) {
        uint32_t align = own_get_align(start_bit, 23u, 32u);
        if (align > num_elements) {
            align = num_elements;
        }
        px_qplc_unpack_be_Nu32u(src_ptr, align, start_bit, 23u, dst_ptr);
        src_ptr += ((align * 23u) + start_bit) >> 3;
        dst_ptr += align * 4;
        num_elements -= align;
    }

    if (num_elements >= 16u) {
        __mmask32 read_mask = OWN_BIT_MASK(23u);
        __m512i   parse_mask0 = _mm512_set1_epi32(OWN_BIT_MASK(23u));
        __m512i   nibble_reversemm = _mm512_load_si512(nibble_reverse_table);
        __m512i   reverse_mask_32u = _mm512_load_si512(reverse_mask_table_32u);
        __m512i   maskmm = _mm512_set1_epi8(0x0F);

        __m512i   shuffle_idx_ptr = _mm512_load_si512(shuffle_idx_table_23u_0);

        __m512i   permutex_idx_ptr[2];
        permutex_idx_ptr[0] = _mm512_load_si512(permutex_idx_table_23u_0);
        permutex_idx_ptr[1] = _mm512_load_si512(permutex_idx_table_23u_1);

        __m512i   shift_mask_ptr[3];
        shift_mask_ptr[0] = _mm512_load_si512(shift_table_23u_0);
        shift_mask_ptr[1] = _mm512_load_si512(shift_table_23u_1);
        shift_mask_ptr[2] = _mm512_load_si512(shift_table_23u_2);

        __m512i   gather_idxmm = _mm512_load_si512(gather_idx_table_23u);

        while (num_elements >= 32u) {
            __m512i srcmm, zmm[2];

            srcmm = _mm512_i64gather_epi64(gather_idxmm, src_ptr, 1u);

            zmm[0] = _mm512_shuffle_epi8(srcmm, shuffle_idx_ptr);

            // shifting elements so they start from the start of the word
            zmm[0] = _mm512_srlv_epi32(zmm[0], shift_mask_ptr[2]);
            zmm[0] = _mm512_and_si512(zmm[0], parse_mask0);

            _mm512_storeu_si512(dst_ptr, zmm[0]);

            src_ptr += 2u * 23u;
            dst_ptr += 64u;
            num_elements -= 16u;
        }

        if (num_elements >= 16u) {
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
            zmm[0] = _mm512_permutexvar_epi32(permutex_idx_ptr[0], srcmm);
            zmm[1] = _mm512_permutexvar_epi32(permutex_idx_ptr[1], srcmm);

            // shifting elements so they start from the start of the word
            zmm[0] = _mm512_srlv_epi64(zmm[0], shift_mask_ptr[0]);
            zmm[1] = _mm512_sllv_epi64(zmm[1], shift_mask_ptr[1]);

            // gathering even and odd elements together
            zmm[0] = _mm512_mask_mov_epi32(zmm[0], 0xAAAA, zmm[1]);
            zmm[0] = _mm512_and_si512(zmm[0], parse_mask0);

            zmm[0] = _mm512_slli_epi32(zmm[0], 9u);
            low_nibblemm = _mm512_and_si512(zmm[0], maskmm);
            high_nibblemm = _mm512_srli_epi16(zmm[0], 4u);
            high_nibblemm = _mm512_and_si512(high_nibblemm, maskmm);

            low_nibblemm = _mm512_shuffle_epi8(nibble_reversemm, low_nibblemm);
            high_nibblemm = _mm512_shuffle_epi8(nibble_reversemm, high_nibblemm);
            low_nibblemm = _mm512_slli_epi16(low_nibblemm, 4u);

            zmm[0] = _mm512_or_si512(low_nibblemm, high_nibblemm);
            zmm[0] = _mm512_shuffle_epi8(zmm[0], reverse_mask_32u);

            _mm512_storeu_si512(dst_ptr, zmm[0]);

            src_ptr += 2u * 23u;
            dst_ptr += 64u;
            num_elements -= 16u;
        }
    }

    if (num_elements > 0) {
        px_qplc_unpack_be_Nu32u(src_ptr, num_elements, 0u, 23u, dst_ptr);
    }
}

// ********************** 24u ****************************** //

OWN_OPT_FUN(void, k0_qplc_unpack_be_24u32u, (const uint8_t *src_ptr,
    uint32_t num_elements,
    uint32_t start_bit,
    uint8_t *dst_ptr)) {
    if (start_bit > 0u) {
        uint32_t align = own_get_align(start_bit, 24u, 32u);
        if (align > num_elements) {
            align = num_elements;
        }
        px_qplc_unpack_be_Nu32u(src_ptr, align, start_bit, 24u, dst_ptr);
        src_ptr += ((align * 24u) + start_bit) >> 3;
        dst_ptr += align * 4;
        num_elements -= align;
    }

    if (num_elements >= 16u) {
        __mmask16 read_mask = OWN_BIT_MASK(OWN_BITS_2_DWORD(24u * OWN_WORD_WIDTH));
        
        __m512i   shuffle_idx = _mm512_load_si512(shuffle_idx_table_24u_0);
        __m512i   permutex_idx = _mm512_load_si512(permutex_idx_table_24u);

        while (num_elements >= 16u) {
            __m512i srcmm, zmm;

            srcmm = _mm512_maskz_loadu_epi32(read_mask, src_ptr);

            zmm = _mm512_permutexvar_epi32(permutex_idx, srcmm);
            zmm = _mm512_shuffle_epi8(zmm, shuffle_idx);

            _mm512_storeu_si512(dst_ptr, zmm);

            src_ptr += 2u * 24u;
            dst_ptr += 64u;
            num_elements -= 16u;
        }
    }

    if (num_elements > 0) {
        px_qplc_unpack_be_Nu32u(src_ptr, num_elements, 0u, 24u, dst_ptr);
    }
}

// ********************** 25u ****************************** //

OWN_OPT_FUN(void, k0_qplc_unpack_be_25u32u, (const uint8_t *src_ptr,
    uint32_t num_elements,
    uint32_t start_bit,
    uint8_t *dst_ptr)) {
    if (start_bit > 0u) {
        uint32_t align = own_get_align(start_bit, 25u, 32u);
        if (align > num_elements) {
            align = num_elements;
        }
        px_qplc_unpack_be_Nu32u(src_ptr, align, start_bit, 25u, dst_ptr);
        src_ptr += ((align * 25u) + start_bit) >> 3;
        dst_ptr += align * 4;
        num_elements -= align;
    }

    if (num_elements >= 16u) {
        __mmask32 read_mask = OWN_BIT_MASK(25u);
        __m512i   parse_mask0 = _mm512_set1_epi32(OWN_BIT_MASK(25u));
        __m512i   nibble_reversemm = _mm512_load_si512(nibble_reverse_table);
        __m512i   reverse_mask_32u = _mm512_load_si512(reverse_mask_table_32u);
        __m512i   maskmm = _mm512_set1_epi8(0x0F);

        __m512i   shuffle_idx_ptr = _mm512_load_si512(shuffle_idx_table_25u_0);

        __m512i   permutex_idx_ptr[2];
        permutex_idx_ptr[0] = _mm512_load_si512(permutex_idx_table_25u_0);
        permutex_idx_ptr[1] = _mm512_load_si512(permutex_idx_table_25u_1);

        __m512i   shift_mask_ptr[3];
        shift_mask_ptr[0] = _mm512_load_si512(shift_table_25u_0);
        shift_mask_ptr[1] = _mm512_load_si512(shift_table_25u_1);
        shift_mask_ptr[2] = _mm512_load_si512(shift_table_25u_2);

        __m512i   gather_idxmm = _mm512_load_si512(gather_idx_table_25u);

        while (num_elements >= 32u) {
            __m512i srcmm, zmm[2];

            srcmm = _mm512_i64gather_epi64(gather_idxmm, src_ptr, 1u);

            zmm[0] = _mm512_shuffle_epi8(srcmm, shuffle_idx_ptr);

            // shifting elements so they start from the start of the word
            zmm[0] = _mm512_srlv_epi32(zmm[0], shift_mask_ptr[2]);
            zmm[0] = _mm512_and_si512(zmm[0], parse_mask0);

            _mm512_storeu_si512(dst_ptr, zmm[0]);

            src_ptr += 2u * 25u;
            dst_ptr += 64u;
            num_elements -= 16u;
        }

        if (num_elements >= 16u) {
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
            zmm[0] = _mm512_permutexvar_epi32(permutex_idx_ptr[0], srcmm);
            zmm[1] = _mm512_permutexvar_epi32(permutex_idx_ptr[1], srcmm);

            // shifting elements so they start from the start of the word
            zmm[0] = _mm512_srlv_epi64(zmm[0], shift_mask_ptr[0]);
            zmm[1] = _mm512_sllv_epi64(zmm[1], shift_mask_ptr[1]);

            // gathering even and odd elements together
            zmm[0] = _mm512_mask_mov_epi32(zmm[0], 0xAAAA, zmm[1]);
            zmm[0] = _mm512_and_si512(zmm[0], parse_mask0);

            zmm[0] = _mm512_slli_epi32(zmm[0], 7u);
            low_nibblemm = _mm512_and_si512(zmm[0], maskmm);
            high_nibblemm = _mm512_srli_epi16(zmm[0], 4u);
            high_nibblemm = _mm512_and_si512(high_nibblemm, maskmm);

            low_nibblemm = _mm512_shuffle_epi8(nibble_reversemm, low_nibblemm);
            high_nibblemm = _mm512_shuffle_epi8(nibble_reversemm, high_nibblemm);
            low_nibblemm = _mm512_slli_epi16(low_nibblemm, 4u);

            zmm[0] = _mm512_or_si512(low_nibblemm, high_nibblemm);
            zmm[0] = _mm512_shuffle_epi8(zmm[0], reverse_mask_32u);

            _mm512_storeu_si512(dst_ptr, zmm[0]);

            src_ptr += 2u * 25u;
            dst_ptr += 64u;
            num_elements -= 16u;
        }
    }

    if (num_elements > 0) {
        px_qplc_unpack_be_Nu32u(src_ptr, num_elements, 0u, 25u, dst_ptr);
    }
}

// ********************** 26u ****************************** //

OWN_OPT_FUN(void, k0_qplc_unpack_be_26u32u, (const uint8_t *src_ptr,
    uint32_t num_elements,
    uint32_t start_bit,
    uint8_t *dst_ptr)) {
    if (start_bit > 0u) {
        uint32_t align = own_get_align(start_bit, 26u, 32u);
        if (align > num_elements) {
            align = num_elements;
        }
        px_qplc_unpack_be_Nu32u(src_ptr, align, start_bit, 26u, dst_ptr);
        src_ptr += ((align * 26u) + start_bit) >> 3;
        dst_ptr += align * 4;
        num_elements -= align;
    }

    if (num_elements >= 16u) {
        __mmask16 read_mask = OWN_BIT_MASK(OWN_BITS_2_DWORD(26u * OWN_WORD_WIDTH));
        __m512i   parse_mask0 = _mm512_set1_epi32(OWN_BIT_MASK(26u));
        __m512i   nibble_reversemm = _mm512_load_si512(nibble_reverse_table);
        __m512i   reverse_mask_32u = _mm512_load_si512(reverse_mask_table_32u);
        __m512i   maskmm = _mm512_set1_epi8(0x0F);

        __m512i   shuffle_idx_ptr = _mm512_load_si512(shuffle_idx_table_26u_0);

        __m512i   permutex_idx_ptr[2];
        permutex_idx_ptr[0] = _mm512_load_si512(permutex_idx_table_26u_0);
        permutex_idx_ptr[1] = _mm512_load_si512(permutex_idx_table_26u_1);

        __m512i   shift_mask_ptr[3];
        shift_mask_ptr[0] = _mm512_load_si512(shift_table_26u_0);
        shift_mask_ptr[1] = _mm512_load_si512(shift_table_26u_1);
        shift_mask_ptr[2] = _mm512_load_si512(shift_table_26u_2);

        __m512i   gather_idxmm = _mm512_load_si512(gather_idx_table_26u);

        while (num_elements >= 32u) {
            __m512i srcmm, zmm[2];

            srcmm = _mm512_i64gather_epi64(gather_idxmm, src_ptr, 1u);

            zmm[0] = _mm512_shuffle_epi8(srcmm, shuffle_idx_ptr);

            // shifting elements so they start from the start of the word
            zmm[0] = _mm512_srlv_epi32(zmm[0], shift_mask_ptr[2]);
            zmm[0] = _mm512_and_si512(zmm[0], parse_mask0);

            _mm512_storeu_si512(dst_ptr, zmm[0]);

            src_ptr += 2u * 26u;
            dst_ptr += 64u;
            num_elements -= 16u;
        }

        if (num_elements >= 16u) {
            __m512i srcmm, zmm[2];

            srcmm = _mm512_maskz_loadu_epi32(read_mask, src_ptr);

            __m512i low_nibblemm = _mm512_and_si512(srcmm, maskmm);
            __m512i high_nibblemm = _mm512_srli_epi16(srcmm, 4u);
            high_nibblemm = _mm512_and_si512(high_nibblemm, maskmm);

            low_nibblemm = _mm512_shuffle_epi8(nibble_reversemm, low_nibblemm);
            high_nibblemm = _mm512_shuffle_epi8(nibble_reversemm, high_nibblemm);
            low_nibblemm = _mm512_slli_epi16(low_nibblemm, 4u);

            srcmm = _mm512_or_si512(low_nibblemm, high_nibblemm);

            // permuting so in zmm[0] will be elements with even indexes and in zmm[1] - with odd ones
            zmm[0] = _mm512_permutexvar_epi32(permutex_idx_ptr[0], srcmm);
            zmm[1] = _mm512_permutexvar_epi32(permutex_idx_ptr[1], srcmm);

            // shifting elements so they start from the start of the word
            zmm[0] = _mm512_srlv_epi64(zmm[0], shift_mask_ptr[0]);
            zmm[1] = _mm512_sllv_epi64(zmm[1], shift_mask_ptr[1]);

            // gathering even and odd elements together
            zmm[0] = _mm512_mask_mov_epi32(zmm[0], 0xAAAA, zmm[1]);
            zmm[0] = _mm512_and_si512(zmm[0], parse_mask0);

            zmm[0] = _mm512_slli_epi32(zmm[0], 6u);
            low_nibblemm = _mm512_and_si512(zmm[0], maskmm);
            high_nibblemm = _mm512_srli_epi16(zmm[0], 4u);
            high_nibblemm = _mm512_and_si512(high_nibblemm, maskmm);

            low_nibblemm = _mm512_shuffle_epi8(nibble_reversemm, low_nibblemm);
            high_nibblemm = _mm512_shuffle_epi8(nibble_reversemm, high_nibblemm);
            low_nibblemm = _mm512_slli_epi16(low_nibblemm, 4u);

            zmm[0] = _mm512_or_si512(low_nibblemm, high_nibblemm);
            zmm[0] = _mm512_shuffle_epi8(zmm[0], reverse_mask_32u);

            _mm512_storeu_si512(dst_ptr, zmm[0]);

            src_ptr += 2u * 26u;
            dst_ptr += 64u;
            num_elements -= 16u;
        }
    }

    if (num_elements > 0) {
        px_qplc_unpack_be_Nu32u(src_ptr, num_elements, 0u, 26u, dst_ptr);
    }
}

// ********************** 27u ****************************** //

OWN_OPT_FUN(void, k0_qplc_unpack_be_27u32u, (const uint8_t *src_ptr,
    uint32_t num_elements,
    uint32_t start_bit,
    uint8_t *dst_ptr)) {
    if (start_bit > 0u) {
        uint32_t align = own_get_align(start_bit, 27u, 32u);
        if (align > num_elements) {
            align = num_elements;
        }
        px_qplc_unpack_be_Nu32u(src_ptr, align, start_bit, 27u, dst_ptr);
        src_ptr += ((align * 27u) + start_bit) >> 3;
        dst_ptr += align * 4;
        num_elements -= align;
    }

    if (num_elements >= 16u) {
        __mmask32 read_mask = OWN_BIT_MASK(27u);
        __m512i   parse_mask0 = _mm512_set1_epi32(OWN_BIT_MASK(27u));
        __m512i   nibble_reversemm = _mm512_load_si512(nibble_reverse_table);
        __m512i   reverse_mask_32u = _mm512_load_si512(reverse_mask_table_32u);
        __m512i   maskmm = _mm512_set1_epi8(0x0F);

        __m512i   shuffle_idx_ptr[2];
        shuffle_idx_ptr[0] = _mm512_load_si512(shuffle_idx_table_27u_0);
        shuffle_idx_ptr[1] = _mm512_load_si512(shuffle_idx_table_27u_1);

        __m512i   permutex_idx_ptr[2];
        permutex_idx_ptr[0] = _mm512_load_si512(permutex_idx_table_27u_0);
        permutex_idx_ptr[1] = _mm512_load_si512(permutex_idx_table_27u_1);

        __m512i   shift_mask_ptr[4];
        shift_mask_ptr[0] = _mm512_load_si512(shift_table_27u_0);
        shift_mask_ptr[1] = _mm512_load_si512(shift_table_27u_1);
        shift_mask_ptr[2] = _mm512_load_si512(shift_table_27u_2);
        shift_mask_ptr[3] = _mm512_load_si512(shift_table_27u_3);

        __m512i   gather_idxmm = _mm512_load_si512(gather_idx_table_27u);

        while (num_elements >= 32u) {
            __m512i srcmm, zmm[2];

            srcmm = _mm512_i64gather_epi64(gather_idxmm, src_ptr, 1u);

            // shuffling so in zmm[0] will be elements with even indexes and in zmm[1] - with odd ones
            zmm[0] = _mm512_shuffle_epi8(srcmm, shuffle_idx_ptr[0]);
            zmm[1] = _mm512_shuffle_epi8(srcmm, shuffle_idx_ptr[1]);

            // shifting elements so they start from the start of the word
            zmm[0] = _mm512_srlv_epi64(zmm[0], shift_mask_ptr[2]);
            zmm[1] = _mm512_sllv_epi64(zmm[1], shift_mask_ptr[3]);

            // gathering even and odd elements together
            zmm[0] = _mm512_mask_mov_epi32(zmm[0], 0xAAAA, zmm[1]);
            zmm[0] = _mm512_and_si512(zmm[0], parse_mask0);

            _mm512_storeu_si512(dst_ptr, zmm[0]);

            src_ptr += 2u * 27u;
            dst_ptr += 64u;
            num_elements -= 16u;
        }
        if (num_elements >= 16u) {
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
            zmm[0] = _mm512_permutexvar_epi32(permutex_idx_ptr[0], srcmm);
            zmm[1] = _mm512_permutexvar_epi32(permutex_idx_ptr[1], srcmm);

            // shifting elements so they start from the start of the word
            zmm[0] = _mm512_srlv_epi64(zmm[0], shift_mask_ptr[0]);
            zmm[1] = _mm512_sllv_epi64(zmm[1], shift_mask_ptr[1]);

            // gathering even and odd elements together
            zmm[0] = _mm512_mask_mov_epi32(zmm[0], 0xAAAA, zmm[1]);
            zmm[0] = _mm512_and_si512(zmm[0], parse_mask0);

            zmm[0] = _mm512_slli_epi32(zmm[0], 5u);
            low_nibblemm = _mm512_and_si512(zmm[0], maskmm);
            high_nibblemm = _mm512_srli_epi16(zmm[0], 4u);
            high_nibblemm = _mm512_and_si512(high_nibblemm, maskmm);

            low_nibblemm = _mm512_shuffle_epi8(nibble_reversemm, low_nibblemm);
            high_nibblemm = _mm512_shuffle_epi8(nibble_reversemm, high_nibblemm);
            low_nibblemm = _mm512_slli_epi16(low_nibblemm, 4u);

            zmm[0] = _mm512_or_si512(low_nibblemm, high_nibblemm);
            zmm[0] = _mm512_shuffle_epi8(zmm[0], reverse_mask_32u);

            _mm512_storeu_si512(dst_ptr, zmm[0]);

            src_ptr += 2u * 27u;
            dst_ptr += 64u;
            num_elements -= 16u;
        }
    }

    if (num_elements > 0) {
        px_qplc_unpack_be_Nu32u(src_ptr, num_elements, 0u, 27u, dst_ptr);
    }
}

// ********************** 28u ****************************** //

OWN_OPT_FUN(void, k0_qplc_unpack_be_28u32u, (const uint8_t *src_ptr,
    uint32_t num_elements,
    uint32_t start_bit,
    uint8_t *dst_ptr)) {
    if (start_bit > 0u) {
        uint32_t align = own_get_align(start_bit, 28u, 32u);
        if (align > num_elements) {
            align = num_elements;
        }
        px_qplc_unpack_be_Nu32u(src_ptr, align, start_bit, 28u, dst_ptr);
        src_ptr += ((align * 28u) + start_bit) >> 3;
        dst_ptr += align * 4;
        num_elements -= align;
    }

    if (num_elements >= 16u) {
        __mmask16 read_mask = OWN_BIT_MASK(OWN_BITS_2_DWORD(28u * OWN_WORD_WIDTH));
        __m512i   parse_mask0 = _mm512_set1_epi32(OWN_BIT_MASK(28u));

        __m512i   shuffle_idx_ptr = _mm512_load_si512(shuffle_idx_table_28u_0);
        __m512i   permutex_idx = _mm512_load_si512(permutex_idx_table_28u);
        __m512i   shift_mask = _mm512_load_si512(shift_table_28u);

        while (num_elements >= 16u) {
            __m512i srcmm, zmm;

            srcmm = _mm512_maskz_loadu_epi32(read_mask, src_ptr);

            zmm = _mm512_permutexvar_epi16(permutex_idx, srcmm);
            zmm = _mm512_shuffle_epi8(zmm, shuffle_idx_ptr);

            // shifting elements so they start from the start of the word
            zmm = _mm512_srlv_epi32(zmm, shift_mask);
            zmm = _mm512_and_si512(zmm, parse_mask0);

            _mm512_storeu_si512(dst_ptr, zmm);

            src_ptr += 2u * 28u;
            dst_ptr += 64u;
            num_elements -= 16u;
        }
    }

    if (num_elements > 0) {
        px_qplc_unpack_be_Nu32u(src_ptr, num_elements, 0u, 28u, dst_ptr);
    }
}

// ********************** 29u ****************************** //

OWN_OPT_FUN(void, k0_qplc_unpack_be_29u32u, (const uint8_t *src_ptr,
    uint32_t num_elements,
    uint32_t start_bit,
    uint8_t *dst_ptr)) {
    if (start_bit > 0u) {
        uint32_t align = own_get_align(start_bit, 29u, 32u);
        if (align > num_elements) {
            align = num_elements;
        }
        px_qplc_unpack_be_Nu32u(src_ptr, align, start_bit, 29u, dst_ptr);
        src_ptr += ((align * 29u) + start_bit) >> 3;
        dst_ptr += align * 4;
        num_elements -= align;
    }

    if (num_elements >= 16u) {
        __mmask32 read_mask = OWN_BIT_MASK(29u);
        __m512i   parse_mask0 = _mm512_set1_epi32(OWN_BIT_MASK(29u));
        __m512i   nibble_reversemm = _mm512_load_si512(nibble_reverse_table);
        __m512i   reverse_mask_32u = _mm512_load_si512(reverse_mask_table_32u);
        __m512i   maskmm = _mm512_set1_epi8(0x0F);

        __m512i   shuffle_idx_ptr[2];
        shuffle_idx_ptr[0] = _mm512_load_si512(shuffle_idx_table_29u_0);
        shuffle_idx_ptr[1] = _mm512_load_si512(shuffle_idx_table_29u_1);

        __m512i   permutex_idx_ptr[2];
        permutex_idx_ptr[0] = _mm512_load_si512(permutex_idx_table_29u_0);
        permutex_idx_ptr[1] = _mm512_load_si512(permutex_idx_table_29u_1);

        __m512i   shift_mask_ptr[4];
        shift_mask_ptr[0] = _mm512_load_si512(shift_table_29u_0);
        shift_mask_ptr[1] = _mm512_load_si512(shift_table_29u_1);
        shift_mask_ptr[2] = _mm512_load_si512(shift_table_29u_2);
        shift_mask_ptr[3] = _mm512_load_si512(shift_table_29u_3);

        __m512i   gather_idxmm = _mm512_load_si512(gather_idx_table_29u);

        while (num_elements >= 32u) {
            __m512i srcmm, zmm[2];

            srcmm = _mm512_i64gather_epi64(gather_idxmm, src_ptr, 1u);

            // shuffling so in zmm[0] will be elements with even indexes and in zmm[1] - with odd ones
            zmm[0] = _mm512_shuffle_epi8(srcmm, shuffle_idx_ptr[0]);
            zmm[1] = _mm512_shuffle_epi8(srcmm, shuffle_idx_ptr[1]);

            // shifting elements so they start from the start of the word
            zmm[0] = _mm512_srlv_epi64(zmm[0], shift_mask_ptr[2]);
            zmm[1] = _mm512_sllv_epi64(zmm[1], shift_mask_ptr[3]);

            // gathering even and odd elements together
            zmm[0] = _mm512_mask_mov_epi32(zmm[0], 0xAAAA, zmm[1]);
            zmm[0] = _mm512_and_si512(zmm[0], parse_mask0);

            _mm512_storeu_si512(dst_ptr, zmm[0]);

            src_ptr += 2u * 29u;
            dst_ptr += 64u;
            num_elements -= 16u;
        }
        if (num_elements >= 16u) {
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
            zmm[0] = _mm512_permutexvar_epi32(permutex_idx_ptr[0], srcmm);
            zmm[1] = _mm512_permutexvar_epi32(permutex_idx_ptr[1], srcmm);

            // shifting elements so they start from the start of the word
            zmm[0] = _mm512_srlv_epi64(zmm[0], shift_mask_ptr[0]);
            zmm[1] = _mm512_sllv_epi64(zmm[1], shift_mask_ptr[1]);

            // gathering even and odd elements together
            zmm[0] = _mm512_mask_mov_epi32(zmm[0], 0xAAAA, zmm[1]);
            zmm[0] = _mm512_and_si512(zmm[0], parse_mask0);

            zmm[0] = _mm512_slli_epi32(zmm[0], 3u);
            low_nibblemm = _mm512_and_si512(zmm[0], maskmm);
            high_nibblemm = _mm512_srli_epi16(zmm[0], 4u);
            high_nibblemm = _mm512_and_si512(high_nibblemm, maskmm);

            low_nibblemm = _mm512_shuffle_epi8(nibble_reversemm, low_nibblemm);
            high_nibblemm = _mm512_shuffle_epi8(nibble_reversemm, high_nibblemm);
            low_nibblemm = _mm512_slli_epi16(low_nibblemm, 4u);

            zmm[0] = _mm512_or_si512(low_nibblemm, high_nibblemm);
            zmm[0] = _mm512_shuffle_epi8(zmm[0], reverse_mask_32u);

            _mm512_storeu_si512(dst_ptr, zmm[0]);

            src_ptr += 2u * 29u;
            dst_ptr += 64u;
            num_elements -= 16u;
        }
    }

    if (num_elements > 0) {
        px_qplc_unpack_be_Nu32u(src_ptr, num_elements, 0u, 29u, dst_ptr);
    }
}

// ********************** 30u ****************************** //

OWN_OPT_FUN(void, k0_qplc_unpack_be_30u32u, (const uint8_t *src_ptr,
    uint32_t num_elements,
    uint32_t start_bit,
    uint8_t *dst_ptr)) {
    if (start_bit > 0u) {
        uint32_t align = own_get_align(start_bit, 30u, 32u);
        if (align > num_elements) {
            align = num_elements;
        }
        px_qplc_unpack_be_Nu32u(src_ptr, align, start_bit, 30u, dst_ptr);
        src_ptr += ((align * 30u) + start_bit) >> 3;
        dst_ptr += align * 4;
        num_elements -= align;
    }

    if (num_elements >= 16u) {
        __mmask16 read_mask = OWN_BIT_MASK(OWN_BITS_2_DWORD(30u * OWN_WORD_WIDTH));
        __m512i   parse_mask0 = _mm512_set1_epi32(OWN_BIT_MASK(30u));
        __m512i   nibble_reversemm = _mm512_load_si512(nibble_reverse_table);
        __m512i   reverse_mask_32u = _mm512_load_si512(reverse_mask_table_32u);
        __m512i   maskmm = _mm512_set1_epi8(0x0F);

        __m512i   shuffle_idx_ptr[2];
        shuffle_idx_ptr[0] = _mm512_load_si512(shuffle_idx_table_30u_0);
        shuffle_idx_ptr[1] = _mm512_load_si512(shuffle_idx_table_30u_1);

        __m512i   permutex_idx_ptr[2];
        permutex_idx_ptr[0] = _mm512_load_si512(permutex_idx_table_30u_0);
        permutex_idx_ptr[1] = _mm512_load_si512(permutex_idx_table_30u_1);

        __m512i   shift_mask_ptr[4];
        shift_mask_ptr[0] = _mm512_load_si512(shift_table_30u_0);
        shift_mask_ptr[1] = _mm512_load_si512(shift_table_30u_1);
        shift_mask_ptr[2] = _mm512_load_si512(shift_table_30u_2);
        shift_mask_ptr[3] = _mm512_load_si512(shift_table_30u_3);

        __m512i   gather_idxmm = _mm512_load_si512(gather_idx_table_30u);

        while (num_elements >= 32u) {
            __m512i srcmm, zmm[2];

            srcmm = _mm512_i64gather_epi64(gather_idxmm, src_ptr, 1u);

            // shuffling so in zmm[0] will be elements with even indexes and in zmm[1] - with odd ones
            zmm[0] = _mm512_shuffle_epi8(srcmm, shuffle_idx_ptr[0]);
            zmm[1] = _mm512_shuffle_epi8(srcmm, shuffle_idx_ptr[1]);

            // shifting elements so they start from the start of the word
            zmm[0] = _mm512_srlv_epi64(zmm[0], shift_mask_ptr[2]);
            zmm[1] = _mm512_sllv_epi64(zmm[1], shift_mask_ptr[3]);

            // gathering even and odd elements together
            zmm[0] = _mm512_mask_mov_epi32(zmm[0], 0xAAAA, zmm[1]);
            zmm[0] = _mm512_and_si512(zmm[0], parse_mask0);

            _mm512_storeu_si512(dst_ptr, zmm[0]);

            src_ptr += 2u * 30u;
            dst_ptr += 64u;
            num_elements -= 16u;
        }
        if (num_elements >= 16u) {
            __m512i srcmm, zmm[2];

            srcmm = _mm512_maskz_loadu_epi32(read_mask, src_ptr);

            __m512i low_nibblemm = _mm512_and_si512(srcmm, maskmm);
            __m512i high_nibblemm = _mm512_srli_epi16(srcmm, 4u);
            high_nibblemm = _mm512_and_si512(high_nibblemm, maskmm);

            low_nibblemm = _mm512_shuffle_epi8(nibble_reversemm, low_nibblemm);
            high_nibblemm = _mm512_shuffle_epi8(nibble_reversemm, high_nibblemm);
            low_nibblemm = _mm512_slli_epi16(low_nibblemm, 4u);

            srcmm = _mm512_or_si512(low_nibblemm, high_nibblemm);

            // permuting so in zmm[0] will be elements with even indexes and in zmm[1] - with odd ones
            zmm[0] = _mm512_permutexvar_epi32(permutex_idx_ptr[0], srcmm);
            zmm[1] = _mm512_permutexvar_epi32(permutex_idx_ptr[1], srcmm);

            // shifting elements so they start from the start of the word
            zmm[0] = _mm512_srlv_epi64(zmm[0], shift_mask_ptr[0]);
            zmm[1] = _mm512_sllv_epi64(zmm[1], shift_mask_ptr[1]);

            // gathering even and odd elements together
            zmm[0] = _mm512_mask_mov_epi32(zmm[0], 0xAAAA, zmm[1]);
            zmm[0] = _mm512_and_si512(zmm[0], parse_mask0);

            zmm[0] = _mm512_slli_epi32(zmm[0], 2u);
            low_nibblemm = _mm512_and_si512(zmm[0], maskmm);
            high_nibblemm = _mm512_srli_epi16(zmm[0], 4u);
            high_nibblemm = _mm512_and_si512(high_nibblemm, maskmm);

            low_nibblemm = _mm512_shuffle_epi8(nibble_reversemm, low_nibblemm);
            high_nibblemm = _mm512_shuffle_epi8(nibble_reversemm, high_nibblemm);
            low_nibblemm = _mm512_slli_epi16(low_nibblemm, 4u);

            zmm[0] = _mm512_or_si512(low_nibblemm, high_nibblemm);
            zmm[0] = _mm512_shuffle_epi8(zmm[0], reverse_mask_32u);

            _mm512_storeu_si512(dst_ptr, zmm[0]);

            src_ptr += 2u * 30u;
            dst_ptr += 64u;
            num_elements -= 16u;
        }
    }

    if (num_elements > 0) {
        px_qplc_unpack_be_Nu32u(src_ptr, num_elements, 0u, 30u, dst_ptr);
    }
}

// ********************** 31u ****************************** //

OWN_OPT_FUN(void, k0_qplc_unpack_be_31u32u, (const uint8_t *src_ptr,
    uint32_t num_elements,
    uint32_t start_bit,
    uint8_t *dst_ptr)) {
    if (start_bit > 0u) {
        uint32_t align = own_get_align(start_bit, 31u, 32u);
        if (align > num_elements) {
            align = num_elements;
        }
        px_qplc_unpack_be_Nu32u(src_ptr, align, start_bit, 31u, dst_ptr);
        src_ptr += ((align * 31u) + start_bit) >> 3;
        dst_ptr += align * 4;
        num_elements -= align;
    }

    if (num_elements >= 16u) {
        __mmask32 read_mask = OWN_BIT_MASK(31u);
        __m512i   parse_mask0 = _mm512_set1_epi32(OWN_BIT_MASK(31u));
        __m512i   nibble_reversemm = _mm512_load_si512(nibble_reverse_table);
        __m512i   reverse_mask_32u = _mm512_load_si512(reverse_mask_table_32u);
        __m512i   maskmm = _mm512_set1_epi8(0x0F);

        __m512i   shuffle_idx_ptr[2];
        shuffle_idx_ptr[0] = _mm512_load_si512(shuffle_idx_table_31u_0);
        shuffle_idx_ptr[1] = _mm512_load_si512(shuffle_idx_table_31u_1);

        __m512i   permutex_idx_ptr[2];
        permutex_idx_ptr[0] = _mm512_load_si512(permutex_idx_table_31u_0);
        permutex_idx_ptr[1] = _mm512_load_si512(permutex_idx_table_31u_1);

        __m512i   shift_mask_ptr[4];
        shift_mask_ptr[0] = _mm512_load_si512(shift_table_31u_0);
        shift_mask_ptr[1] = _mm512_load_si512(shift_table_31u_1);
        shift_mask_ptr[2] = _mm512_load_si512(shift_table_31u_2);
        shift_mask_ptr[3] = _mm512_load_si512(shift_table_31u_3);

        __m512i   gather_idxmm = _mm512_load_si512(gather_idx_table_31u);

        while (num_elements >= 32u) {
            __m512i srcmm, zmm[2];

            srcmm = _mm512_i64gather_epi64(gather_idxmm, src_ptr, 1u);

            // shuffling so in zmm[0] will be elements with even indexes and in zmm[1] - with odd ones
            zmm[0] = _mm512_shuffle_epi8(srcmm, shuffle_idx_ptr[0]);
            zmm[1] = _mm512_shuffle_epi8(srcmm, shuffle_idx_ptr[1]);

            // shifting elements so they start from the start of the word
            zmm[0] = _mm512_srlv_epi64(zmm[0], shift_mask_ptr[2]);
            zmm[1] = _mm512_sllv_epi64(zmm[1], shift_mask_ptr[3]);

            // gathering even and odd elements together
            zmm[0] = _mm512_mask_mov_epi32(zmm[0], 0xAAAA, zmm[1]);
            zmm[0] = _mm512_and_si512(zmm[0], parse_mask0);

            _mm512_storeu_si512(dst_ptr, zmm[0]);

            src_ptr += 2u * 31u;
            dst_ptr += 64u;
            num_elements -= 16u;
        }
        if (num_elements >= 16u) {
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
            zmm[0] = _mm512_permutexvar_epi32(permutex_idx_ptr[0], srcmm);
            zmm[1] = _mm512_permutexvar_epi32(permutex_idx_ptr[1], srcmm);

            // shifting elements so they start from the start of the word
            zmm[0] = _mm512_srlv_epi64(zmm[0], shift_mask_ptr[0]);
            zmm[1] = _mm512_sllv_epi64(zmm[1], shift_mask_ptr[1]);

            // gathering even and odd elements together
            zmm[0] = _mm512_mask_mov_epi32(zmm[0], 0xAAAA, zmm[1]);
            zmm[0] = _mm512_and_si512(zmm[0], parse_mask0);

            zmm[0] = _mm512_slli_epi32(zmm[0], 1u);
            low_nibblemm = _mm512_and_si512(zmm[0], maskmm);
            high_nibblemm = _mm512_srli_epi16(zmm[0], 4u);
            high_nibblemm = _mm512_and_si512(high_nibblemm, maskmm);

            low_nibblemm = _mm512_shuffle_epi8(nibble_reversemm, low_nibblemm);
            high_nibblemm = _mm512_shuffle_epi8(nibble_reversemm, high_nibblemm);
            low_nibblemm = _mm512_slli_epi16(low_nibblemm, 4u);

            zmm[0] = _mm512_or_si512(low_nibblemm, high_nibblemm);
            zmm[0] = _mm512_shuffle_epi8(zmm[0], reverse_mask_32u);

            _mm512_storeu_si512(dst_ptr, zmm[0]);

            src_ptr += 2u * 31u;
            dst_ptr += 64u;
            num_elements -= 16u;
        }
    }

    if (num_elements > 0) {
        px_qplc_unpack_be_Nu32u(src_ptr, num_elements, 0u, 31u, dst_ptr);
    }
}

// ********************** 32u ****************************** //

OWN_OPT_FUN(void, k0_qplc_unpack_be_32u32u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr)) {
    if (num_elements >= 16u) {
        __m512i reverse_mask_32u = _mm512_load_si512(reverse_mask_table_32u);
        while (num_elements >= 16u) {
            __m512i srcmm = _mm512_loadu_si512(src_ptr);
            srcmm = _mm512_shuffle_epi8(srcmm, reverse_mask_32u);
            _mm512_storeu_si512(dst_ptr, srcmm);

            src_ptr += 2u * 32u;
            dst_ptr += 64u;
            num_elements -= 16u;
        }
    }
    uint32_t *src32u_ptr = (uint32_t *) src_ptr;
    uint32_t *dst32u_ptr = (uint32_t *) dst_ptr;

    for (uint32_t i = 0u; i < num_elements; i++) {
        dst32u_ptr[i] = qplc_swap_bytes_32u(src32u_ptr[i]);
    }
}
