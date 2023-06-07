/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/*------- qplc_pack_16u_k0.h -------*/

/**
 * @brief Contains implementation of functions for vector packing byte integers to 9...16-bit integers
 * @date 16/02/2021
 *
 * @details Function list:
 *          - @ref k0_qplc_pack_16u9u
 *          - @ref k0_qplc_pack_16u10u
 *          - @ref k0_qplc_pack_16u11u
 *          - @ref k0_qplc_pack_16u12u
 *          - @ref k0_qplc_pack_16u13u
 *          - @ref k0_qplc_pack_16u14u
 *          - @ref k0_qplc_pack_16u15u
 *          - @ref k0_qplc_pack_16u32u
 *
 */
#ifndef OWN_PACK_16U_H
#define OWN_PACK_16U_H

#include "own_qplc_defs.h"

// *********************** Masks  ****************************** //

// ----------------------- 16u9u ------------------------------- //
OWN_ALIGNED_64_ARRAY(static uint16_t permutex_idx_table_9u_0[32]) = {
    0u, 2u, 4u, 6u, 0x0, 9u, 11u, 13u, 15u, 16u, 18u, 20u, 22u, 0x0, 25u, 27u,
    29u, 31u, 32u, 34u, 36u, 38u, 0x0, 41u, 43u, 45u, 47u, 0x0, 0x0, 0x0, 0x0, 0x0};
OWN_ALIGNED_64_ARRAY(static uint16_t permutex_idx_table_9u_1[32]) = {
    1u, 3u, 5u, 7u, 8u, 10u, 12u, 14u, 0x0, 17u, 19u, 21u, 23u, 24u, 26u, 28u,
    30u, 0x0, 33u, 35u, 37u, 39u, 40u, 42u, 44u, 46u, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0};
OWN_ALIGNED_64_ARRAY(static uint16_t permutex_idx_table_9u_2[32]) = {
    0x0, 1u, 3u, 5u, 7u, 8u, 10u, 12u, 14u, 0x0, 17u, 19u, 21u, 23u, 24u, 26u,
    28u, 30u, 0x0, 33u, 35u, 37u, 39u, 40u, 42u, 44u, 46u, 0x0, 0x0, 0x0, 0x0, 0x0};
OWN_ALIGNED_64_ARRAY(static uint16_t permutex_idx_table_9u_3[32]) = {
    16u, 18u, 20u, 22u, 0x0, 25u, 27u, 29u, 31u, 32u, 34u, 36u, 38u, 0x0, 41u, 43u,
    45u, 47u, 48u, 50u, 52u, 54u, 0x0, 57u, 59u, 61u, 63u, 0x0, 0x0, 0x0, 0x0, 0x0};
OWN_ALIGNED_64_ARRAY(static uint16_t permutex_idx_table_9u_4[32]) = {
    17u, 19u, 21u, 23u, 24u, 26u, 28u, 30u, 0x0, 33u, 35u, 37u, 39u, 40u, 42u, 44u,
    46u, 0x0, 49u, 51u, 53u, 55u, 56u, 58u, 60u, 62u, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0};
OWN_ALIGNED_64_ARRAY(static uint16_t permutex_idx_table_9u_5[32]) = {
    0x0, 17u, 19u, 21u, 23u, 24u, 26u, 28u, 30u, 0x0, 33u, 35u, 37u, 39u, 40u, 42u,
    44u, 46u, 0x0, 49u, 51u, 53u, 55u, 56u, 58u, 60u, 62u, 0x0, 0x0, 0x0, 0x0, 0x0};
static __mmask32 permutex_masks_9u_ptr[3] = {0x07BFDFEF, 0x03FDFEFF, 0x07FBFDFE};

OWN_ALIGNED_64_ARRAY(static uint16_t shift_mask_table_9u_0[32]) = {
    0u, 2u, 4u, 6u, 0u, 1u, 3u, 5u, 7u, 0u, 2u, 4u, 6u, 0u, 1u, 3u,
    5u, 7u, 0u, 2u, 4u, 6u, 0u, 1u, 3u, 5u, 7u, 0x0, 0x0, 0x0, 0x0, 0x0};
OWN_ALIGNED_64_ARRAY(static uint16_t shift_mask_table_9u_1[32]) = {
    9u, 11u, 13u, 15u, 8u, 10u, 12u, 14u, 8u, 9u, 11u, 13u, 15u, 8u, 10u, 12u,
    14u, 8u, 9u, 11u, 13u, 15u, 8u, 10u, 12u, 14u, 8u, 0x0, 0x0, 0x0, 0x0, 0x0};
OWN_ALIGNED_64_ARRAY(static uint16_t shift_mask_table_9u_2[32]) = {
    0u, 7u, 5u, 3u, 1u, 8u, 6u, 4u, 2u, 0u, 7u, 5u, 3u, 1u, 8u, 6u,
    4u, 2u, 0u, 7u, 5u, 3u, 1u, 8u, 6u, 4u, 2u, 0x0, 0x0, 0x0, 0x0, 0x0};

// ----------------------- 16u10u ------------------------------ //
OWN_ALIGNED_64_ARRAY(static uint16_t permutex_idx_table_10u_0[32]) = {
    0u, 2u, 0x0, 5u, 7u, 8u, 10u, 0x0, 13u, 15u, 16u, 18u, 0x0, 21u, 23u, 24u,
    26u, 0x0, 29u, 31u, 32u, 34u, 0x0, 37u, 39u, 40u, 42u, 0x0, 45u, 47u, 0x0, 0x0};
OWN_ALIGNED_64_ARRAY(static uint16_t permutex_idx_table_10u_1[32]) = {
    1u, 3u, 4u, 6u, 0x0, 9u, 11u, 12u, 14u, 0x0, 17u, 19u, 20u, 22u, 0x0, 25u,
    27u, 28u, 30u, 0x0, 33u, 35u, 36u, 38u, 0x0, 41u, 43u, 44u, 46u, 0x0, 0x0, 0x0};
OWN_ALIGNED_64_ARRAY(static uint16_t permutex_idx_table_10u_2[32]) = {
    0x0, 1u, 3u, 4u, 6u, 0x0, 9u, 11u, 12u, 14u, 0x0, 17u, 19u, 20u, 22u, 0x0,
    25u, 27u, 28u, 30u, 0x0, 33u, 35u, 36u, 38u, 0x0, 41u, 43u, 44u, 46u, 0x0, 0x0};
OWN_ALIGNED_64_ARRAY(static uint16_t permutex_idx_table_10u_3[32]) = {
    16u, 18u, 0x0, 21u, 23u, 24u, 26u, 0x0, 29u, 31u, 32u, 34u, 0x0, 37u, 39u, 40u,
    42u, 0x0, 45u, 47u, 48u, 50u, 0x0, 53u, 55u, 56u, 58u, 0x0, 61u, 63u, 0x0, 0x0};
OWN_ALIGNED_64_ARRAY(static uint16_t permutex_idx_table_10u_4[32]) = {
    17u, 19u, 20u, 22u, 0x0, 25u, 27u, 28u, 30u, 0x0, 33u, 35u, 36u, 38u, 0x0, 41u,
    43u, 44u, 46u, 0x0, 49u, 51u, 52u, 54u, 0x0, 57u, 59u, 60u, 62u, 0x0, 0x0, 0x0};
OWN_ALIGNED_64_ARRAY(static uint16_t permutex_idx_table_10u_5[32]) = {
    0x0, 17u, 19u, 20u, 22u, 0x0, 25u, 27u, 28u, 30u, 0x0, 33u, 35u, 36u, 38u, 0x0,
    41u, 43u, 44u, 46u, 0x0, 49u, 51u, 52u, 54u, 0x0, 57u, 59u, 60u, 62u, 0x0, 0x0};
static __mmask32 permutex_masks_10u_ptr[3] = {0x37BDEF7B, 0x1EF7BDEF, 0x3DEF7BDE};

OWN_ALIGNED_64_ARRAY(static uint16_t shift_mask_table_10u_0[32]) = {
    0u, 4u, 0u, 2u, 6u, 0u, 4u, 0u, 2u, 6u, 0u, 4u, 0u, 2u, 6u, 0u,
    4u, 0u, 2u, 6u, 0u, 4u, 0u, 2u, 6u, 0u, 4u, 0u, 2u, 6u, 0x0, 0x0};
OWN_ALIGNED_64_ARRAY(static uint16_t shift_mask_table_10u_1[32]) = {
    10u, 14u, 8u, 12u, 0u, 10u, 14u, 8u, 12u, 0u, 10u, 14u, 8u, 12u, 0u, 10u,
    14u, 8u, 12u, 0u, 10u, 14u, 8u, 12u, 0u, 10u, 14u, 8u, 12u, 0u, 0x0, 0x0};
OWN_ALIGNED_64_ARRAY(static uint16_t shift_mask_table_10u_2[32]) = {
    0u, 6u, 2u, 8u, 4u, 0u, 6u, 2u, 8u, 4u, 0u, 6u, 2u, 8u, 4u, 0u,
    6u, 2u, 8u, 4u, 0u, 6u, 2u, 8u, 4u, 0u, 6u, 2u, 8u, 4u, 0x0, 0x0};

// ----------------------- 16u11u ------------------------------ //
OWN_ALIGNED_64_ARRAY(static uint16_t permutex_idx_table_11u_0[32]) = {
    0u, 2u, 3u, 5u, 6u, 0x0, 9u, 0x0, 12u, 0x0, 15u, 16u, 18u, 19u, 21u, 22u,
    0x0, 25u, 0x0, 28u, 0x0, 31u, 32u, 34u, 35u, 37u, 38u, 0x0, 0x0, 0x0, 0x0, 0x0};
OWN_ALIGNED_64_ARRAY(static uint16_t permutex_idx_table_11u_1[32]) = {
    1u, 0x0, 4u, 0x0, 7u, 8u, 10u, 11u, 13u, 14u, 0x0, 17u, 0x0, 20u, 0x0, 23u,
    24u, 26u, 27u, 29u, 30u, 0x0, 33u, 0x0, 36u, 0x0, 39u, 40u, 0x0, 0x0, 0x0, 0x0};
OWN_ALIGNED_64_ARRAY(static uint16_t permutex_idx_table_11u_2[32]) = {
    0x0, 1u, 2u, 4u, 5u, 7u, 8u, 10u, 11u, 13u, 14u, 0x0, 17u, 18u, 20u, 21u,
    23u, 24u, 26u, 27u, 29u, 30u, 0x0, 33u, 34u, 36u, 37u, 39u, 0x0, 0x0, 0x0, 0x0};

OWN_ALIGNED_64_ARRAY(static uint16_t permutex_idx_table_11u_3[32]) = {
    9u, 0x0, 12u, 0x0, 15u, 16u, 18u, 19u, 21u, 22u, 0x0, 25u, 0x0, 28u, 0x0, 31u,
    32u, 34u, 35u, 37u, 38u, 0x0, 41u, 0x0, 44u, 0x0, 47u, 0x0, 0x0, 0x0, 0x0, 0x0};
OWN_ALIGNED_64_ARRAY(static uint16_t permutex_idx_table_11u_4[32]) = {
    10u, 11u, 13u, 14u, 0x0, 17u, 0x0, 20u, 0x0, 23u, 24u, 26u, 27u, 29u, 30u, 0x0,
    33u, 0x0, 36u, 0x0, 39u, 40u, 42u, 43u, 45u, 46u, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0};
OWN_ALIGNED_64_ARRAY(static uint16_t permutex_idx_table_11u_5[32]) = {
    8u, 10u, 11u, 13u, 14u, 0x0, 17u, 18u, 20u, 21u, 23u, 24u, 26u, 27u, 29u, 30u,
    0x0, 33u, 34u, 36u, 37u, 39u, 40u, 42u, 43u, 45u, 46u, 0x0, 0x0, 0x0, 0x0, 0x0};

OWN_ALIGNED_64_ARRAY(static uint16_t permutex_idx_table_11u_6[32]) = {
    16u, 18u, 19u, 21u, 22u, 0x0, 25u, 0x0, 28u, 0x0, 31u, 32u, 34u, 35u, 37u, 38u,
    0x0, 41u, 0x0, 44u, 0x0, 47u, 48u, 50u, 51u, 53u, 54u, 0x0, 0x0, 0x0, 0x0, 0x0};
OWN_ALIGNED_64_ARRAY(static uint16_t permutex_idx_table_11u_7[32]) = {
    17u, 0x0, 20u, 0x0, 23u, 24u, 26u, 27u, 29u, 30u, 0x0, 33u, 0x0, 36u, 0x0, 39u,
    40u, 42u, 43u, 45u, 46u, 0x0, 49u, 0x0, 52u, 0x0, 55u, 56u, 0x0, 0x0, 0x0, 0x0};
OWN_ALIGNED_64_ARRAY(static uint16_t permutex_idx_table_11u_8[32]) = {
    0x0, 17u, 18u, 20u, 21u, 23u, 24u, 26u, 27u, 29u, 30u, 0x0, 33u, 34u, 36u, 37u,
    39u, 40u, 42u, 43u, 45u, 46u, 0x0, 49u, 50u, 52u, 53u, 55u, 0x0, 0x0, 0x0, 0x0};

OWN_ALIGNED_64_ARRAY(static uint16_t permutex_idx_table_11u_9[32]) = {
    25u, 0x0, 28u, 0x0, 31u, 32u, 34u, 35u, 37u, 38u, 0x0, 41u, 0x0, 44u, 0x0, 47u,
    48u, 50u, 51u, 53u, 54u, 0x0, 57u, 0x0, 60u, 0x0, 63u, 0x0, 0x0, 0x0, 0x0, 0x0};
OWN_ALIGNED_64_ARRAY(static uint16_t permutex_idx_table_11u_10[32]) = {
    26u, 27u, 29u, 30u, 0x0, 33u, 0x0, 36u, 0x0, 39u, 40u, 42u, 43u, 45u, 46u, 0x0,
    49u, 0x0, 52u, 0x0, 55u, 56u, 58u, 59u, 61u, 62u, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0};
OWN_ALIGNED_64_ARRAY(static uint16_t permutex_idx_table_11u_11[32]) = {
    24u, 26u, 27u, 29u, 30u, 0x0, 33u, 34u, 36u, 37u, 39u, 40u, 42u, 43u, 45u, 46u,
    0x0, 49u, 50u, 52u, 53u, 55u, 56u, 58u, 59u, 61u, 62u, 0x0, 0x0, 0x0, 0x0, 0x0};
static __mmask32 permutex_masks_11u_ptr[6] = {0x07EAFD5F, 0x0D5FABF5, 0x0FBFF7FE, 0x055FABF5, 0x03F57EAF, 0x07FEFFDF};

OWN_ALIGNED_64_ARRAY(static uint16_t shift_mask_table_11u_0[32]) = {
    0u, 6u, 1u, 7u, 2u, 0u, 3u, 0u, 4u, 0u, 5u, 0u, 6u, 1u, 7u, 2u,
    0u, 3u, 0u, 4u, 0u, 5u, 0u, 6u, 1u, 7u, 2u, 0u, 0x0, 0x0, 0x0, 0x0};
OWN_ALIGNED_64_ARRAY(static uint16_t shift_mask_table_11u_1[32]) = {
    11u, 8u, 12u, 8u, 13u, 8u, 14u, 9u, 15u, 10u, 8u, 11u, 8u, 12u, 8u, 13u,
    8u, 14u, 9u, 15u, 10u, 8u, 11u, 8u, 12u, 8u, 13u, 8u, 0x0, 0x0, 0x0, 0x0};
OWN_ALIGNED_64_ARRAY(static uint16_t shift_mask_table_11u_2[32]) = {
    0u, 5u, 10u, 4u, 9u, 3u, 8u, 2u, 7u, 1u, 6u, 0u, 5u, 10u, 4u, 9u,
    3u, 8u, 2u, 7u, 1u, 6u, 0u, 5u, 10u, 4u, 9u, 3u, 0x0, 0x0, 0x0, 0x0};
OWN_ALIGNED_64_ARRAY(static uint16_t shift_mask_table_11u_3[32]) = {
    3u, 0u, 4u, 0u, 5u, 0u, 6u, 1u, 7u, 2u, 0u, 3u, 0u, 4u, 0u, 5u,
    0u, 6u, 1u, 7u, 2u, 0u, 3u, 0u, 4u, 0u, 5u, 0x0, 0x0, 0x0, 0x0, 0x0};
OWN_ALIGNED_64_ARRAY(static uint16_t shift_mask_table_11u_4[32]) = {
    14u, 9u, 15u, 10u, 8u, 11u, 8u, 12u, 8u, 13u, 8u, 14u, 9u, 15u, 10u, 8u,
    11u, 8u, 12u, 8u, 13u, 8u, 14u, 9u, 15u, 10u, 8u, 0x0, 0x0, 0x0, 0x0, 0x0};
OWN_ALIGNED_64_ARRAY(static uint16_t shift_mask_table_11u_5[32]) = {
    8u, 2u, 7u, 1u, 6u, 0u, 5u, 10u, 4u, 9u, 3u, 8u, 2u, 7u, 1u, 6u,
    0u, 5u, 10u, 4u, 9u, 3u, 8u, 2u, 7u, 1u, 6u, 0x0, 0x0, 0x0, 0x0, 0x0};

// ----------------------- 16u12u ------------------------------ //
OWN_ALIGNED_64_ARRAY(static uint16_t permutex_idx_table_12u_0[32]) = {
    1u, 2u, 3u, 5u, 6u, 7u, 9u, 10u, 11u, 13u, 14u, 15u, 17u, 18u, 19u, 21u,
    22u, 23u, 25u, 26u, 27u, 29u, 30u, 31u, 33u, 34u, 35u, 37u, 38u, 39u, 41u, 42u};
OWN_ALIGNED_64_ARRAY(static uint16_t permutex_idx_table_12u_1[32]) = {
    0u, 1u, 2u, 4u, 5u, 6u, 8u, 9u, 10u, 12u, 13u, 14u, 16u, 17u, 18u, 20u,
    21u, 22u, 24u, 25u, 26u, 28u, 29u, 30u, 32u, 33u, 34u, 36u, 37u, 38u, 40u, 41u};
OWN_ALIGNED_64_ARRAY(static uint16_t permutex_idx_table_12u_2[32]) = {
    11u, 13u, 14u, 15u, 17u, 18u, 19u, 21u, 22u, 23u, 25u, 26u, 27u, 29u, 30u, 31u,
    33u, 34u, 35u, 37u, 38u, 39u, 41u, 42u, 43u, 45u, 46u, 47u, 49u, 50u, 51u, 53u};
OWN_ALIGNED_64_ARRAY(static uint16_t permutex_idx_table_12u_3[32]) = {
    10u, 12u, 13u, 14u, 16u, 17u, 18u, 20u, 21u, 22u, 24u, 25u, 26u, 28u, 29u, 30u,
    32u, 33u, 34u, 36u, 37u, 38u, 40u, 41u, 42u, 44u, 45u, 46u, 48u, 49u, 50u, 52u};
OWN_ALIGNED_64_ARRAY(static uint16_t permutex_idx_table_12u_4[32]) = {
    22u, 23u, 25u, 26u, 27u, 29u, 30u, 31u, 33u, 34u, 35u, 37u, 38u, 39u, 41u, 42u,
    43u, 45u, 46u, 47u, 49u, 50u, 51u, 53u, 54u, 55u, 57u, 58u, 59u, 61u, 62u, 63u};
OWN_ALIGNED_64_ARRAY(static uint16_t permutex_idx_table_12u_5[32]) = {
    21u, 22u, 24u, 25u, 26u, 28u, 29u, 30u, 32u, 33u, 34u, 36u, 37u, 38u, 40u, 41u,
    42u, 44u, 45u, 46u, 48u, 49u, 50u, 52u, 53u, 54u, 56u, 57u, 58u, 60u, 61u, 62u};

OWN_ALIGNED_64_ARRAY(static uint16_t shift_mask_table_12u_0[32]) = {
    12u, 8u, 4u, 12u, 8u, 4u, 12u, 8u, 4u, 12u, 8u, 4u, 12u, 8u, 4u, 12u,
    8u, 4u, 12u, 8u, 4u, 12u, 8u, 4u, 12u, 8u, 4u, 12u, 8u, 4u, 12u, 8u};
OWN_ALIGNED_64_ARRAY(static uint16_t shift_mask_table_12u_1[32]) = {
    0u, 4u, 8u, 0u, 4u, 8u, 0u, 4u, 8u, 0u, 4u, 8u, 0u, 4u, 8u, 0u,
    4u, 8u, 0u, 4u, 8u, 0u, 4u, 8u, 0u, 4u, 8u, 0u, 4u, 8u, 0u, 4u};
OWN_ALIGNED_64_ARRAY(static uint16_t shift_mask_table_12u_2[32]) = {
    4u, 12u, 8u, 4u, 12u, 8u, 4u, 12u, 8u, 4u, 12u, 8u, 4u, 12u, 8u, 4u,
    12u, 8u, 4u, 12u, 8u, 4u, 12u, 8u, 4u, 12u, 8u, 4u, 12u, 8u, 4u, 12u};
OWN_ALIGNED_64_ARRAY(static uint16_t shift_mask_table_12u_3[32]) = {
    8u, 0u, 4u, 8u, 0u, 4u, 8u, 0u, 4u, 8u, 0u, 4u, 8u, 0u, 4u, 8u,
    0u, 4u, 8u, 0u, 4u, 8u, 0u, 4u, 8u, 0u, 4u, 8u, 0u, 4u, 8u, 0u};
OWN_ALIGNED_64_ARRAY(static uint16_t shift_mask_table_12u_4[32]) = {
    8u, 4u, 12u, 8u, 4u, 12u, 8u, 4u, 12u, 8u, 4u, 12u, 8u, 4u, 12u, 8u,
    4u, 12u, 8u, 4u, 12u, 8u, 4u, 12u, 8u, 4u, 12u, 8u, 4u, 12u, 8u, 4u};
OWN_ALIGNED_64_ARRAY(static uint16_t shift_mask_table_12u_5[32]) = {
    4u, 8u, 0u, 4u, 8u, 0u, 4u, 8u, 0u, 4u, 8u, 0u, 4u, 8u, 0u, 4u,
    8u, 0u, 4u, 8u, 0u, 4u, 8u, 0u, 4u, 8u, 0u, 4u, 8u, 0u, 4u, 8u};

// ----------------------- 16u13u ------------------------------ //
OWN_ALIGNED_64_ARRAY(static uint16_t permutex_idx_table_13u_0[32]) = {
0u, 0x0, 3u, 4u, 5u, 0x0, 0x0, 9u, 10u, 0x0, 0x0, 14u, 15u, 16u, 0x0, 19u,
20u, 21u, 0x0, 0x0, 25u, 26u, 0x0, 0x0, 30u, 31u, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0};
OWN_ALIGNED_64_ARRAY(static uint16_t permutex_idx_table_13u_1[32]) = {
    1u, 2u, 0x0, 0x0, 6u, 7u, 8u, 0x0, 11u, 12u, 13u, 0x0, 0x0, 17u, 18u, 0x0,
    0x0, 22u, 23u, 24u, 0x0, 27u, 28u, 29u, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0};
OWN_ALIGNED_64_ARRAY(static uint16_t permutex_idx_table_13u_2[32]) = {
    0x0, 1u, 2u, 3u, 4u, 6u, 7u, 8u, 9u, 11u, 12u, 13u, 14u, 0x0, 17u, 18u,
    19u, 20u, 22u, 23u, 24u, 25u, 27u, 28u, 29u, 30u, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0};
static __mmask32 permutex_masks_13u_ptr[3] = {0x0333B99D, 0x00EE6773, 0x03FFDFFE};

OWN_ALIGNED_64_ARRAY(static uint16_t shift_mask_table_13u_0[32]) = {
    0u, 0u, 7u, 4u, 1u, 0u, 0u, 5u, 2u, 0u, 0u, 6u, 3u, 0u, 0u, 7u,
    4u, 1u, 0u, 0u, 5u, 2u, 0u, 0u, 6u, 3u, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0};
OWN_ALIGNED_64_ARRAY(static uint16_t shift_mask_table_13u_1[32]) = {
    13u, 10u, 0u, 0u, 14u, 11u, 8u, 0u, 15u, 12u, 9u, 0u, 0u, 13u, 10u, 0u,
    0u, 14u, 11u, 8u, 0u, 15u, 12u, 9u, 0u, 0u, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0};
OWN_ALIGNED_64_ARRAY(static uint16_t shift_mask_table_13u_2[32]) = {
    0u, 3u, 6u, 9u, 12u, 2u, 5u, 8u, 11u, 1u, 4u, 7u, 10u, 0u, 3u, 6u,
    9u, 12u, 2u, 5u, 8u, 11u, 1u, 4u, 7u, 10u, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0};

// ----------------------- 16u14u ------------------------------ //
OWN_ALIGNED_64_ARRAY(static uint16_t permutex_idx_table_14u_0[32]) = {
    1u, 2u, 3u, 4u, 5u, 6u, 7u, 9u, 10u, 11u, 12u, 13u, 14u, 15u, 17u, 18u,
    19u, 20u, 21u, 22u, 23u, 25u, 26u, 27u, 28u, 29u, 30u, 31u, 0x0, 0x0, 0x0, 0x0};
OWN_ALIGNED_64_ARRAY(static uint16_t permutex_idx_table_14u_1[32]) = {
    0u, 1u, 2u, 3u, 4u, 5u, 6u, 8u, 9u, 10u, 11u, 12u, 13u, 14u, 16u, 17u,
    18u, 19u, 20u, 21u, 22u, 24u, 25u, 26u, 27u, 28u, 29u, 30u, 0x0, 0x0, 0x0, 0x0};

OWN_ALIGNED_64_ARRAY(static uint16_t shift_mask_table_14u_0[32]) = {
    14u, 12u, 10u, 8u, 6u, 4u, 2u, 14u, 12u, 10u, 8u, 6u, 4u, 2u, 14u, 12u,
    10u, 8u, 6u, 4u, 2u, 14u, 12u, 10u, 8u, 6u, 4u, 2u, 0x0, 0x0, 0x0, 0x0};
OWN_ALIGNED_64_ARRAY(static uint16_t shift_mask_table_14u_1[32]) = {
    0u, 2u, 4u, 6u, 8u, 10u, 12u, 0u, 2u, 4u, 6u, 8u, 10u, 12u, 0u, 2u,
    4u, 6u, 8u, 10u, 12u, 0u, 2u, 4u, 6u, 8u, 10u, 12, 0x0, 0x0, 0x0, 0x0};

// ----------------------- 16u15u ------------------------------ //
OWN_ALIGNED_64_ARRAY(static uint16_t permutex_idx_table_15u_0[32]) = {
    1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u, 12u, 13u, 14u, 15u, 17u,
    18u, 19u, 20u, 21u, 22u, 23u, 24u, 25u, 26u, 27u, 28u, 29u, 30u, 31u, 0x0, 0x0};
OWN_ALIGNED_64_ARRAY(static uint16_t permutex_idx_table_15u_1[32]) = {
    0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u, 12u, 13u, 14u, 16u,
    17u, 18u, 19u, 20u, 21u, 22u, 23u, 24u, 25u, 26u, 27u, 28u, 29u, 30u, 0x0, 0x0};

OWN_ALIGNED_64_ARRAY(static uint16_t shift_mask_table_15u_0[32]) = {
    15u, 14u, 13u, 12u, 11u, 10u, 9u, 8u, 7u, 6u, 5u, 4u, 3u, 2u, 1u, 15u,
    14u, 13u, 12u, 11u, 10u, 9u, 8u, 7u, 6u, 5u, 4u, 3u, 2u, 1u, 0x0, 0x0};
OWN_ALIGNED_64_ARRAY(static uint16_t shift_mask_table_15u_1[32]) = {
    0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u, 12u, 13u, 14u, 0u,
    1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u, 12u, 13u, 14u, 0x0, 0x0};

 // ********************** 16u32u ****************************** //

OWN_QPLC_INLINE(void, k0_qplc_pack_16u32u_tail, (const uint8_t *src_ptr, uint32_t num_elements, uint8_t *dst_ptr))
{
    __m256i srcmm;
    __m512i dstmm;

    __mmask16 tail_mask = OWN_BIT_MASK(num_elements);
    srcmm = _mm256_maskz_loadu_epi16(tail_mask, (const __m256i *)src_ptr);
    dstmm = _mm512_maskz_cvtepu16_epi32(tail_mask, srcmm);
    _mm512_mask_storeu_epi32(dst_ptr, tail_mask, dstmm);
}

OWN_OPT_FUN(void, k0_qplc_pack_16u32u, (const uint8_t *src_ptr, uint32_t num_elements, uint8_t *dst_ptr)) {
    __m256i srcmm;
    __m512i dstmm;

    while (num_elements > 16u)
    {
        srcmm = _mm256_loadu_si256((const __m256i *)src_ptr);
        dstmm = _mm512_cvtepu16_epi32(srcmm);
        _mm512_storeu_si512(dst_ptr, dstmm);

        num_elements -= 16u;
        src_ptr += 32u;
        dst_ptr += 64u;
    }

    k0_qplc_pack_16u32u_tail(src_ptr, num_elements, dst_ptr);
}

// ********************** 16u9u ****************************** //

OWN_QPLC_INLINE(uint32_t, own_get_align, (uint32_t start_bit, uint32_t base, uint32_t bitsize)) {
    uint32_t remnant = bitsize - start_bit;
    uint32_t ret_value = 0xFFFFFFFF;
    for (uint32_t i = 0u; i < bitsize; ++i) {
        uint32_t test_value = (i * base) % bitsize;
        if (test_value == remnant) {
            ret_value = i;
            break;
        }
    }
    return ret_value;
}

OWN_QPLC_INLINE(void, k0_qplc_pack_16u9u_tail, (const uint8_t *src_ptr, uint32_t num_elements, uint8_t *dst_ptr, uint32_t start_bit))
{
    uint32_t bit_width = 9u;
    int32_t  bits_in_buf = (int32_t)(bit_width + start_bit);
    uint16_t *src_16u_ptr = (uint16_t *)src_ptr;
    uint16_t *dst_16u_ptr = (uint16_t *)dst_ptr;
    uint32_t src = (uint32_t)(*dst_16u_ptr) & OWN_BIT_MASK(start_bit);

    src |= ((uint32_t)(*src_16u_ptr)) << start_bit;
    src_16u_ptr++;
    num_elements--;

    while (0u < num_elements) {
        if (OWN_WORD_WIDTH <= (uint32_t)bits_in_buf) {
            *dst_16u_ptr = (uint16_t)(src);
            dst_16u_ptr++;
            src = src >> OWN_WORD_WIDTH;
            bits_in_buf -= OWN_WORD_WIDTH;
        }
        src = src | (((uint32_t)(*src_16u_ptr)) << bits_in_buf);
        src_16u_ptr++;
        num_elements--;
        bits_in_buf += bit_width;
    }
    dst_ptr = (uint8_t *)dst_16u_ptr;
    while (0 < bits_in_buf) {
        *dst_ptr = (uint8_t)(src);
        bits_in_buf -= OWN_BYTE_WIDTH;
        dst_ptr++;
        src >>= OWN_BYTE_WIDTH;
    }
}

OWN_OPT_FUN(void, k0_qplc_pack_16u9u, (const uint8_t *src_ptr, uint32_t num_elements, uint8_t *dst_ptr, uint32_t start_bit))
{
    if (start_bit > 0u) {
        uint32_t align = own_get_align(start_bit, 9u, 16u);
        if (align > num_elements) {
            align = num_elements;
        }
        k0_qplc_pack_16u9u_tail(src_ptr, align, dst_ptr, start_bit);
        src_ptr += align * 2;
        dst_ptr += ((align * 9u) + start_bit) >> 3u;
        num_elements -= align;
    }

    uint32_t tail = num_elements % 32u;
    if (num_elements >= 32u)
    {
        uint32_t num_elements_96 = num_elements / 96u;
        uint32_t num_elements_32 = (num_elements % 96u) / 32u;
        __m512i srcmm0, srcmm1, srcmm2;
        __m512i zmm0, zmm1, zmm2, zmm3, zmm4, zmm5;

        __m512i permutex_idx_ptr[6];
        permutex_idx_ptr[0] = _mm512_load_si512(permutex_idx_table_9u_0);
        permutex_idx_ptr[1] = _mm512_load_si512(permutex_idx_table_9u_1);
        permutex_idx_ptr[2] = _mm512_load_si512(permutex_idx_table_9u_2);
        permutex_idx_ptr[3] = _mm512_load_si512(permutex_idx_table_9u_3);
        permutex_idx_ptr[4] = _mm512_load_si512(permutex_idx_table_9u_4);
        permutex_idx_ptr[5] = _mm512_load_si512(permutex_idx_table_9u_5);

        __m512i shift_masks_ptr[3];
        shift_masks_ptr[0] = _mm512_load_si512(shift_mask_table_9u_0);
        shift_masks_ptr[1] = _mm512_load_si512(shift_mask_table_9u_1);
        shift_masks_ptr[2] = _mm512_load_si512(shift_mask_table_9u_2);

        for (uint32_t idx = 0; idx < num_elements_96; ++idx)
        {
            srcmm0 = _mm512_loadu_si512(src_ptr);
            srcmm1 = _mm512_loadu_si512((src_ptr + 64u));
            srcmm2 = _mm512_loadu_si512((src_ptr + 128u));

            zmm0 = _mm512_maskz_permutex2var_epi16(permutex_masks_9u_ptr[0], srcmm0, permutex_idx_ptr[0], srcmm1);
            zmm1 = _mm512_maskz_permutex2var_epi16(permutex_masks_9u_ptr[1], srcmm0, permutex_idx_ptr[1], srcmm1);
            zmm2 = _mm512_maskz_permutex2var_epi16(permutex_masks_9u_ptr[2], srcmm0, permutex_idx_ptr[2], srcmm1);
            zmm3 = _mm512_maskz_permutex2var_epi16(permutex_masks_9u_ptr[0], srcmm1, permutex_idx_ptr[3], srcmm2);
            zmm4 = _mm512_maskz_permutex2var_epi16(permutex_masks_9u_ptr[1], srcmm1, permutex_idx_ptr[4], srcmm2);
            zmm5 = _mm512_maskz_permutex2var_epi16(permutex_masks_9u_ptr[2], srcmm1, permutex_idx_ptr[5], srcmm2);

            zmm0 = _mm512_sllv_epi16(zmm0, shift_masks_ptr[0]);
            zmm1 = _mm512_sllv_epi16(zmm1, shift_masks_ptr[1]);
            zmm2 = _mm512_srlv_epi16(zmm2, shift_masks_ptr[2]);
            zmm3 = _mm512_sllv_epi16(zmm3, shift_masks_ptr[0]);
            zmm4 = _mm512_sllv_epi16(zmm4, shift_masks_ptr[1]);
            zmm5 = _mm512_srlv_epi16(zmm5, shift_masks_ptr[2]);

            zmm0 = _mm512_or_si512(zmm0, zmm1);
            zmm0 = _mm512_or_si512(zmm0, zmm2);
            zmm3 = _mm512_or_si512(zmm3, zmm4);
            zmm3 = _mm512_or_si512(zmm3, zmm5);

            _mm512_mask_storeu_epi16(dst_ptr, 0x07FFFFFF, zmm0);
            _mm512_mask_storeu_epi16((dst_ptr + 54u), 0x07FFFFFF, zmm3);

            src_ptr += 192u;
            dst_ptr += 9u * 12u;
        }

        for (uint32_t idx = 0; idx < num_elements_32; ++idx)
        {
            srcmm0 = _mm512_loadu_si512(src_ptr);
            zmm0 = _mm512_maskz_permutexvar_epi16(permutex_masks_9u_ptr[0], permutex_idx_ptr[0], srcmm0);
            zmm1 = _mm512_maskz_permutexvar_epi16(permutex_masks_9u_ptr[1], permutex_idx_ptr[1], srcmm0);
            zmm2 = _mm512_maskz_permutexvar_epi16(permutex_masks_9u_ptr[2], permutex_idx_ptr[2], srcmm0);

            zmm0 = _mm512_sllv_epi16(zmm0, shift_masks_ptr[0]);
            zmm1 = _mm512_sllv_epi16(zmm1, shift_masks_ptr[1]);
            zmm2 = _mm512_srlv_epi16(zmm2, shift_masks_ptr[2]);

            zmm0 = _mm512_or_si512(zmm0, zmm1);
            zmm0 = _mm512_or_si512(zmm0, zmm2);
            _mm512_mask_storeu_epi16(dst_ptr, 0x0003FFFF, zmm0);

            src_ptr += 64u;
            dst_ptr += 9u * 4u;
        }
    }

    if (tail > 0) {
        k0_qplc_pack_16u9u_tail(src_ptr, tail, dst_ptr, 0u);
    }
}

OWN_QPLC_INLINE(void, k0_qplc_pack_16u10u_tail, (const uint8_t *src_ptr, uint32_t num_elements, uint8_t *dst_ptr, uint32_t start_bit))
{
    uint32_t bit_width = 10u;
    int32_t  bits_in_buf = (int32_t)(bit_width + start_bit);
    uint16_t *src_16u_ptr = (uint16_t *)src_ptr;
    uint16_t *dst_16u_ptr = (uint16_t *)dst_ptr;
    uint32_t src = (uint32_t)(*dst_16u_ptr) & OWN_BIT_MASK(start_bit);

    src |= ((uint32_t)(*src_16u_ptr)) << start_bit;
    src_16u_ptr++;
    num_elements--;

    while (0u < num_elements) {
        if (OWN_WORD_WIDTH <= (uint32_t)bits_in_buf) {
            *dst_16u_ptr = (uint16_t)(src);
            dst_16u_ptr++;
            src = src >> OWN_WORD_WIDTH;
            bits_in_buf -= OWN_WORD_WIDTH;
        }
        src = src | (((uint32_t)(*src_16u_ptr)) << bits_in_buf);
        src_16u_ptr++;
        num_elements--;
        bits_in_buf += bit_width;
    }
    dst_ptr = (uint8_t *)dst_16u_ptr;
    while (0 < bits_in_buf) {
        *dst_ptr = (uint8_t)(src);
        bits_in_buf -= OWN_BYTE_WIDTH;
        dst_ptr++;
        src >>= OWN_BYTE_WIDTH;
    }
}

OWN_OPT_FUN(void, k0_qplc_pack_16u10u, (const uint8_t *src_ptr, uint32_t num_elements, uint8_t *dst_ptr, uint32_t start_bit))
{
    if (start_bit > 0u) {
        uint32_t align = own_get_align(start_bit, 10u, 16u);
        if (align > num_elements) {
            align = num_elements;
        }
        k0_qplc_pack_16u10u_tail(src_ptr, align, dst_ptr, start_bit);
        src_ptr += align * 2;
        dst_ptr += ((align * 10u) + start_bit) >> 3u;
        num_elements -= align;
    }

    uint32_t tail = num_elements % 32u;
    if (num_elements >= 32u)
    {
        uint32_t num_elements_96 = num_elements / 96u;
        uint32_t num_elements_32 = (num_elements % 96u) / 32u;
        __m512i srcmm0, srcmm1, srcmm2;
        __m512i zmm0, zmm1, zmm2, zmm3, zmm4, zmm5;

        __m512i permutex_idx_ptr[6];
        permutex_idx_ptr[0] = _mm512_load_si512(permutex_idx_table_10u_0);
        permutex_idx_ptr[1] = _mm512_load_si512(permutex_idx_table_10u_1);
        permutex_idx_ptr[2] = _mm512_load_si512(permutex_idx_table_10u_2);
        permutex_idx_ptr[3] = _mm512_load_si512(permutex_idx_table_10u_3);
        permutex_idx_ptr[4] = _mm512_load_si512(permutex_idx_table_10u_4);
        permutex_idx_ptr[5] = _mm512_load_si512(permutex_idx_table_10u_5);

        __m512i shift_masks_ptr[3];
        shift_masks_ptr[0] = _mm512_load_si512(shift_mask_table_10u_0);
        shift_masks_ptr[1] = _mm512_load_si512(shift_mask_table_10u_1);
        shift_masks_ptr[2] = _mm512_load_si512(shift_mask_table_10u_2);

        for (uint32_t idx = 0; idx < num_elements_96; ++idx)
        {
            srcmm0 = _mm512_loadu_si512(src_ptr);
            srcmm1 = _mm512_loadu_si512((src_ptr + 64u));
            srcmm2 = _mm512_loadu_si512((src_ptr + 128u));

            zmm0 = _mm512_maskz_permutex2var_epi16(permutex_masks_10u_ptr[0], srcmm0, permutex_idx_ptr[0], srcmm1);
            zmm1 = _mm512_maskz_permutex2var_epi16(permutex_masks_10u_ptr[1], srcmm0, permutex_idx_ptr[1], srcmm1);
            zmm2 = _mm512_maskz_permutex2var_epi16(permutex_masks_10u_ptr[2], srcmm0, permutex_idx_ptr[2], srcmm1);
            zmm3 = _mm512_maskz_permutex2var_epi16(permutex_masks_10u_ptr[0], srcmm1, permutex_idx_ptr[3], srcmm2);
            zmm4 = _mm512_maskz_permutex2var_epi16(permutex_masks_10u_ptr[1], srcmm1, permutex_idx_ptr[4], srcmm2);
            zmm5 = _mm512_maskz_permutex2var_epi16(permutex_masks_10u_ptr[2], srcmm1, permutex_idx_ptr[5], srcmm2);

            zmm0 = _mm512_sllv_epi16(zmm0, shift_masks_ptr[0]);
            zmm1 = _mm512_sllv_epi16(zmm1, shift_masks_ptr[1]);
            zmm2 = _mm512_srlv_epi16(zmm2, shift_masks_ptr[2]);
            zmm3 = _mm512_sllv_epi16(zmm3, shift_masks_ptr[0]);
            zmm4 = _mm512_sllv_epi16(zmm4, shift_masks_ptr[1]);
            zmm5 = _mm512_srlv_epi16(zmm5, shift_masks_ptr[2]);

            zmm0 = _mm512_or_si512(zmm0, zmm1);
            zmm0 = _mm512_or_si512(zmm0, zmm2);
            zmm3 = _mm512_or_si512(zmm3, zmm4);
            zmm3 = _mm512_or_si512(zmm3, zmm5);

            _mm512_mask_storeu_epi16(dst_ptr, 0x3FFFFFFF, zmm0);
            _mm512_mask_storeu_epi16((dst_ptr + 60u), 0x3FFFFFFF, zmm3);

            src_ptr += 192u;
            dst_ptr += 10u * 12u;
        }

        for (uint32_t idx = 0; idx < num_elements_32; ++idx)
        {
            srcmm0 = _mm512_loadu_si512(src_ptr);
            zmm0 = _mm512_maskz_permutexvar_epi16(permutex_masks_10u_ptr[0], permutex_idx_ptr[0], srcmm0);
            zmm1 = _mm512_maskz_permutexvar_epi16(permutex_masks_10u_ptr[1], permutex_idx_ptr[1], srcmm0);
            zmm2 = _mm512_maskz_permutexvar_epi16(permutex_masks_10u_ptr[2], permutex_idx_ptr[2], srcmm0);

            zmm0 = _mm512_sllv_epi16(zmm0, shift_masks_ptr[0]);
            zmm1 = _mm512_sllv_epi16(zmm1, shift_masks_ptr[1]);
            zmm2 = _mm512_srlv_epi16(zmm2, shift_masks_ptr[2]);

            zmm0 = _mm512_or_si512(zmm0, zmm1);
            zmm0 = _mm512_or_si512(zmm0, zmm2);
            _mm512_mask_storeu_epi16(dst_ptr, 0x000FFFFF, zmm0);

            src_ptr += 64u;
            dst_ptr += 10u * 4u;
        }
    }

    if (tail > 0) {
        k0_qplc_pack_16u10u_tail(src_ptr, tail, dst_ptr, 0u);
    }
}

OWN_QPLC_INLINE(void, k0_qplc_pack_16u11u_tail, (const uint8_t *src_ptr, uint32_t num_elements, uint8_t *dst_ptr, uint32_t start_bit))
{
    uint32_t bit_width = 11u;
    int32_t  bits_in_buf = (int32_t)(bit_width + start_bit);
    uint16_t *src_16u_ptr = (uint16_t *)src_ptr;
    uint16_t *dst_16u_ptr = (uint16_t *)dst_ptr;
    uint32_t src = (uint32_t)(*dst_16u_ptr) & OWN_BIT_MASK(start_bit);

    src |= ((uint32_t)(*src_16u_ptr)) << start_bit;
    src_16u_ptr++;
    num_elements--;

    while (0u < num_elements) {
        if (OWN_WORD_WIDTH <= (uint32_t)bits_in_buf) {
            *dst_16u_ptr = (uint16_t)(src);
            dst_16u_ptr++;
            src = src >> OWN_WORD_WIDTH;
            bits_in_buf -= OWN_WORD_WIDTH;
        }
        src = src | (((uint32_t)(*src_16u_ptr)) << bits_in_buf);
        src_16u_ptr++;
        num_elements--;
        bits_in_buf += bit_width;
    }
    dst_ptr = (uint8_t *)dst_16u_ptr;
    while (0 < bits_in_buf) {
        *dst_ptr = (uint8_t)(src);
        bits_in_buf -= OWN_BYTE_WIDTH;
        dst_ptr++;
        src >>= OWN_BYTE_WIDTH;
    }
}

OWN_OPT_FUN(void, k0_qplc_pack_16u11u, (const uint8_t *src_ptr, uint32_t num_elements, uint8_t *dst_ptr, uint32_t start_bit))
{
    if (start_bit > 0u) {
        uint32_t align = own_get_align(start_bit, 11u, 16u);
        if (align > num_elements) {
            align = num_elements;
        }
        k0_qplc_pack_16u11u_tail(src_ptr, align, dst_ptr, start_bit);
        src_ptr += align * 2;
        dst_ptr += ((align * 11u) + start_bit) >> 3u;
        num_elements -= align;
    }

    uint32_t tail = num_elements % 32u;
    if (num_elements >= 32u)
    {
        uint32_t num_elements_160 = num_elements / 160u;
        uint32_t num_elements_32 = (num_elements % 160u) / 32u;
        __m512i srcmm0, srcmm1, srcmm2, srcmm3, srcmm4;
        __m512i zmm0, zmm1, zmm2, zmm3, zmm4, zmm5, zmm6, zmm7, zmm8, zmm9, zmm10, zmm11;

        __m512i permutex_idx_ptr[12];
        permutex_idx_ptr[0]  = _mm512_loadu_si512(permutex_idx_table_11u_0);
        permutex_idx_ptr[1]  = _mm512_loadu_si512(permutex_idx_table_11u_1);
        permutex_idx_ptr[2]  = _mm512_loadu_si512(permutex_idx_table_11u_2);
        permutex_idx_ptr[3]  = _mm512_loadu_si512(permutex_idx_table_11u_3);
        permutex_idx_ptr[4]  = _mm512_loadu_si512(permutex_idx_table_11u_4);
        permutex_idx_ptr[5]  = _mm512_loadu_si512(permutex_idx_table_11u_5);
        permutex_idx_ptr[6]  = _mm512_loadu_si512(permutex_idx_table_11u_6);
        permutex_idx_ptr[7]  = _mm512_loadu_si512(permutex_idx_table_11u_7);
        permutex_idx_ptr[8]  = _mm512_loadu_si512(permutex_idx_table_11u_8);
        permutex_idx_ptr[9]  = _mm512_loadu_si512(permutex_idx_table_11u_9);
        permutex_idx_ptr[10] = _mm512_loadu_si512(permutex_idx_table_11u_10);
        permutex_idx_ptr[11] = _mm512_loadu_si512(permutex_idx_table_11u_11);

        __m512i shift_masks_ptr[6];
        shift_masks_ptr[0] = _mm512_loadu_si512(shift_mask_table_11u_0);
        shift_masks_ptr[1] = _mm512_loadu_si512(shift_mask_table_11u_1);
        shift_masks_ptr[2] = _mm512_loadu_si512(shift_mask_table_11u_2);
        shift_masks_ptr[3] = _mm512_loadu_si512(shift_mask_table_11u_3);
        shift_masks_ptr[4] = _mm512_loadu_si512(shift_mask_table_11u_4);
        shift_masks_ptr[5] = _mm512_loadu_si512(shift_mask_table_11u_5);

        for (uint32_t idx = 0; idx < num_elements_160; ++idx)
        {
            srcmm0 = _mm512_loadu_si512(src_ptr);
            srcmm1 = _mm512_loadu_si512((src_ptr + 64u));
            srcmm2 = _mm512_loadu_si512((src_ptr + 128u));
            srcmm3 = _mm512_loadu_si512((src_ptr + 192u));
            srcmm4 = _mm512_loadu_si512((src_ptr + 256u));

            zmm0 = _mm512_maskz_permutex2var_epi16(permutex_masks_11u_ptr[0], srcmm0, permutex_idx_ptr[0], srcmm1);
            zmm1 = _mm512_maskz_permutex2var_epi16(permutex_masks_11u_ptr[1], srcmm0, permutex_idx_ptr[1], srcmm1);
            zmm2 = _mm512_maskz_permutex2var_epi16(permutex_masks_11u_ptr[2], srcmm0, permutex_idx_ptr[2], srcmm1);
            zmm3 = _mm512_maskz_permutex2var_epi16(permutex_masks_11u_ptr[3], srcmm1, permutex_idx_ptr[3], srcmm2);
            zmm4 = _mm512_maskz_permutex2var_epi16(permutex_masks_11u_ptr[4], srcmm1, permutex_idx_ptr[4], srcmm2);
            zmm5 = _mm512_maskz_permutex2var_epi16(permutex_masks_11u_ptr[5], srcmm1, permutex_idx_ptr[5], srcmm2);
            zmm6 = _mm512_maskz_permutex2var_epi16(permutex_masks_11u_ptr[0], srcmm2, permutex_idx_ptr[6], srcmm3);
            zmm7 = _mm512_maskz_permutex2var_epi16(permutex_masks_11u_ptr[1], srcmm2, permutex_idx_ptr[7], srcmm3);
            zmm8 = _mm512_maskz_permutex2var_epi16(permutex_masks_11u_ptr[2], srcmm2, permutex_idx_ptr[8], srcmm3);
            zmm9 = _mm512_maskz_permutex2var_epi16(permutex_masks_11u_ptr[3], srcmm3, permutex_idx_ptr[9], srcmm4);
            zmm10 = _mm512_maskz_permutex2var_epi16(permutex_masks_11u_ptr[4], srcmm3, permutex_idx_ptr[10], srcmm4);
            zmm11 = _mm512_maskz_permutex2var_epi16(permutex_masks_11u_ptr[5], srcmm3, permutex_idx_ptr[11], srcmm4);

            zmm0 = _mm512_sllv_epi16(zmm0, shift_masks_ptr[0]);
            zmm1 = _mm512_sllv_epi16(zmm1, shift_masks_ptr[1]);
            zmm2 = _mm512_srlv_epi16(zmm2, shift_masks_ptr[2]);
            zmm3 = _mm512_sllv_epi16(zmm3, shift_masks_ptr[3]);
            zmm4 = _mm512_sllv_epi16(zmm4, shift_masks_ptr[4]);
            zmm5 = _mm512_srlv_epi16(zmm5, shift_masks_ptr[5]);
            zmm6 = _mm512_sllv_epi16(zmm6, shift_masks_ptr[0]);
            zmm7 = _mm512_sllv_epi16(zmm7, shift_masks_ptr[1]);
            zmm8 = _mm512_srlv_epi16(zmm8, shift_masks_ptr[2]);
            zmm9 = _mm512_sllv_epi16(zmm9, shift_masks_ptr[3]);
            zmm10 = _mm512_sllv_epi16(zmm10, shift_masks_ptr[4]);
            zmm11 = _mm512_srlv_epi16(zmm11, shift_masks_ptr[5]);

            zmm0 = _mm512_or_si512(zmm0, zmm1);
            zmm0 = _mm512_or_si512(zmm0, zmm2);
            zmm3 = _mm512_or_si512(zmm3, zmm4);
            zmm3 = _mm512_or_si512(zmm3, zmm5);
            zmm6 = _mm512_or_si512(zmm6, zmm7);
            zmm6 = _mm512_or_si512(zmm6, zmm8);
            zmm9 = _mm512_or_si512(zmm9, zmm10);
            zmm9 = _mm512_or_si512(zmm9, zmm11);

            _mm512_mask_storeu_epi16(dst_ptr, 0x0FFFFFFF, zmm0);
            _mm512_mask_storeu_epi16((dst_ptr + 56u), 0x07FFFFFF, zmm3);
            _mm512_mask_storeu_epi16((dst_ptr + 110u), 0x0FFFFFFF, zmm6);
            _mm512_mask_storeu_epi16((dst_ptr + 166u), 0x07FFFFFF, zmm9);

            src_ptr += 320u;
            dst_ptr += 11u * 20u;
        }

        for (uint32_t idx = 0; idx < num_elements_32; ++idx)
        {
            srcmm0 = _mm512_loadu_si512(src_ptr);
            zmm0 = _mm512_maskz_permutexvar_epi16(permutex_masks_11u_ptr[0], permutex_idx_ptr[0], srcmm0);
            zmm1 = _mm512_maskz_permutexvar_epi16(permutex_masks_11u_ptr[1], permutex_idx_ptr[1], srcmm0);
            zmm2 = _mm512_maskz_permutexvar_epi16(permutex_masks_11u_ptr[2], permutex_idx_ptr[2], srcmm0);

            zmm0 = _mm512_sllv_epi16(zmm0, shift_masks_ptr[0]);
            zmm1 = _mm512_sllv_epi16(zmm1, shift_masks_ptr[1]);
            zmm2 = _mm512_srlv_epi16(zmm2, shift_masks_ptr[2]);

            zmm0 = _mm512_or_si512(zmm0, zmm1);
            zmm0 = _mm512_or_si512(zmm0, zmm2);
            _mm512_mask_storeu_epi16(dst_ptr, 0x003FFFFF, zmm0);

            src_ptr += 64u;
            dst_ptr += 11u * 4u;
        }
    }

    if (tail > 0) {
        k0_qplc_pack_16u11u_tail(src_ptr, tail, dst_ptr, 0u);
    }
}


OWN_QPLC_INLINE(void, k0_qplc_pack_16u12u_tail, (const uint8_t *src_ptr, uint32_t num_elements, uint8_t *dst_ptr, uint32_t start_bit))
{
    uint32_t bit_width = 12u;
    int32_t  bits_in_buf = (int32_t)(bit_width + start_bit);
    uint16_t *src_16u_ptr = (uint16_t *)src_ptr;
    uint16_t *dst_16u_ptr = (uint16_t *)dst_ptr;
    uint32_t src = (uint32_t)(*dst_16u_ptr) & OWN_BIT_MASK(start_bit);

    src |= ((uint32_t)(*src_16u_ptr)) << start_bit;
    src_16u_ptr++;
    num_elements--;

    while (0u < num_elements) {
        if (OWN_WORD_WIDTH <= (uint32_t)bits_in_buf) {
            *dst_16u_ptr = (uint16_t)(src);
            dst_16u_ptr++;
            src = src >> OWN_WORD_WIDTH;
            bits_in_buf -= OWN_WORD_WIDTH;
        }
        src = src | (((uint32_t)(*src_16u_ptr)) << bits_in_buf);
        src_16u_ptr++;
        num_elements--;
        bits_in_buf += bit_width;
    }
    dst_ptr = (uint8_t *)dst_16u_ptr;
    while (0 < bits_in_buf) {
        *dst_ptr = (uint8_t)(src);
        bits_in_buf -= OWN_BYTE_WIDTH;
        dst_ptr++;
        src >>= OWN_BYTE_WIDTH;
    }
}

OWN_OPT_FUN(void, k0_qplc_pack_16u12u, (const uint8_t *src_ptr, uint32_t num_elements, uint8_t *dst_ptr, uint32_t start_bit))
{
    if (start_bit > 0u) {
        uint32_t align = own_get_align(start_bit, 12u, 16u);
        if (align > num_elements) {
            align = num_elements;
        }
        k0_qplc_pack_16u12u_tail(src_ptr, align, dst_ptr, start_bit);
        src_ptr += align * 2;
        dst_ptr += ((align * 12u) + start_bit) >> 3u;
        num_elements -= align;
    }

    uint32_t tail = num_elements % 32u;
    if (num_elements >= 32u)
    {
        uint32_t num_elements_128 = num_elements / 128u;
        uint32_t num_elements_32 = (num_elements % 128u) / 32u;
        __m512i srcmm0, srcmm1, srcmm2, srcmm3;
        __m512i zmm0, zmm1, zmm2, zmm3, zmm4, zmm5;

        __m512i permutex_idx_ptr[6];
        permutex_idx_ptr[0] = _mm512_load_si512(permutex_idx_table_12u_0);
        permutex_idx_ptr[1] = _mm512_load_si512(permutex_idx_table_12u_1);
        permutex_idx_ptr[2] = _mm512_load_si512(permutex_idx_table_12u_2);
        permutex_idx_ptr[3] = _mm512_load_si512(permutex_idx_table_12u_3);
        permutex_idx_ptr[4] = _mm512_load_si512(permutex_idx_table_12u_4);
        permutex_idx_ptr[5] = _mm512_load_si512(permutex_idx_table_12u_5);

        __m512i shift_masks_ptr[6];
        shift_masks_ptr[0] = _mm512_load_si512(shift_mask_table_12u_0);
        shift_masks_ptr[1] = _mm512_load_si512(shift_mask_table_12u_1);
        shift_masks_ptr[2] = _mm512_load_si512(shift_mask_table_12u_2);
        shift_masks_ptr[3] = _mm512_load_si512(shift_mask_table_12u_3);
        shift_masks_ptr[4] = _mm512_load_si512(shift_mask_table_12u_4);
        shift_masks_ptr[5] = _mm512_load_si512(shift_mask_table_12u_5);

        for (uint32_t idx = 0; idx < num_elements_128; ++idx)
        {
            srcmm0 = _mm512_loadu_si512(src_ptr);
            srcmm1 = _mm512_loadu_si512((src_ptr + 64u));
            srcmm2 = _mm512_loadu_si512((src_ptr + 128u));
            srcmm3 = _mm512_loadu_si512((src_ptr + 192u));

            zmm0 = _mm512_permutex2var_epi16(srcmm0, permutex_idx_ptr[0], srcmm1);
            zmm1 = _mm512_permutex2var_epi16(srcmm0, permutex_idx_ptr[1], srcmm1);
            zmm2 = _mm512_permutex2var_epi16(srcmm1, permutex_idx_ptr[2], srcmm2);
            zmm3 = _mm512_permutex2var_epi16(srcmm1, permutex_idx_ptr[3], srcmm2);
            zmm4 = _mm512_permutex2var_epi16(srcmm2, permutex_idx_ptr[4], srcmm3);
            zmm5 = _mm512_permutex2var_epi16(srcmm2, permutex_idx_ptr[5], srcmm3);

            zmm0 = _mm512_sllv_epi16(zmm0, shift_masks_ptr[0]);
            zmm1 = _mm512_srlv_epi16(zmm1, shift_masks_ptr[1]);
            zmm2 = _mm512_sllv_epi16(zmm2, shift_masks_ptr[2]);
            zmm3 = _mm512_srlv_epi16(zmm3, shift_masks_ptr[3]);
            zmm4 = _mm512_sllv_epi16(zmm4, shift_masks_ptr[4]);
            zmm5 = _mm512_srlv_epi16(zmm5, shift_masks_ptr[5]);

            zmm0 = _mm512_or_si512(zmm0, zmm1);
            zmm2 = _mm512_or_si512(zmm2, zmm3);
            zmm4 = _mm512_or_si512(zmm4, zmm5);

            _mm512_storeu_si512(dst_ptr, zmm0);
            _mm512_storeu_si512((dst_ptr + 64u), zmm2);
            _mm512_storeu_si512((dst_ptr + 128u), zmm4);

            src_ptr += 256;
            dst_ptr += 12u * 16u;
        }

        for (uint32_t idx = 0; idx < num_elements_32; ++idx)
        {
            srcmm0 = _mm512_loadu_si512(src_ptr);
            zmm0 = _mm512_permutexvar_epi16(permutex_idx_ptr[0], srcmm0);
            zmm1 = _mm512_permutexvar_epi16(permutex_idx_ptr[1], srcmm0);

            zmm0 = _mm512_sllv_epi16(zmm0, shift_masks_ptr[0]);
            zmm1 = _mm512_srlv_epi16(zmm1, shift_masks_ptr[1]);

            zmm0 = _mm512_or_si512(zmm0, zmm1);
            _mm512_mask_storeu_epi16(dst_ptr, 0x00FFFFFF, zmm0);

            src_ptr += 64u;
            dst_ptr += 12u * 4u;
        }
    }

    if (tail > 0) {
        k0_qplc_pack_16u12u_tail(src_ptr, tail, dst_ptr, 0u);
    }
}

OWN_QPLC_INLINE(void, k0_qplc_pack_16u13u_tail, (const uint8_t *src_ptr, uint32_t num_elements, uint8_t *dst_ptr, uint32_t start_bit))
{
    uint32_t bit_width = 13u;
    int32_t  bits_in_buf = (int32_t)(bit_width + start_bit);
    uint16_t *src_16u_ptr = (uint16_t *)src_ptr;
    uint16_t *dst_16u_ptr = (uint16_t *)dst_ptr;
    uint32_t src = (uint32_t)(*dst_16u_ptr) & OWN_BIT_MASK(start_bit);

    src |= ((uint32_t)(*src_16u_ptr)) << start_bit;
    src_16u_ptr++;
    num_elements--;

    while (0u < num_elements) {
        if (OWN_WORD_WIDTH <= (uint32_t)bits_in_buf) {
            *dst_16u_ptr = (uint16_t)(src);
            dst_16u_ptr++;
            src = src >> OWN_WORD_WIDTH;
            bits_in_buf -= OWN_WORD_WIDTH;
        }
        src = src | (((uint32_t)(*src_16u_ptr)) << bits_in_buf);
        src_16u_ptr++;
        num_elements--;
        bits_in_buf += bit_width;
    }
    dst_ptr = (uint8_t *)dst_16u_ptr;
    while (0 < bits_in_buf) {
        *dst_ptr = (uint8_t)(src);
        bits_in_buf -= OWN_BYTE_WIDTH;
        dst_ptr++;
        src >>= OWN_BYTE_WIDTH;
    }
}

OWN_OPT_FUN(void, k0_qplc_pack_16u13u, (const uint8_t *src_ptr, uint32_t num_elements, uint8_t *dst_ptr, uint32_t start_bit))
{
    if (start_bit > 0u) {
        uint32_t align = own_get_align(start_bit, 13u, 16u);
        if (align > num_elements) {
            align = num_elements;
        }
        k0_qplc_pack_16u13u_tail(src_ptr, align, dst_ptr, start_bit);
        src_ptr += align * 2;
        dst_ptr += ((align * 13u) + start_bit) >> 3u;
        num_elements -= align;
    }

    uint32_t tail = num_elements % 32u;
    if (num_elements >= 32u)
    {
        uint32_t num_elements_32 = num_elements / 32u;
        __m512i srcmm0;
        __m512i zmm0, zmm1, zmm2;

        __m512i permutex_idx_ptr[3];
        permutex_idx_ptr[0] = _mm512_load_si512(permutex_idx_table_13u_0);
        permutex_idx_ptr[1] = _mm512_load_si512(permutex_idx_table_13u_1);
        permutex_idx_ptr[2] = _mm512_load_si512(permutex_idx_table_13u_2);

        __m512i shift_masks_ptr[3];
        shift_masks_ptr[0] = _mm512_load_si512(shift_mask_table_13u_0);
        shift_masks_ptr[1] = _mm512_load_si512(shift_mask_table_13u_1);
        shift_masks_ptr[2] = _mm512_load_si512(shift_mask_table_13u_2);

        for (uint32_t idx = 0; idx < num_elements_32; ++idx)
        {
            srcmm0 = _mm512_loadu_si512(src_ptr);
            zmm0 = _mm512_maskz_permutexvar_epi16(permutex_masks_13u_ptr[0], permutex_idx_ptr[0], srcmm0);
            zmm1 = _mm512_maskz_permutexvar_epi16(permutex_masks_13u_ptr[1], permutex_idx_ptr[1], srcmm0);
            zmm2 = _mm512_maskz_permutexvar_epi16(permutex_masks_13u_ptr[2], permutex_idx_ptr[2], srcmm0);

            zmm0 = _mm512_sllv_epi16(zmm0, shift_masks_ptr[0]);
            zmm1 = _mm512_sllv_epi16(zmm1, shift_masks_ptr[1]);
            zmm2 = _mm512_srlv_epi16(zmm2, shift_masks_ptr[2]);

            zmm0 = _mm512_or_si512(zmm0, zmm1);
            zmm0 = _mm512_or_si512(zmm0, zmm2);
            _mm512_mask_storeu_epi16(dst_ptr, 0x03FFFFFF, zmm0);

            src_ptr += 64u;
            dst_ptr += 13u * 4u;
        }
    }

    if (tail > 0) {
        k0_qplc_pack_16u13u_tail(src_ptr, tail, dst_ptr, 0u);
    }
}

OWN_QPLC_INLINE(void, k0_qplc_pack_16u14u_tail, (const uint8_t *src_ptr, uint32_t num_elements, uint8_t *dst_ptr, uint32_t start_bit))
{
    uint32_t bit_width = 14u;
    int32_t  bits_in_buf = (int32_t)(bit_width + start_bit);
    uint16_t *src_16u_ptr = (uint16_t *)src_ptr;
    uint16_t *dst_16u_ptr = (uint16_t *)dst_ptr;
    uint32_t src = (uint32_t)(*dst_16u_ptr) & OWN_BIT_MASK(start_bit);

    src |= ((uint32_t)(*src_16u_ptr)) << start_bit;
    src_16u_ptr++;
    num_elements--;

    while (0u < num_elements) {
        if (OWN_WORD_WIDTH <= (uint32_t)bits_in_buf) {
            *dst_16u_ptr = (uint16_t)(src);
            dst_16u_ptr++;
            src = src >> OWN_WORD_WIDTH;
            bits_in_buf -= OWN_WORD_WIDTH;
        }
        src = src | (((uint32_t)(*src_16u_ptr)) << bits_in_buf);
        src_16u_ptr++;
        num_elements--;
        bits_in_buf += bit_width;
    }
    dst_ptr = (uint8_t *)dst_16u_ptr;
    while (0 < bits_in_buf) {
        *dst_ptr = (uint8_t)(src);
        bits_in_buf -= OWN_BYTE_WIDTH;
        dst_ptr++;
        src >>= OWN_BYTE_WIDTH;
    }
}

OWN_OPT_FUN(void, k0_qplc_pack_16u14u, (const uint8_t *src_ptr, uint32_t num_elements, uint8_t *dst_ptr, uint32_t start_bit))
{
    if (start_bit > 0u) {
        uint32_t align = own_get_align(start_bit, 14u, 16u);
        if (align > num_elements) {
            align = num_elements;
        }
        k0_qplc_pack_16u14u_tail(src_ptr, align, dst_ptr, start_bit);
        src_ptr += align * 2;
        dst_ptr += ((align * 14u) + start_bit) >> 3u;
        num_elements -= align;
    }

    uint32_t tail = num_elements % 32u;
    if (num_elements >= 32u)
    {
        uint32_t num_elements_32 = num_elements / 32u;
        __m512i srcmm0;
        __m512i zmm0, zmm1;

        __m512i permutex_idx_ptr[2];
        permutex_idx_ptr[0] = _mm512_load_si512(permutex_idx_table_14u_0);
        permutex_idx_ptr[1] = _mm512_load_si512(permutex_idx_table_14u_1);

        __m512i shift_masks_ptr[2];
        shift_masks_ptr[0] = _mm512_load_si512(shift_mask_table_14u_0);
        shift_masks_ptr[1] = _mm512_load_si512(shift_mask_table_14u_1);

        for (uint32_t idx = 0; idx < num_elements_32; ++idx)
        {
            srcmm0 = _mm512_loadu_si512(src_ptr);
            zmm0 = _mm512_permutexvar_epi16(permutex_idx_ptr[0], srcmm0);
            zmm1 = _mm512_permutexvar_epi16(permutex_idx_ptr[1], srcmm0);

            zmm0 = _mm512_sllv_epi16(zmm0, shift_masks_ptr[0]);
            zmm1 = _mm512_srlv_epi16(zmm1, shift_masks_ptr[1]);

            zmm0 = _mm512_or_si512(zmm0, zmm1);
            _mm512_mask_storeu_epi16(dst_ptr, 0x0FFFFFFF, zmm0);

            src_ptr += 64u;
            dst_ptr += 14u * 4u;
        }
    }

    if (tail > 0) {
        k0_qplc_pack_16u14u_tail(src_ptr, tail, dst_ptr, 0u);
    }
}

OWN_QPLC_INLINE(void, k0_qplc_pack_16u15u_tail, (const uint8_t *src_ptr, uint32_t num_elements, uint8_t *dst_ptr, uint32_t start_bit))
{
    uint32_t bit_width = 15u;
    int32_t  bits_in_buf = (int32_t)(bit_width + start_bit);
    uint16_t *src_16u_ptr = (uint16_t *)src_ptr;
    uint16_t *dst_16u_ptr = (uint16_t *)dst_ptr;
    uint32_t src = (uint32_t)(*dst_16u_ptr) & OWN_BIT_MASK(start_bit);

    src |= ((uint32_t)(*src_16u_ptr)) << start_bit;
    src_16u_ptr++;
    num_elements--;

    while (0u < num_elements) {
        if (OWN_WORD_WIDTH <= (uint32_t)bits_in_buf) {
            *dst_16u_ptr = (uint16_t)(src);
            dst_16u_ptr++;
            src = src >> OWN_WORD_WIDTH;
            bits_in_buf -= OWN_WORD_WIDTH;
        }
        src = src | (((uint32_t)(*src_16u_ptr)) << bits_in_buf);
        src_16u_ptr++;
        num_elements--;
        bits_in_buf += bit_width;
    }
    dst_ptr = (uint8_t *)dst_16u_ptr;
    while (0 < bits_in_buf) {
        *dst_ptr = (uint8_t)(src);
        bits_in_buf -= OWN_BYTE_WIDTH;
        dst_ptr++;
        src >>= OWN_BYTE_WIDTH;
    }
}

OWN_OPT_FUN(void, k0_qplc_pack_16u15u, (const uint8_t *src_ptr, uint32_t num_elements, uint8_t *dst_ptr, uint32_t start_bit))
{
    if (start_bit > 0u) {
        uint32_t align = own_get_align(start_bit, 15u, 16u);
        if (align > num_elements) {
            align = num_elements;
        }
        k0_qplc_pack_16u15u_tail(src_ptr, align, dst_ptr, start_bit);
        src_ptr += align * 2;
        dst_ptr += ((align * 15u) + start_bit) >> 3u;
        num_elements -= align;
    }

    uint32_t tail = num_elements % 32u;
    if (num_elements >= 32u)
    {
        uint32_t num_elements_32 = num_elements / 32u;
        __m512i srcmm0;
        __m512i zmm0, zmm1;

        __m512i permutex_idx_ptr[2];
        permutex_idx_ptr[0] = _mm512_load_si512(permutex_idx_table_15u_0);
        permutex_idx_ptr[1] = _mm512_load_si512(permutex_idx_table_15u_1);

        __m512i shift_masks_ptr[2];
        shift_masks_ptr[0] = _mm512_load_si512(shift_mask_table_15u_0);
        shift_masks_ptr[1] = _mm512_load_si512(shift_mask_table_15u_1);

        for (uint32_t idx = 0; idx < num_elements_32; ++idx)
        {
            srcmm0 = _mm512_loadu_si512(src_ptr);
            zmm0 = _mm512_permutexvar_epi16(permutex_idx_ptr[0], srcmm0);
            zmm1 = _mm512_permutexvar_epi16(permutex_idx_ptr[1], srcmm0);

            zmm0 = _mm512_sllv_epi16(zmm0, shift_masks_ptr[0]);
            zmm1 = _mm512_srlv_epi16(zmm1, shift_masks_ptr[1]);

            zmm0 = _mm512_or_si512(zmm0, zmm1);
            _mm512_mask_storeu_epi16(dst_ptr, 0x3FFFFFFF, zmm0);

            src_ptr += 64u;
            dst_ptr += 15u * 4u;
        }
    }

    if (tail > 0) {
        k0_qplc_pack_16u15u_tail(src_ptr, tail, dst_ptr, 0u);
    }
}
#endif // OWN_PACK_8U_H
