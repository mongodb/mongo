/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/*------- qplc_pack_16u_k0.h -------*/

/**
 * @brief Contains implementation of functions for vector packing dword integers to 17...32-bit integers
 * @date 22/03/2021
 *
 * @details Function list:
 *          - @ref qplc_pack_32u17u
 *          - @ref qplc_pack_32u18u
 *          - @ref qplc_pack_32u19u
 *          - @ref qplc_pack_32u20u
 *          - @ref qplc_pack_32u21u
 *          - @ref qplc_pack_32u22u
 *          - @ref qplc_pack_32u23u
 *          - @ref qplc_pack_32u24u
 *          - @ref qplc_pack_32u25u
 *          - @ref qplc_pack_32u26u
 *          - @ref qplc_pack_32u27u
 *          - @ref qplc_pack_32u28u
 *          - @ref qplc_pack_32u29u
 *          - @ref qplc_pack_32u30u
 *          - @ref qplc_pack_32u31u
 *          - @ref qplc_pack_32u32u
 */
#ifndef OWN_PACK_32U_H
#define OWN_PACK_32U_H

#include "own_qplc_defs.h"

// *********************** Masks  ****************************** //

// ----------------------- 32u17u ------------------------------- //
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_17u_0[16]) = {
    0u, 2u, 4u, 6u, 8u, 10u, 12u, 14u, 0x0, 17u, 19u, 21u, 23u, 25u, 27u, 29u};
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_17u_1[16]) = {
    1u, 3u, 5u, 7u, 9u, 11u, 13u, 15u, 16u, 18u, 20u, 22u, 24u, 26u, 28u, 30u};
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_17u_2[16]) = {
    0x0, 1u, 3u, 5u, 7u, 9u, 11u, 13u, 15u, 16u, 18u, 20u, 22u, 24u, 26u, 28u};
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_17u_3[16]) = {
    15u, 16u, 18u, 20u, 22u, 24u, 26u, 28u, 30u, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0};
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_17u_4[16]) = {
    0x0, 17u, 19u, 21u, 23u, 25u, 27u, 29u, 31u, 32u, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0};
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_17u_5[16]) = {
    14u, 0x0, 17u, 19u, 21u, 23u, 25u, 27u, 29u, 31u, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0};
static __mmask16 permutex_masks_17u_ptr[6] = {0xFEFF, 0xFFFF, 0xFFFE, 0x01FF, 0x03FE, 0x03FD};

OWN_ALIGNED_64_ARRAY(static uint32_t shift_mask_table_17u_0[16]) = {
    0u, 2u, 4u, 6u, 8u, 10u, 12u, 14u, 0u, 1u, 3u, 5u, 7u, 9u, 11u, 13u};
OWN_ALIGNED_64_ARRAY(static uint32_t shift_mask_table_17u_1[16]) = {
    17u, 19u, 21u, 23u, 25u, 27u, 29u, 31u, 16u, 18u, 20u, 22u, 24u, 26u, 28u, 30u};
OWN_ALIGNED_64_ARRAY(static uint32_t shift_mask_table_17u_2[16]) = {
    0u, 15u, 13u, 11u, 9u, 7u, 5u, 3u, 1u, 16u, 14u, 12u, 10u, 8u, 6u, 4u};
OWN_ALIGNED_64_ARRAY(static uint32_t shift_mask_table_17u_3[16]) = {
    15u, 0u, 2u, 4u, 6u, 8u, 10u, 12u, 14u, 0u, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0};
OWN_ALIGNED_64_ARRAY(static uint32_t shift_mask_table_17u_4[16]) = {
    0u, 17u, 19u, 21u, 23u, 25u, 27u, 29u, 31u, 16u, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0};
OWN_ALIGNED_64_ARRAY(static uint32_t shift_mask_table_17u_5[16]) = {
    2u, 0u, 15u, 13u, 11u, 9u, 7u, 5u, 3u, 1u, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0};

// ----------------------- 32u18u ------------------------------- //
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_18u_0[16]) = {
    0u, 2u, 4u, 6u, 0x0, 9u, 11u, 13u, 15u, 16u, 18u, 20u, 22u, 0x0, 25u, 27u};
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_18u_1[16]) = {
    1u, 3u, 5u, 7u, 8u, 10u, 12u, 14u, 0x0, 17u, 19u, 21u, 23u, 24u, 26u, 28u};
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_18u_2[16]) = {
    0x0, 1u, 3u, 5u, 7u, 8u, 10u, 12u, 14u, 0x0, 17u, 19u, 21u, 23u, 24u, 26u};
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_18u_3[16]) = {
    13u, 15u, 16u, 18u, 20u, 22u, 0x0, 25u, 27u, 29u, 31u, 0x0, 0x0, 0x0, 0x0, 0x0};
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_18u_4[16]) = {
    14u, 0x0, 17u, 19u, 21u, 23u, 24u, 26u, 28u, 30u, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0};
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_18u_5[16]) = {
    12u, 14u, 0x0, 17u, 19u, 21u, 23u, 24u, 26u, 28u, 30u, 0x0, 0x0, 0x0, 0x0, 0x0};
static __mmask16 permutex_masks_18u_ptr[6] = {0xDFEF, 0xFEFF, 0xFDFE, 0x07BF, 0x03FD, 0x07FB};

OWN_ALIGNED_64_ARRAY(static uint32_t shift_mask_table_18u_0[16]) = {
    0u, 4u, 8u, 12u, 0u, 2u, 6u, 10u, 14u, 0u, 4u, 8u, 12u, 0u, 2u, 6u};
OWN_ALIGNED_64_ARRAY(static uint32_t shift_mask_table_18u_1[16]) = {
    18u, 22u, 26u, 30u, 16u, 20u, 24u, 28u, 0u, 18u, 22u, 26u, 30u, 16u, 20u, 24u};
OWN_ALIGNED_64_ARRAY(static uint32_t shift_mask_table_18u_2[16]) = {
    0u, 14u, 10u, 6u, 2u, 16u, 12u, 8u, 4u, 0u, 14u, 10u, 6u, 2u, 16u, 12u};
OWN_ALIGNED_64_ARRAY(static uint32_t shift_mask_table_18u_3[16]) = {
    10u, 14u, 0u, 4u, 8u, 12u, 0u, 2u, 6u, 10u, 14u, 0x0, 0x0, 0x0, 0x0, 0x0};
OWN_ALIGNED_64_ARRAY(static uint32_t shift_mask_table_18u_4[16]) = {
    28u, 0u, 18u, 22u, 26u, 30u, 16u, 20u, 24u, 28u, 0u, 0x0, 0x0, 0x0, 0x0, 0x0};
OWN_ALIGNED_64_ARRAY(static uint32_t shift_mask_table_18u_5[16]) = {
    8u, 4u, 0u, 14u, 10u, 6u, 2u, 16u, 12u, 8u, 4u, 0x0, 0x0, 0x0, 0x0, 0x0};

// ----------------------- 32u19u ------------------------------- //
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_19u_0[16]) = {
    0u, 2u, 4u, 0x0, 7u, 9u, 0x0, 12u, 14u, 0x0, 17u, 19u, 21u, 22u, 24u, 26u};
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_19u_1[16]) = {
    1u, 3u, 5u, 6u, 8u, 10u, 11u, 13u, 15u, 16u, 18u, 20u, 0x0, 23u, 25u, 0x0};
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_19u_2[16]) = {
    0x0, 1u, 3u, 5u, 6u, 8u, 10u, 11u, 13u, 15u, 16u, 18u, 20u, 21u, 23u, 25u};
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_19u_3[16]) = {
    11u, 13u, 15u, 16u, 18u, 20u, 0x0, 23u, 25u, 0x0, 28u, 30u, 0x0, 0x0, 0x0, 0x0};
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_19u_4[16]) = {
    12u, 14u, 0x0, 17u, 19u, 21u, 22u, 24u, 26u, 27u, 29u, 31u, 0x0, 0x0, 0x0, 0x0};
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_19u_5[16]) = {
    10u, 12u, 14u, 0x0, 17u, 19u, 21u, 22u, 24u, 26u, 27u, 29u, 31u, 0x0, 0x0, 0x0};
static __mmask16 permutex_masks_19u_ptr[6] = {0xFDB7, 0x6FFF, 0xFFFE, 0x0DBF, 0x0FFB, 0x1FF7};

OWN_ALIGNED_64_ARRAY(static uint32_t shift_mask_table_19u_0[16]) = {
    0u, 6u, 12u, 0u, 5u, 11u, 0u, 4u, 10u, 0u, 3u, 9u, 15u, 2u, 8u, 14u};
OWN_ALIGNED_64_ARRAY(static uint32_t shift_mask_table_19u_1[16]) = {
    19u, 25u, 31u, 18u, 24u, 30u, 17u, 23u, 29u, 16u, 22u, 28u, 0u, 21u, 27u, 0u};
OWN_ALIGNED_64_ARRAY(static uint32_t shift_mask_table_19u_2[16]) = {
    0u, 13u, 7u, 1u, 14u, 8u, 2u, 15u, 9u, 3u, 16u, 10u, 4u, 17u, 11u, 5u};
OWN_ALIGNED_64_ARRAY(static uint32_t shift_mask_table_19u_3[16]) = {
    1u, 7u, 13u, 0u, 6u, 12u, 0u, 5u, 11u, 0u, 4u, 10u, 0u, 0x0, 0x0, 0x0};
OWN_ALIGNED_64_ARRAY(static uint32_t shift_mask_table_19u_4[16]) = {
    20u, 26u, 0u, 19u, 25u, 31u, 18u, 24u, 30u, 17u, 23u, 29u, 0u, 0x0, 0x0, 0x0};
OWN_ALIGNED_64_ARRAY(static uint32_t shift_mask_table_19u_5[16]) = {
    18u, 12u, 6u, 0u, 13u, 7u, 1u, 14u, 8u, 2u, 15u, 9u, 3u, 0x0, 0x0, 0x0};

// ----------------------- 32u20u ------------------------------- //
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_20u_0[16]) = {
    0u, 2u, 0x0, 5u, 7u, 8u, 10u, 0x0, 13u, 15u, 16u, 18u, 0x0, 21u, 23u, 0x0};
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_20u_1[16]) = {
    1u, 3u, 4u, 6u, 0x0, 9u, 11u, 12u, 14u, 0x0, 17u, 19u, 20u, 22u, 0x0, 0x0};
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_20u_2[16]) = {
    0x0, 1u, 3u, 4u, 6u, 0x0, 9u, 11u, 12u, 14u, 0x0, 17u, 19u, 20u, 22u, 0x0};
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_20u_3[16]) = {
    8u, 10u, 0x0, 13u, 15u, 16u, 18u, 0x0, 21u, 23u, 24u, 26u, 0x0, 29u, 31u, 0x0};
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_20u_4[16]) = {
    9u, 11u, 12u, 14u, 0x0, 17u, 19u, 20u, 22u, 0x0, 25u, 27u, 28u, 30u, 0x0, 0x0};
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_20u_5[16]) = {
    0x0, 9u, 11u, 12u, 14u, 0x0, 17u, 19u, 20u, 22u, 0x0, 25u, 27u, 28u, 30u, 0x0};
static __mmask16 permutex_masks_20u_ptr[3] = {0x6F7B, 0x3DEF, 0x7BDE};

OWN_ALIGNED_64_ARRAY(static uint32_t shift_mask_table_20u_0[16]) = {
    0u, 8u, 0u, 4u, 12u, 0u, 8u, 0u, 4u, 12u, 0u, 8u, 0u, 4u, 12u, 0u};
OWN_ALIGNED_64_ARRAY(static uint32_t shift_mask_table_20u_1[16]) = {
    20u, 28u, 16u, 24u, 0u, 20u, 28u, 16u, 24u, 0u, 20u, 28u, 16u, 24u, 0u, 0u};
OWN_ALIGNED_64_ARRAY(static uint32_t shift_mask_table_20u_2[16]) = {
    0u, 12u, 4u, 16u, 8u, 0u, 12u, 4u, 16u, 8u, 0u, 12u, 4u, 16u, 8u, 0u};

// ----------------------- 32u21u ------------------------------- //
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_21u_0[16]) = {
    0u, 2u, 0x0, 5u, 0x0, 8u, 0x0, 11u, 0x0, 14u, 0x0, 17u, 19u, 20u, 22u, 23u};
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_21u_1[16]) = {
    1u, 3u, 4u, 6u, 7u, 9u, 10u, 12u, 13u, 15u, 16u, 18u, 0x0, 21u, 0x0, 24u};
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_21u_2[16]) = {
    0x0, 1u, 3u, 4u, 6u, 7u, 9u, 10u, 12u, 13u, 15u, 16u, 18u, 19u, 21u, 22u};
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_21u_3[16]) = {
    9u, 10u, 12u, 13u, 15u, 16u, 18u, 0x0, 21u, 0x0, 24u, 0x0, 27u, 0x0, 30u, 0x0};
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_21u_4[16]) = {
    0x0, 11u, 0x0, 14u, 0x0, 17u, 19u, 20u, 22u, 23u, 25u, 26u, 28u, 29u, 31u, 0x0};
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_21u_5[16]) = {
    8u, 9u, 11u, 12u, 14u, 0x0, 17u, 19u, 20u, 22u, 23u, 25u, 26u, 28u, 29u, 31u};
static __mmask16 permutex_masks_21u_ptr[6] = {0xFAAB, 0xAFFF, 0xFFFE, 0x557F, 0x7FEA, 0xFFDF};

OWN_ALIGNED_64_ARRAY(static uint32_t shift_mask_table_21u_0[16]) = {
    0u, 10u, 0u, 9u, 0u, 8u, 0u, 7u, 0u, 6u, 0u, 5u, 15u, 4u, 14u, 3u};
OWN_ALIGNED_64_ARRAY(static uint32_t shift_mask_table_21u_1[16]) = {
    21u, 31u, 20u, 30u, 19u, 29u, 18u, 28u, 17u, 27u, 16u, 26u, 0u, 25u, 0u, 24u};
OWN_ALIGNED_64_ARRAY(static uint32_t shift_mask_table_21u_2[16]) = {
    0u, 11u, 1u, 12u, 2u, 13u, 3u, 14u, 4u, 15u, 5u, 16u, 6u, 17u, 7u, 18u};
OWN_ALIGNED_64_ARRAY(static uint32_t shift_mask_table_21u_3[16]) = {
    13u, 2u, 12u, 1u, 11u, 0u, 10u, 0u, 9u, 0u, 8u, 0u, 7u, 0u, 6u, 0u};
OWN_ALIGNED_64_ARRAY(static uint32_t shift_mask_table_21u_4[16]) = {
    0u, 23u, 0u, 22u, 0u, 21u, 31u, 20u, 30u, 19u, 29u, 18u, 28u, 17u, 27u, 0u};
OWN_ALIGNED_64_ARRAY(static uint32_t shift_mask_table_21u_5[16]) = {
    8u, 19u, 9u, 20u, 10u, 0u, 11u, 1u, 12u, 2u, 13u, 3u, 14u, 4u, 15u, 5u};

// ----------------------- 32u22u ------------------------------- //
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_22u_0[16]) = {
    0u, 2u, 3u, 5u, 6u, 0x0, 9u, 0x0, 12u, 0x0, 15u, 16u, 18u, 19u, 21u, 22u};
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_22u_1[16]) = {
    1u, 0x0, 4u, 0x0, 7u, 8u, 10u, 11u, 13u, 14u, 0x0, 17u, 0x0, 20u, 0x0, 23u};
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_22u_2[16]) = {
    0x0, 1u, 2u, 4u, 5u, 7u, 8u, 10u, 11u, 13u, 14u, 0x0, 17u, 18u, 20u, 21u};
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_22u_3[16]) = {
    0x0, 9u, 0x0, 12u, 0x0, 15u, 16u, 18u, 19u, 21u, 22u, 0x0, 25u, 0x0, 28u, 0x0};
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_22u_4[16]) = {
    8u, 10u, 11u, 13u, 14u, 0x0, 17u, 0x0, 20u, 0x0, 23u, 24u, 26u, 27u, 29u, 30u};
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_22u_5[16]) = {
    7u, 8u, 10u, 11u, 13u, 14u, 0x0, 17u, 18u, 20u, 21u, 23u, 24u, 26u, 27u, 29u};
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_22u_6[16]) = {
    15u, 16u, 18u, 19u, 21u, 22u, 0x0, 25u, 0x0, 28u, 0x0, 31u, 0x0, 0x0, 0x0, 0x0};
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_22u_7[16]) = {
    0x0, 17u, 0x0, 20u, 0x0, 23u, 24u, 26u, 27u, 29u, 30u, 0x0, 0x0, 0x0, 0x0, 0x0};
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_22u_8[16]) = {
    14u, 0x0, 17u, 18u, 20u, 21u, 23u, 24u, 26u, 27u, 29u, 30u, 0x0, 0x0, 0x0, 0x0};
static __mmask16 permutex_masks_22u_ptr[9] = {0xFD5F, 0xABF5, 0xF7FE, 0x57EA, 0xFD5F, 0xFFBF, 0x0ABF, 0x07EA, 0x0FFD};

OWN_ALIGNED_64_ARRAY(static uint32_t shift_mask_table_22u_0[16]) = {
    0u, 12u, 2u, 14u, 4u, 0u, 6u, 0u, 8u, 9u, 10u, 0u, 12u, 2u, 14u, 4u};
OWN_ALIGNED_64_ARRAY(static uint32_t shift_mask_table_22u_1[16]) = {
    22u, 0u, 24u, 0u, 26u, 16u, 28u, 18u, 30u, 20u, 0u, 22u, 0u, 24u, 0u, 26u};
OWN_ALIGNED_64_ARRAY(static uint32_t shift_mask_table_22u_2[16]) = {
    0u, 10u, 20u, 8u, 18u, 6u, 16u, 4u, 14u, 2u, 12u, 0u, 10u, 20u, 8u, 18u};
OWN_ALIGNED_64_ARRAY(static uint32_t shift_mask_table_22u_3[16]) = {
    0u, 6u, 0u, 8u, 9u, 10u, 0u, 12u, 2u, 14u, 4u, 0u, 6u, 0u, 8u, 9u};
OWN_ALIGNED_64_ARRAY(static uint32_t shift_mask_table_22u_4[16]) = {
    16u, 28u, 18u, 30u, 20u, 0u, 22u, 0u, 24u, 0u, 26u, 16u, 28u, 18u, 30u, 20u};
OWN_ALIGNED_64_ARRAY(static uint32_t shift_mask_table_22u_5[16]) = {
    6u, 16u, 4u, 14u, 2u, 12u, 0u, 10u, 20u, 8u, 18u, 6u, 16u, 4u, 14u, 2u};
OWN_ALIGNED_64_ARRAY(static uint32_t shift_mask_table_22u_6[16]) = {
    10u, 0u, 12u, 2u, 14u, 4u, 0u, 6u, 0u, 8u, 9u, 10u, 0x0, 0x0, 0x0, 0x0};
OWN_ALIGNED_64_ARRAY(static uint32_t shift_mask_table_22u_7[16]) = {
    0u, 22u, 0u, 24u, 0u, 26u, 16u, 28u, 18u, 30u, 20u, 0u, 0x0, 0x0, 0x0, 0x0};
OWN_ALIGNED_64_ARRAY(static uint32_t shift_mask_table_22u_8[16]) = {
    12u, 0u, 10u, 20u, 8u, 18u, 6u, 16u, 4u, 14u, 2u, 12u, 0x0, 0x0, 0x0, 0x0};

// ----------------------- 32u23u ------------------------------- //
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_23u_0[16]) = {
    0u, 2u, 3u, 0x0, 6u, 7u, 9u, 10u, 0x0, 13u, 14u, 0x0, 17u, 0x0, 20u, 21u};
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_23u_1[16]) = {
    1u, 0x0, 4u, 5u, 0x0, 8u, 0x0, 11u, 12u, 0x0, 15u, 16u, 18u, 19u, 0x0, 22u};
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_23u_2[16]) = {
    0x0, 1u, 2u, 4u, 5u, 6u, 8u, 9u, 11u, 12u, 13u, 15u, 16u, 18u, 19u, 20u};
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_23u_3[16]) = {
    0x0, 8u, 0x0, 11u, 12u, 0x0, 15u, 16u, 18u, 19u, 0x0, 22u, 23u, 25u, 26u, 0x0};
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_23u_4[16]) = {
    7u, 9u, 10u, 0x0, 13u, 14u, 0x0, 17u, 0x0, 20u, 21u, 0x0, 24u, 0x0, 27u, 28u};
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_23u_5[16]) = {
    6u, 7u, 9u, 10u, 11u, 13u, 14u, 0x0, 17u, 18u, 20u, 21u, 22u, 24u, 25u, 27u};
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_23u_6[16]) = {
    13u, 14u, 0x0, 17u, 0x0, 20u, 21u, 0x0, 24u, 0x0, 27u, 28u, 0x0, 31u, 0x0, 0x0};
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_23u_7[16]) = {
    0x0, 15u, 16u, 18u, 19u, 0x0, 22u, 23u, 25u, 26u, 0x0, 29u, 30u, 0x0, 0x0, 0x0};
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_23u_8[16]) = {
    12u, 13u, 15u, 16u, 18u, 19u, 20u, 22u, 23u, 25u, 26u, 27u, 29u, 30u, 0x0, 0x0};
static __mmask16 permutex_masks_23u_ptr[9] = {0xD6F7, 0xBDAD, 0xFFFE, 0x7BDA, 0xD6B7, 0xFF7F, 0x2D6B, 0x1BDE, 0x3FFF};

OWN_ALIGNED_64_ARRAY(static uint32_t shift_mask_table_23u_0[16]) = {
    0u, 14u, 5u, 0u, 10u, 1u, 15u, 6u, 0u, 11u, 2u, 0u, 7u, 0u, 12u, 3u};
OWN_ALIGNED_64_ARRAY(static uint32_t shift_mask_table_23u_1[16]) = {
    23u, 0u, 28u, 19u, 0u, 24u, 0u, 29u, 20u, 0u, 25u, 16u, 30u, 21u, 0u, 26u};
OWN_ALIGNED_64_ARRAY(static uint32_t shift_mask_table_23u_2[16]) = {
    0u, 9u, 18u, 4u, 13u, 22u, 8u, 17u, 3u, 12u, 21u, 7u, 16u, 2u, 11u, 20u};
OWN_ALIGNED_64_ARRAY(static uint32_t shift_mask_table_23u_3[16]) = {
    0u, 8u, 0u, 13u, 4u, 0u, 9u, 0u, 14u, 5u, 0u, 10u, 1u, 15u, 6u, 0u};
OWN_ALIGNED_64_ARRAY(static uint32_t shift_mask_table_23u_4[16]) = {
    17u, 31u, 22u, 0u, 27u, 18u, 0u, 23u, 0u, 28u, 19u, 0u, 24u, 0u, 29u, 20u};
OWN_ALIGNED_64_ARRAY(static uint32_t shift_mask_table_23u_5[16]) = {
    6u, 15u, 1u, 10u, 19u, 5u, 14u, 0u, 9u, 18u, 4u, 13u, 22u, 8u, 17u, 3u};
OWN_ALIGNED_64_ARRAY(static uint32_t shift_mask_table_23u_6[16]) = {
    11u, 2u, 0u, 7u, 0u, 12u, 3u, 0u, 8u, 0u, 13u, 4u, 0u, 9u, 0x0, 0x0};
OWN_ALIGNED_64_ARRAY(static uint32_t shift_mask_table_23u_7[16]) = {
    0u, 25u, 16u, 30u, 21u, 0u, 26u, 17u, 31u, 22u, 0u, 27u, 18u, 0u, 0x0, 0x0};
OWN_ALIGNED_64_ARRAY(static uint32_t shift_mask_table_23u_8[16]) = {
    12u, 21u, 7u, 16u, 2u, 11u, 20u, 6u, 15u, 1u, 10u, 19u, 5u, 14u, 0x0, 0x0};

// ----------------------- 32u24u ------------------------------- //
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_24u_0[16]) = {
    1u, 2u, 3u, 5u, 6u, 7u, 9u, 10u, 11u, 13u, 14u, 15u, 17u, 18u, 19u, 21u};
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_24u_1[16]) = {
    0u, 1u, 2u, 4u, 5u, 6u, 8u, 9u, 10u, 12u, 13u, 14u, 16u, 17u, 18u, 20u};
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_24u_2[16]) = {
    6u, 7u, 9u, 10u, 11u, 13u, 14u, 15u, 17u, 18u, 19u, 21u, 22u, 23u, 25u, 26u};
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_24u_3[16]) = {
    5u, 6u, 8u, 9u, 10u, 12u, 13u, 14u, 16u, 17u, 18u, 20u, 21u, 22u, 24u, 25u};
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_24u_4[16]) = {
    11u, 13u, 14u, 15u, 17u, 18u, 19u, 21u, 22u, 23u, 25u, 26u, 27u, 29u, 30u, 31u};
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_24u_5[16]) = {
    10u, 12u, 13u, 14u, 16u, 17u, 18u, 20u, 21u, 22u, 24u, 25u, 26u, 28u, 29u, 30u};

OWN_ALIGNED_64_ARRAY(static uint32_t shift_mask_table_24u_0[16]) = {
    24u, 16u, 8u, 24u, 16u, 8u, 24u, 16u, 8u, 24u, 16u, 8u, 24u, 16u, 8u, 24u};
OWN_ALIGNED_64_ARRAY(static uint32_t shift_mask_table_24u_1[16]) = {
    0u, 8u, 16u, 0u, 8u, 16u, 0u, 8u, 16u, 0u, 8u, 16u, 0u, 8u, 16u, 0u};
OWN_ALIGNED_64_ARRAY(static uint32_t shift_mask_table_24u_2[16]) = {
    16u, 8u, 24u, 16u, 8u, 24u, 16u, 8u, 24u, 16u, 8u, 24u, 16u, 8u, 24u, 16u};
OWN_ALIGNED_64_ARRAY(static uint32_t shift_mask_table_24u_3[16]) = {
    8u, 16u, 0u, 8u, 16u, 0u, 8u, 16u, 0u, 8u, 16u, 0u, 8u, 16u, 0u, 8u};
OWN_ALIGNED_64_ARRAY(static uint32_t shift_mask_table_24u_4[16]) = {
    8u, 24u, 16u, 8u, 24u, 16u, 8u, 24u, 16u, 8u, 24u, 16u, 8u, 24u, 16u, 8u};
OWN_ALIGNED_64_ARRAY(static uint32_t shift_mask_table_24u_5[16]) = {
    16u, 0u, 8u, 16u, 0u, 8u, 16u, 0u, 8u, 16u, 0u, 8u, 16u, 0u, 8u, 16u};

// ----------------------- 32u25u ------------------------------- //
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_25u_0[16]) = {
    0u, 0x0, 3u, 4u, 0x0, 7u, 8u, 9u, 0x0, 12u, 13u, 0x0, 0x0, 0x0, 0x0, 0x0};
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_25u_1[16]) = {
    1u, 2u, 0x0, 5u, 6u, 0x0, 0x0, 10u, 11u, 0x0, 14u, 15u, 0x0, 0x0, 0x0, 0x0};
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_25u_2[16]) = {
    0x0, 1u, 2u, 3u, 5u, 6u, 7u, 8u, 10u, 11u, 12u, 14u, 15u, 0x0, 0x0, 0x0};
static __mmask16 permutex_masks_25u_ptr[3] = {0x06ED, 0x0D9B, 0x1FFE};

OWN_ALIGNED_64_ARRAY(static uint32_t shift_mask_table_25u_0[16]) = {
    0u, 0u, 11u, 4u, 0u, 15u, 8u, 1u, 0u, 12u, 5u, 0u, 0u, 0u, 0u, 0u};
OWN_ALIGNED_64_ARRAY(static uint32_t shift_mask_table_25u_1[16]) = {
    25u, 18u, 0u, 29u, 22u, 0u, 0u, 26u, 19u, 0u, 30u, 23u, 0u, 0u, 0u, 0u};
OWN_ALIGNED_64_ARRAY(static uint32_t shift_mask_table_25u_2[16]) = {
    0u, 7u, 14u, 21u, 3u, 10u, 17u, 24u, 6u, 13u, 20u, 2u, 9u, 0u, 0u, 0u};

// ----------------------- 32u26u ------------------------------- //
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_26u_0[16]) = {
    0u, 0x0, 3u, 4u, 5u, 0x0, 0x0, 9u, 10u, 0x0, 0x0, 14u, 15u, 0x0, 0x0, 0x0};
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_26u_1[16]) = {
    1u, 2u, 0x0, 0x0, 6u, 7u, 8u, 0x0, 11u, 12u, 13u, 0x0, 0x0, 0x0, 0x0, 0x0};
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_26u_2[16]) = {
    0x0, 1u, 2u, 3u, 4u, 6u, 7u, 8u, 9u, 11u, 12u, 13u, 14u, 0x0, 0x0, 0x0};
static __mmask16 permutex_masks_26u_ptr[3] = {0x199D, 0x0773, 0x1FFE};

OWN_ALIGNED_64_ARRAY(static uint32_t shift_mask_table_26u_0[16]) = {
    0u, 0u, 14u, 8u, 2u, 0u, 0u, 10u, 4u, 0u, 0u, 12u, 6u, 0x0, 0x0, 0x0};
OWN_ALIGNED_64_ARRAY(static uint32_t shift_mask_table_26u_1[16]) = {
    26u, 20u, 0u, 0u, 28u, 22u, 16u, 0u, 30u, 24u, 18u, 0u, 0u, 0x0, 0x0, 0x0};
OWN_ALIGNED_64_ARRAY(static uint32_t shift_mask_table_26u_2[16]) = {
    0u, 6u, 12u, 18u, 24u, 4u, 10u, 16u, 22u, 2u, 8u, 14u, 20u, 0x0, 0x0, 0x0};

// ----------------------- 32u27u ------------------------------- //
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_27u_0[16]) = {
    0u, 0x0, 0x0, 4u, 5u, 6u, 0x0, 0x0, 10u, 11u, 12u, 0x0, 0x0, 0x0, 0x0, 0x0};
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_27u_1[16]) = {
    1u, 2u, 3u, 0x0, 0x0, 7u, 8u, 9u, 0x0, 0x0, 13u, 14u, 15u, 0x0, 0x0, 0x0};
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_27u_2[16]) = {
    0x0, 1u, 2u, 3u, 4u, 5u, 7u, 8u, 9u, 10u, 11u, 13u, 14u, 15u, 0x0, 0x0};
static __mmask16 permutex_masks_27u_ptr[3] = {0x0739, 0x1CE7, 0x3FFE};

OWN_ALIGNED_64_ARRAY(static uint32_t shift_mask_table_27u_0[16]) = {
    0u, 0u, 0u, 12u, 7u, 2u, 0u, 0u, 14u, 9u, 4u, 0u, 0u, 0u, 0x0, 0x0};
OWN_ALIGNED_64_ARRAY(static uint32_t shift_mask_table_27u_1[16]) = {
    27u, 22u, 17u, 0u, 0u, 29u, 24u, 19u, 0u, 0u, 31u, 26u, 21u, 0u, 0x0, 0x0};
OWN_ALIGNED_64_ARRAY(static uint32_t shift_mask_table_27u_2[16]) = {
    0u, 5u, 10u, 15u, 20u, 25u, 3u, 8u, 13u, 18u, 23u, 1u, 6u, 11u, 0x0, 0x0};

// ----------------------- 32u28u ------------------------------- //
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_28u_0[16]) = {
    1u, 2u, 3u, 4u, 5u, 6u, 7u, 9u, 10u, 11u, 12u, 13u, 14u, 15u, 0x0, 0x0};
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_28u_1[16]) = {
    0u, 1u, 2u, 3u, 4u, 5u, 6u, 8u, 9u, 10u, 11u, 12u, 13u, 14u, 0x0, 0x0};
static __mmask16 permutex_masks_28u_ptr[2] = {0x3FFF, 0x3FFF};

OWN_ALIGNED_64_ARRAY(static uint32_t shift_mask_table_28u_0[16]) = {
    28u, 24u, 20u, 16u, 12u, 8u, 4u, 28u, 24u, 20u, 16u, 12u, 8u, 4u, 0x0, 0x0};
OWN_ALIGNED_64_ARRAY(static uint32_t shift_mask_table_28u_1[16]) = {
    0u, 4u, 8u, 12u, 16u, 20u, 24u, 0u, 4u, 8u, 12u, 16u, 20u, 24u, 0x0, 0x0};

// ----------------------- 32u29u ------------------------------- //
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_29u_0[16]) = {
    0u, 0x0, 0x0, 0x0, 0x0, 6u, 7u, 8u, 9u, 10u, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0};
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_29u_1[16]) = {
    1u, 2u, 3u, 4u, 5u, 0x0, 0x0, 0x0, 0x0, 11u, 12u, 13u, 14u, 15u, 0x0, 0x0};
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_29u_2[16]) = {
    0x0, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 11u, 12u, 13u, 14u, 15u, 0x0};
static __mmask16 permutex_masks_29u_ptr[3] = {0x03E1, 0x3E1F, 0x7FFE};

OWN_ALIGNED_64_ARRAY(static uint32_t shift_mask_table_29u_0[16]) = {
    0u, 0u, 0u, 0u, 0u, 14u, 11u, 8u, 5u, 2u, 0u, 0u, 0u, 0u, 0u, 0x0};
OWN_ALIGNED_64_ARRAY(static uint32_t shift_mask_table_29u_1[16]) = {
    29u, 26u, 23u, 20u, 17u, 0u, 0u, 0u, 0u, 31u, 28u, 25u, 22u, 19u, 0u, 0x0};
OWN_ALIGNED_64_ARRAY(static uint32_t shift_mask_table_29u_2[16]) = {
    0u, 3u, 6u, 9u, 12u, 15u, 18u, 21u, 24u, 27u, 1u, 4u, 7u, 10u, 13u, 0x0};

// ----------------------- 32u30u ------------------------------- //
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_30u_0[16]) = {
    1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u, 12u, 13u, 14u, 15u, 0x0};
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_30u_1[16]) = {
    0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u, 12u, 13u, 14u, 0x0};
static __mmask16 permutex_masks_30u_ptr[2] = {0x7FFF, 0x7FFF};

OWN_ALIGNED_64_ARRAY(static uint32_t shift_mask_table_30u_0[16]) = {
    30u, 28u, 26u, 24u, 22u, 20u, 18u, 16u, 14u, 12u, 10u, 8u, 6u, 4u, 2u, 0x0};
OWN_ALIGNED_64_ARRAY(static uint32_t shift_mask_table_30u_1[16]) = {
    0u, 2u, 4u, 6u, 8u, 10u, 12u, 14u, 16u, 18u, 20u, 22u, 24u, 26u, 28u, 0x0};

// ----------------------- 32u31u ------------------------------- //
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_31u_0[16]) = {
    1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u, 12u, 13u, 14u, 15u, 0x0};
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_31u_1[16]) = {
    0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u, 12u, 13u, 14u, 15u};
static __mmask16 permutex_masks_31u_ptr[2] = {0x7FFF, 0xFFFF};

OWN_ALIGNED_64_ARRAY(static uint32_t shift_mask_table_31u_0[16]) = {
    31u, 30u, 29u, 28u, 27u, 26u, 25u, 24u, 23u, 22u, 21u, 20u, 19u, 18u, 17u, 0u};
OWN_ALIGNED_64_ARRAY(static uint32_t shift_mask_table_31u_1[16]) = {
    0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u, 12u, 13u, 14u, 15u};


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

// ********************** 32u17u ****************************** //

OWN_QPLC_INLINE(void, px_qplc_pack_32u17u, (const uint8_t *src_ptr, uint32_t num_elements, uint8_t *dst_ptr, uint32_t start_bit))
{
    uint32_t bit_width = 17u;
    int32_t  bits_in_buf = (int32_t)(bit_width + start_bit);
    uint32_t *src_32u_ptr = (uint32_t *)src_ptr;
    uint32_t *dst_32u_ptr = (uint32_t *)dst_ptr;
    uint64_t src = (uint64_t)(*dst_32u_ptr) & OWN_BIT_MASK(start_bit);

    src |= ((uint64_t)(*src_32u_ptr)) << start_bit;
    src_32u_ptr++;
    num_elements--;

    while (0u < num_elements) {
        if (OWN_DWORD_WIDTH <= (uint32_t)bits_in_buf) {
            *dst_32u_ptr = (uint32_t)(src);
            dst_32u_ptr++;
            src = src >> OWN_DWORD_WIDTH;
            bits_in_buf -= OWN_DWORD_WIDTH;
        }
        src = src | (((uint64_t)(*src_32u_ptr)) << bits_in_buf);
        src_32u_ptr++;
        num_elements--;
        bits_in_buf += (int32_t)bit_width;
    }
    dst_ptr = (uint8_t *)dst_32u_ptr;
    while (0 < bits_in_buf) {
        *dst_ptr = (uint8_t)(src);
        bits_in_buf -= OWN_BYTE_WIDTH;
        dst_ptr++;
        src >>= OWN_BYTE_WIDTH;
    }
}

OWN_QPLC_INLINE(void, k0_qplc_pack_32u17u_tail, (const uint8_t *src_ptr, uint32_t num_elements, uint8_t *dst_ptr))
{
    __m512i srcmm0;
    __m512i zmm0, zmm1, zmm2;

    __mmask16 tail_mask = OWN_BIT_MASK(num_elements);
    __mmask64 store_mask = OWN_BIT_MASK(OWN_BITS_2_BYTE(num_elements * 17u));

    __m512i permutex_idx_ptr[3];
    permutex_idx_ptr[0] = _mm512_load_si512(permutex_idx_table_17u_0);
    permutex_idx_ptr[1] = _mm512_load_si512(permutex_idx_table_17u_1);
    permutex_idx_ptr[2] = _mm512_load_si512(permutex_idx_table_17u_2);

    __m512i shift_masks_ptr[3];
    shift_masks_ptr[0] = _mm512_load_si512(shift_mask_table_17u_0);
    shift_masks_ptr[1] = _mm512_load_si512(shift_mask_table_17u_1);
    shift_masks_ptr[2] = _mm512_load_si512(shift_mask_table_17u_2);

    srcmm0 = _mm512_maskz_loadu_epi32(tail_mask, src_ptr);
    zmm0 = _mm512_maskz_permutexvar_epi32(permutex_masks_17u_ptr[0], permutex_idx_ptr[0], srcmm0);
    zmm1 = _mm512_maskz_permutexvar_epi32(permutex_masks_17u_ptr[1], permutex_idx_ptr[1], srcmm0);
    zmm2 = _mm512_maskz_permutexvar_epi32(permutex_masks_17u_ptr[2], permutex_idx_ptr[2], srcmm0);

    zmm0 = _mm512_sllv_epi32(zmm0, shift_masks_ptr[0]);
    zmm1 = _mm512_sllv_epi32(zmm1, shift_masks_ptr[1]);
    zmm2 = _mm512_srlv_epi32(zmm2, shift_masks_ptr[2]);

    zmm0 = _mm512_or_si512(zmm0, zmm1);
    zmm0 = _mm512_or_si512(zmm0, zmm2);
    _mm512_mask_storeu_epi8(dst_ptr, store_mask, zmm0);
}

OWN_OPT_FUN(void, k0_qplc_pack_32u17u, (const uint8_t *src_ptr, uint32_t num_elements, uint8_t *dst_ptr, uint32_t start_bit))
{
    if (start_bit > 0u) {
        uint32_t align = own_get_align(start_bit, 17u, 32u);
        if (align > num_elements) {
            align = num_elements;
        }
        px_qplc_pack_32u17u(src_ptr, align, dst_ptr, start_bit);
        src_ptr += align * 4;
        dst_ptr += ((align * 17u) + start_bit) >> 3u;
        num_elements -= align;
    }

    uint32_t tail = num_elements % 16u;
    if (num_elements >= 16u)
    {
        uint32_t num_elements_48 = num_elements / 48u;
        uint32_t num_elements_16 = (num_elements % 48u) / 16u;
        __m512i srcmm0, srcmm1, srcmm2;
        __m512i zmm0, zmm1, zmm2, zmm3, zmm4, zmm5;

        __m512i permutex_idx_ptr[6];
        permutex_idx_ptr[0] = _mm512_load_si512(permutex_idx_table_17u_0);
        permutex_idx_ptr[1] = _mm512_load_si512(permutex_idx_table_17u_1);
        permutex_idx_ptr[2] = _mm512_load_si512(permutex_idx_table_17u_2);
        permutex_idx_ptr[3] = _mm512_load_si512(permutex_idx_table_17u_3);
        permutex_idx_ptr[4] = _mm512_load_si512(permutex_idx_table_17u_4);
        permutex_idx_ptr[5] = _mm512_load_si512(permutex_idx_table_17u_5);

        __m512i shift_masks_ptr[6];
        shift_masks_ptr[0] = _mm512_load_si512(shift_mask_table_17u_0);
        shift_masks_ptr[1] = _mm512_load_si512(shift_mask_table_17u_1);
        shift_masks_ptr[2] = _mm512_load_si512(shift_mask_table_17u_2);
        shift_masks_ptr[3] = _mm512_load_si512(shift_mask_table_17u_3);
        shift_masks_ptr[4] = _mm512_load_si512(shift_mask_table_17u_4);
        shift_masks_ptr[5] = _mm512_load_si512(shift_mask_table_17u_5);

        for (uint32_t idx = 0; idx < num_elements_48; ++idx)
        {
            srcmm0 = _mm512_loadu_si512(src_ptr);
            srcmm1 = _mm512_loadu_si512(src_ptr + 64u);
            srcmm2 = _mm512_loadu_si512(src_ptr + 128u);

            zmm0 = _mm512_maskz_permutex2var_epi32(permutex_masks_17u_ptr[0], srcmm0, permutex_idx_ptr[0], srcmm1);
            zmm1 = _mm512_maskz_permutex2var_epi32(permutex_masks_17u_ptr[1], srcmm0, permutex_idx_ptr[1], srcmm1);
            zmm2 = _mm512_maskz_permutex2var_epi32(permutex_masks_17u_ptr[2], srcmm0, permutex_idx_ptr[2], srcmm1);
            zmm3 = _mm512_maskz_permutex2var_epi32(permutex_masks_17u_ptr[3], srcmm1, permutex_idx_ptr[3], srcmm2);
            zmm4 = _mm512_maskz_permutex2var_epi32(permutex_masks_17u_ptr[4], srcmm1, permutex_idx_ptr[4], srcmm2);
            zmm5 = _mm512_maskz_permutex2var_epi32(permutex_masks_17u_ptr[5], srcmm1, permutex_idx_ptr[5], srcmm2);

            zmm0 = _mm512_sllv_epi32(zmm0, shift_masks_ptr[0]);
            zmm1 = _mm512_sllv_epi32(zmm1, shift_masks_ptr[1]);
            zmm2 = _mm512_srlv_epi32(zmm2, shift_masks_ptr[2]);
            zmm3 = _mm512_sllv_epi32(zmm3, shift_masks_ptr[3]);
            zmm4 = _mm512_sllv_epi32(zmm4, shift_masks_ptr[4]);
            zmm5 = _mm512_srlv_epi32(zmm5, shift_masks_ptr[5]);

            zmm0 = _mm512_or_si512(zmm0, zmm1);
            zmm0 = _mm512_or_si512(zmm0, zmm2);
            zmm3 = _mm512_or_si512(zmm3, zmm4);
            zmm3 = _mm512_or_si512(zmm3, zmm5);

            _mm512_storeu_si512(dst_ptr, zmm0);
            _mm512_mask_storeu_epi16(dst_ptr + 64u, 0x0007FFFF, zmm3);

            src_ptr += 192u;
            dst_ptr += 17u * 6u;
        }

        for (uint32_t idx = 0; idx < num_elements_16; ++idx)
        {
            srcmm0 = _mm512_loadu_si512(src_ptr);
            zmm0 = _mm512_maskz_permutexvar_epi32(permutex_masks_17u_ptr[0], permutex_idx_ptr[0], srcmm0);
            zmm1 = _mm512_maskz_permutexvar_epi32(permutex_masks_17u_ptr[1], permutex_idx_ptr[1], srcmm0);
            zmm2 = _mm512_maskz_permutexvar_epi32(permutex_masks_17u_ptr[2], permutex_idx_ptr[2], srcmm0);

            zmm0 = _mm512_sllv_epi32(zmm0, shift_masks_ptr[0]);
            zmm1 = _mm512_sllv_epi32(zmm1, shift_masks_ptr[1]);
            zmm2 = _mm512_srlv_epi32(zmm2, shift_masks_ptr[2]);

            zmm0 = _mm512_or_si512(zmm0, zmm1);
            zmm0 = _mm512_or_si512(zmm0, zmm2);
            _mm512_mask_storeu_epi16(dst_ptr, 0x0001FFFF, zmm0);

            src_ptr += 64u;
            dst_ptr += 17u * 2u;
        }
    }

    if (tail > 0) {
        k0_qplc_pack_32u17u_tail(src_ptr, tail, dst_ptr);
    }
}

// ********************** 32u18u ****************************** //

OWN_QPLC_INLINE(void, px_qplc_pack_32u18u, (const uint8_t *src_ptr, uint32_t num_elements, uint8_t *dst_ptr, uint32_t start_bit))
{
    uint32_t bit_width = 18u;
    int32_t  bits_in_buf = (int32_t)(bit_width + start_bit);
    uint32_t *src_32u_ptr = (uint32_t *)src_ptr;
    uint32_t *dst_32u_ptr = (uint32_t *)dst_ptr;
    uint64_t src = (uint64_t)(*dst_32u_ptr) & OWN_BIT_MASK(start_bit);

    src |= ((uint64_t)(*src_32u_ptr)) << start_bit;
    src_32u_ptr++;
    num_elements--;

    while (0u < num_elements) {
        if (OWN_DWORD_WIDTH <= (uint32_t)bits_in_buf) {
            *dst_32u_ptr = (uint32_t)(src);
            dst_32u_ptr++;
            src = src >> OWN_DWORD_WIDTH;
            bits_in_buf -= OWN_DWORD_WIDTH;
        }
        src = src | (((uint64_t)(*src_32u_ptr)) << bits_in_buf);
        src_32u_ptr++;
        num_elements--;
        bits_in_buf += (int32_t)bit_width;
    }
    dst_ptr = (uint8_t *)dst_32u_ptr;
    while (0 < bits_in_buf) {
        *dst_ptr = (uint8_t)(src);
        bits_in_buf -= OWN_BYTE_WIDTH;
        dst_ptr++;
        src >>= OWN_BYTE_WIDTH;
    }
}

OWN_QPLC_INLINE(void, k0_qplc_pack_32u18u_tail, (const uint8_t *src_ptr, uint32_t num_elements, uint8_t *dst_ptr))
{
    __m512i srcmm0;
    __m512i zmm0, zmm1, zmm2;

    __mmask16 tail_mask = OWN_BIT_MASK(num_elements);
    __mmask64 store_mask = OWN_BIT_MASK(OWN_BITS_2_BYTE(num_elements * 18u));

    __m512i permutex_idx_ptr[3];
    permutex_idx_ptr[0] = _mm512_load_si512(permutex_idx_table_18u_0);
    permutex_idx_ptr[1] = _mm512_load_si512(permutex_idx_table_18u_1);
    permutex_idx_ptr[2] = _mm512_load_si512(permutex_idx_table_18u_2);

    __m512i shift_masks_ptr[3];
    shift_masks_ptr[0] = _mm512_load_si512(shift_mask_table_18u_0);
    shift_masks_ptr[1] = _mm512_load_si512(shift_mask_table_18u_1);
    shift_masks_ptr[2] = _mm512_load_si512(shift_mask_table_18u_2);

    srcmm0 = _mm512_maskz_loadu_epi32(tail_mask, src_ptr);
    zmm0 = _mm512_maskz_permutexvar_epi32(permutex_masks_18u_ptr[0], permutex_idx_ptr[0], srcmm0);
    zmm1 = _mm512_maskz_permutexvar_epi32(permutex_masks_18u_ptr[1], permutex_idx_ptr[1], srcmm0);
    zmm2 = _mm512_maskz_permutexvar_epi32(permutex_masks_18u_ptr[2], permutex_idx_ptr[2], srcmm0);

    zmm0 = _mm512_sllv_epi32(zmm0, shift_masks_ptr[0]);
    zmm1 = _mm512_sllv_epi32(zmm1, shift_masks_ptr[1]);
    zmm2 = _mm512_srlv_epi32(zmm2, shift_masks_ptr[2]);

    zmm0 = _mm512_or_si512(zmm0, zmm1);
    zmm0 = _mm512_or_si512(zmm0, zmm2);
    _mm512_mask_storeu_epi8(dst_ptr, store_mask, zmm0);
}

OWN_OPT_FUN(void, k0_qplc_pack_32u18u, (const uint8_t *src_ptr, uint32_t num_elements, uint8_t *dst_ptr, uint32_t start_bit))
{
    if (start_bit > 0u) {
        uint32_t align = own_get_align(start_bit, 18u, 32u);
        if (align > num_elements) {
            align = num_elements;
        }
        px_qplc_pack_32u18u(src_ptr, align, dst_ptr, start_bit);
        src_ptr += align * 4;
        dst_ptr += ((align * 18u) + start_bit) >> 3u;
        num_elements -= align;
    }

    uint32_t tail = num_elements % 16u;
    if (num_elements >= 16u)
    {
        uint32_t num_elements_48 = num_elements / 48u;
        uint32_t num_elements_16 = (num_elements % 48u) / 16u;
        __m512i srcmm0, srcmm1, srcmm2;
        __m512i zmm0, zmm1, zmm2, zmm3, zmm4, zmm5;

        __m512i permutex_idx_ptr[6];
        permutex_idx_ptr[0] = _mm512_load_si512(permutex_idx_table_18u_0);
        permutex_idx_ptr[1] = _mm512_load_si512(permutex_idx_table_18u_1);
        permutex_idx_ptr[2] = _mm512_load_si512(permutex_idx_table_18u_2);
        permutex_idx_ptr[3] = _mm512_load_si512(permutex_idx_table_18u_3);
        permutex_idx_ptr[4] = _mm512_load_si512(permutex_idx_table_18u_4);
        permutex_idx_ptr[5] = _mm512_load_si512(permutex_idx_table_18u_5);

        __m512i shift_masks_ptr[6];
        shift_masks_ptr[0] = _mm512_load_si512(shift_mask_table_18u_0);
        shift_masks_ptr[1] = _mm512_load_si512(shift_mask_table_18u_1);
        shift_masks_ptr[2] = _mm512_load_si512(shift_mask_table_18u_2);
        shift_masks_ptr[3] = _mm512_load_si512(shift_mask_table_18u_3);
        shift_masks_ptr[4] = _mm512_load_si512(shift_mask_table_18u_4);
        shift_masks_ptr[5] = _mm512_load_si512(shift_mask_table_18u_5);

        for (uint32_t idx = 0; idx < num_elements_48; ++idx)
        {
            srcmm0 = _mm512_loadu_si512(src_ptr);
            srcmm1 = _mm512_loadu_si512(src_ptr + 64u);
            srcmm2 = _mm512_loadu_si512(src_ptr + 128u);

            zmm0 = _mm512_maskz_permutex2var_epi32(permutex_masks_18u_ptr[0], srcmm0, permutex_idx_ptr[0], srcmm1);
            zmm1 = _mm512_maskz_permutex2var_epi32(permutex_masks_18u_ptr[1], srcmm0, permutex_idx_ptr[1], srcmm1);
            zmm2 = _mm512_maskz_permutex2var_epi32(permutex_masks_18u_ptr[2], srcmm0, permutex_idx_ptr[2], srcmm1);
            zmm3 = _mm512_maskz_permutex2var_epi32(permutex_masks_18u_ptr[3], srcmm1, permutex_idx_ptr[3], srcmm2);
            zmm4 = _mm512_maskz_permutex2var_epi32(permutex_masks_18u_ptr[4], srcmm1, permutex_idx_ptr[4], srcmm2);
            zmm5 = _mm512_maskz_permutex2var_epi32(permutex_masks_18u_ptr[5], srcmm1, permutex_idx_ptr[5], srcmm2);

            zmm0 = _mm512_sllv_epi32(zmm0, shift_masks_ptr[0]);
            zmm1 = _mm512_sllv_epi32(zmm1, shift_masks_ptr[1]);
            zmm2 = _mm512_srlv_epi32(zmm2, shift_masks_ptr[2]);
            zmm3 = _mm512_sllv_epi32(zmm3, shift_masks_ptr[3]);
            zmm4 = _mm512_sllv_epi32(zmm4, shift_masks_ptr[4]);
            zmm5 = _mm512_srlv_epi32(zmm5, shift_masks_ptr[5]);

            zmm0 = _mm512_or_si512(zmm0, zmm1);
            zmm0 = _mm512_or_si512(zmm0, zmm2);
            zmm3 = _mm512_or_si512(zmm3, zmm4);
            zmm3 = _mm512_or_si512(zmm3, zmm5);

            _mm512_storeu_si512(dst_ptr, zmm0);
            _mm512_mask_storeu_epi16(dst_ptr + 64u, 0x003FFFFF, zmm3);

            src_ptr += 192u;
            dst_ptr += 18u * 6u;
        }

        for (uint32_t idx = 0; idx < num_elements_16; ++idx)
        {
            srcmm0 = _mm512_loadu_si512(src_ptr);
            zmm0 = _mm512_maskz_permutexvar_epi32(permutex_masks_18u_ptr[0], permutex_idx_ptr[0], srcmm0);
            zmm1 = _mm512_maskz_permutexvar_epi32(permutex_masks_18u_ptr[1], permutex_idx_ptr[1], srcmm0);
            zmm2 = _mm512_maskz_permutexvar_epi32(permutex_masks_18u_ptr[2], permutex_idx_ptr[2], srcmm0);

            zmm0 = _mm512_sllv_epi32(zmm0, shift_masks_ptr[0]);
            zmm1 = _mm512_sllv_epi32(zmm1, shift_masks_ptr[1]);
            zmm2 = _mm512_srlv_epi32(zmm2, shift_masks_ptr[2]);

            zmm0 = _mm512_or_si512(zmm0, zmm1);
            zmm0 = _mm512_or_si512(zmm0, zmm2);
            _mm512_mask_storeu_epi16(dst_ptr, 0x0003FFFF, zmm0);

            src_ptr += 64u;
            dst_ptr += 18u * 2u;
        }
    }

    if (tail > 0) {
        k0_qplc_pack_32u18u_tail(src_ptr, tail, dst_ptr);
    }
}

// ********************** 32u19u ****************************** //

OWN_QPLC_INLINE(void, px_qplc_pack_32u19u, (const uint8_t *src_ptr, uint32_t num_elements, uint8_t *dst_ptr, uint32_t start_bit))
{
    uint32_t bit_width = 19u;
    int32_t  bits_in_buf = (int32_t)(bit_width + start_bit);
    uint32_t *src_32u_ptr = (uint32_t *)src_ptr;
    uint32_t *dst_32u_ptr = (uint32_t *)dst_ptr;
    uint64_t src = (uint64_t)(*dst_32u_ptr) & OWN_BIT_MASK(start_bit);

    src |= ((uint64_t)(*src_32u_ptr)) << start_bit;
    src_32u_ptr++;
    num_elements--;

    while (0u < num_elements) {
        if (OWN_DWORD_WIDTH <= (uint32_t)bits_in_buf) {
            *dst_32u_ptr = (uint32_t)(src);
            dst_32u_ptr++;
            src = src >> OWN_DWORD_WIDTH;
            bits_in_buf -= OWN_DWORD_WIDTH;
        }
        src = src | (((uint64_t)(*src_32u_ptr)) << bits_in_buf);
        src_32u_ptr++;
        num_elements--;
        bits_in_buf += (int32_t)bit_width;
    }
    dst_ptr = (uint8_t *)dst_32u_ptr;
    while (0 < bits_in_buf) {
        *dst_ptr = (uint8_t)(src);
        bits_in_buf -= OWN_BYTE_WIDTH;
        dst_ptr++;
        src >>= OWN_BYTE_WIDTH;
    }
}

OWN_QPLC_INLINE(void, k0_qplc_pack_32u19u_tail, (const uint8_t *src_ptr, uint32_t num_elements, uint8_t *dst_ptr))
{
    __m512i srcmm0;
    __m512i zmm0, zmm1, zmm2;

    __mmask16 tail_mask = OWN_BIT_MASK(num_elements);
    __mmask64 store_mask = OWN_BIT_MASK(OWN_BITS_2_BYTE(num_elements * 19u));

    __m512i permutex_idx_ptr[3];
    permutex_idx_ptr[0] = _mm512_load_si512(permutex_idx_table_19u_0);
    permutex_idx_ptr[1] = _mm512_load_si512(permutex_idx_table_19u_1);
    permutex_idx_ptr[2] = _mm512_load_si512(permutex_idx_table_19u_2);

    __m512i shift_masks_ptr[3];
    shift_masks_ptr[0] = _mm512_load_si512(shift_mask_table_19u_0);
    shift_masks_ptr[1] = _mm512_load_si512(shift_mask_table_19u_1);
    shift_masks_ptr[2] = _mm512_load_si512(shift_mask_table_19u_2);

    srcmm0 = _mm512_maskz_loadu_epi32(tail_mask, src_ptr);
    zmm0 = _mm512_maskz_permutexvar_epi32(permutex_masks_19u_ptr[0], permutex_idx_ptr[0], srcmm0);
    zmm1 = _mm512_maskz_permutexvar_epi32(permutex_masks_19u_ptr[1], permutex_idx_ptr[1], srcmm0);
    zmm2 = _mm512_maskz_permutexvar_epi32(permutex_masks_19u_ptr[2], permutex_idx_ptr[2], srcmm0);

    zmm0 = _mm512_sllv_epi32(zmm0, shift_masks_ptr[0]);
    zmm1 = _mm512_sllv_epi32(zmm1, shift_masks_ptr[1]);
    zmm2 = _mm512_srlv_epi32(zmm2, shift_masks_ptr[2]);

    zmm0 = _mm512_or_si512(zmm0, zmm1);
    zmm0 = _mm512_or_si512(zmm0, zmm2);
    _mm512_mask_storeu_epi8(dst_ptr, store_mask, zmm0);
}

OWN_OPT_FUN(void, k0_qplc_pack_32u19u, (const uint8_t *src_ptr, uint32_t num_elements, uint8_t *dst_ptr, uint32_t start_bit))
{
    if (start_bit > 0u) {
        uint32_t align = own_get_align(start_bit, 19u, 32u);
        if (align > num_elements) {
            align = num_elements;
        }
        px_qplc_pack_32u19u(src_ptr, align, dst_ptr, start_bit);
        src_ptr += align * 4;
        dst_ptr += ((align * 19u) + start_bit) >> 3u;
        num_elements -= align;
    }

    uint32_t tail = num_elements % 16u;
    if (num_elements >= 16u)
    {
        uint32_t num_elements_48 = num_elements / 48u;
        uint32_t num_elements_16 = (num_elements % 48u) / 16u;
        __m512i srcmm0, srcmm1, srcmm2;
        __m512i zmm0, zmm1, zmm2, zmm3, zmm4, zmm5;

        __m512i permutex_idx_ptr[6];
        permutex_idx_ptr[0] = _mm512_load_si512(permutex_idx_table_19u_0);
        permutex_idx_ptr[1] = _mm512_load_si512(permutex_idx_table_19u_1);
        permutex_idx_ptr[2] = _mm512_load_si512(permutex_idx_table_19u_2);
        permutex_idx_ptr[3] = _mm512_load_si512(permutex_idx_table_19u_3);
        permutex_idx_ptr[4] = _mm512_load_si512(permutex_idx_table_19u_4);
        permutex_idx_ptr[5] = _mm512_load_si512(permutex_idx_table_19u_5);

        __m512i shift_masks_ptr[6];
        shift_masks_ptr[0] = _mm512_load_si512(shift_mask_table_19u_0);
        shift_masks_ptr[1] = _mm512_load_si512(shift_mask_table_19u_1);
        shift_masks_ptr[2] = _mm512_load_si512(shift_mask_table_19u_2);
        shift_masks_ptr[3] = _mm512_load_si512(shift_mask_table_19u_3);
        shift_masks_ptr[4] = _mm512_load_si512(shift_mask_table_19u_4);
        shift_masks_ptr[5] = _mm512_load_si512(shift_mask_table_19u_5);

        for (uint32_t idx = 0; idx < num_elements_48; ++idx)
        {
            srcmm0 = _mm512_loadu_si512(src_ptr);
            srcmm1 = _mm512_loadu_si512(src_ptr + 64u);
            srcmm2 = _mm512_loadu_si512(src_ptr + 128u);

            zmm0 = _mm512_maskz_permutex2var_epi32(permutex_masks_19u_ptr[0], srcmm0, permutex_idx_ptr[0], srcmm1);
            zmm1 = _mm512_maskz_permutex2var_epi32(permutex_masks_19u_ptr[1], srcmm0, permutex_idx_ptr[1], srcmm1);
            zmm2 = _mm512_maskz_permutex2var_epi32(permutex_masks_19u_ptr[2], srcmm0, permutex_idx_ptr[2], srcmm1);
            zmm3 = _mm512_maskz_permutex2var_epi32(permutex_masks_19u_ptr[3], srcmm1, permutex_idx_ptr[3], srcmm2);
            zmm4 = _mm512_maskz_permutex2var_epi32(permutex_masks_19u_ptr[4], srcmm1, permutex_idx_ptr[4], srcmm2);
            zmm5 = _mm512_maskz_permutex2var_epi32(permutex_masks_19u_ptr[5], srcmm1, permutex_idx_ptr[5], srcmm2);

            zmm0 = _mm512_sllv_epi32(zmm0, shift_masks_ptr[0]);
            zmm1 = _mm512_sllv_epi32(zmm1, shift_masks_ptr[1]);
            zmm2 = _mm512_srlv_epi32(zmm2, shift_masks_ptr[2]);
            zmm3 = _mm512_sllv_epi32(zmm3, shift_masks_ptr[3]);
            zmm4 = _mm512_sllv_epi32(zmm4, shift_masks_ptr[4]);
            zmm5 = _mm512_srlv_epi32(zmm5, shift_masks_ptr[5]);

            zmm0 = _mm512_or_si512(zmm0, zmm1);
            zmm0 = _mm512_or_si512(zmm0, zmm2);
            zmm3 = _mm512_or_si512(zmm3, zmm4);
            zmm3 = _mm512_or_si512(zmm3, zmm5);

            _mm512_storeu_si512(dst_ptr, zmm0);
            _mm512_mask_storeu_epi16(dst_ptr + 64u, 0x01FFFFFF, zmm3);

            src_ptr += 192u;
            dst_ptr += 19u * 6u;
        }

        for (uint32_t idx = 0; idx < num_elements_16; ++idx)
        {
            srcmm0 = _mm512_loadu_si512(src_ptr);
            zmm0 = _mm512_maskz_permutexvar_epi32(permutex_masks_19u_ptr[0], permutex_idx_ptr[0], srcmm0);
            zmm1 = _mm512_maskz_permutexvar_epi32(permutex_masks_19u_ptr[1], permutex_idx_ptr[1], srcmm0);
            zmm2 = _mm512_maskz_permutexvar_epi32(permutex_masks_19u_ptr[2], permutex_idx_ptr[2], srcmm0);

            zmm0 = _mm512_sllv_epi32(zmm0, shift_masks_ptr[0]);
            zmm1 = _mm512_sllv_epi32(zmm1, shift_masks_ptr[1]);
            zmm2 = _mm512_srlv_epi32(zmm2, shift_masks_ptr[2]);

            zmm0 = _mm512_or_si512(zmm0, zmm1);
            zmm0 = _mm512_or_si512(zmm0, zmm2);
            _mm512_mask_storeu_epi16(dst_ptr, 0x0007FFFF, zmm0);

            src_ptr += 64u;
            dst_ptr += 19u * 2u;
        }
    }

    if (tail > 0) {
        k0_qplc_pack_32u19u_tail(src_ptr, tail, dst_ptr);
    }
}

// ********************** 32u20u ****************************** //

OWN_QPLC_INLINE(void, px_qplc_pack_32u20u, (const uint8_t *src_ptr, uint32_t num_elements, uint8_t *dst_ptr, uint32_t start_bit))
{
    uint32_t bit_width = 20u;
    int32_t  bits_in_buf = (int32_t)(bit_width + start_bit);
    uint32_t *src_32u_ptr = (uint32_t *)src_ptr;
    uint32_t *dst_32u_ptr = (uint32_t *)dst_ptr;
    uint64_t src = (uint64_t)(*dst_32u_ptr) & OWN_BIT_MASK(start_bit);

    src |= ((uint64_t)(*src_32u_ptr)) << start_bit;
    src_32u_ptr++;
    num_elements--;

    while (0u < num_elements) {
        if (OWN_DWORD_WIDTH <= (uint32_t)bits_in_buf) {
            *dst_32u_ptr = (uint32_t)(src);
            dst_32u_ptr++;
            src = src >> OWN_DWORD_WIDTH;
            bits_in_buf -= OWN_DWORD_WIDTH;
        }
        src = src | (((uint64_t)(*src_32u_ptr)) << bits_in_buf);
        src_32u_ptr++;
        num_elements--;
        bits_in_buf += (int32_t)bit_width;
    }
    dst_ptr = (uint8_t *)dst_32u_ptr;
    while (0 < bits_in_buf) {
        *dst_ptr = (uint8_t)(src);
        bits_in_buf -= OWN_BYTE_WIDTH;
        dst_ptr++;
        src >>= OWN_BYTE_WIDTH;
    }
}

OWN_QPLC_INLINE(void, k0_qplc_pack_32u20u_tail, (const uint8_t *src_ptr, uint32_t num_elements, uint8_t *dst_ptr))
{
    __m512i srcmm0;
    __m512i zmm0, zmm1, zmm2;

    __mmask16 tail_mask = OWN_BIT_MASK(num_elements);
    __mmask64 store_mask = OWN_BIT_MASK(OWN_BITS_2_BYTE(num_elements * 20u));

    __m512i permutex_idx_ptr[3];
    permutex_idx_ptr[0] = _mm512_load_si512(permutex_idx_table_20u_0);
    permutex_idx_ptr[1] = _mm512_load_si512(permutex_idx_table_20u_1);
    permutex_idx_ptr[2] = _mm512_load_si512(permutex_idx_table_20u_2);

    __m512i shift_masks_ptr[3];
    shift_masks_ptr[0] = _mm512_load_si512(shift_mask_table_20u_0);
    shift_masks_ptr[1] = _mm512_load_si512(shift_mask_table_20u_1);
    shift_masks_ptr[2] = _mm512_load_si512(shift_mask_table_20u_2);

    srcmm0 = _mm512_maskz_loadu_epi32(tail_mask, src_ptr);
    zmm0 = _mm512_maskz_permutexvar_epi32(permutex_masks_20u_ptr[0], permutex_idx_ptr[0], srcmm0);
    zmm1 = _mm512_maskz_permutexvar_epi32(permutex_masks_20u_ptr[1], permutex_idx_ptr[1], srcmm0);
    zmm2 = _mm512_maskz_permutexvar_epi32(permutex_masks_20u_ptr[2], permutex_idx_ptr[2], srcmm0);

    zmm0 = _mm512_sllv_epi32(zmm0, shift_masks_ptr[0]);
    zmm1 = _mm512_sllv_epi32(zmm1, shift_masks_ptr[1]);
    zmm2 = _mm512_srlv_epi32(zmm2, shift_masks_ptr[2]);

    zmm0 = _mm512_or_si512(zmm0, zmm1);
    zmm0 = _mm512_or_si512(zmm0, zmm2);
    _mm512_mask_storeu_epi8(dst_ptr, store_mask, zmm0);
}

OWN_OPT_FUN(void, k0_qplc_pack_32u20u, (const uint8_t *src_ptr, uint32_t num_elements, uint8_t *dst_ptr, uint32_t start_bit))
{
    if (start_bit > 0u) {
        uint32_t align = own_get_align(start_bit, 20u, 32u);
        if (align > num_elements) {
            align = num_elements;
        }
        px_qplc_pack_32u20u(src_ptr, align, dst_ptr, start_bit);
        src_ptr += align * 4;
        dst_ptr += ((align * 20u) + start_bit) >> 3u;
        num_elements -= align;
    }

    uint32_t tail = num_elements % 16u;
    if (num_elements >= 16u)
    {
        uint32_t num_elements_48 = num_elements / 48u;
        uint32_t num_elements_16 = (num_elements % 48u) / 16u;
        __m512i srcmm0, srcmm1, srcmm2;
        __m512i zmm0, zmm1, zmm2, zmm3, zmm4, zmm5;

        __m512i permutex_idx_ptr[6];
        permutex_idx_ptr[0] = _mm512_load_si512(permutex_idx_table_20u_0);
        permutex_idx_ptr[1] = _mm512_load_si512(permutex_idx_table_20u_1);
        permutex_idx_ptr[2] = _mm512_load_si512(permutex_idx_table_20u_2);
        permutex_idx_ptr[3] = _mm512_load_si512(permutex_idx_table_20u_3);
        permutex_idx_ptr[4] = _mm512_load_si512(permutex_idx_table_20u_4);
        permutex_idx_ptr[5] = _mm512_load_si512(permutex_idx_table_20u_5);

        __m512i shift_masks_ptr[3];
        shift_masks_ptr[0] = _mm512_load_si512(shift_mask_table_20u_0);
        shift_masks_ptr[1] = _mm512_load_si512(shift_mask_table_20u_1);
        shift_masks_ptr[2] = _mm512_load_si512(shift_mask_table_20u_2);

        for (uint32_t idx = 0; idx < num_elements_48; ++idx)
        {
            srcmm0 = _mm512_loadu_si512(src_ptr);
            srcmm1 = _mm512_loadu_si512(src_ptr + 64u);
            srcmm2 = _mm512_loadu_si512(src_ptr + 128u);

            zmm0 = _mm512_maskz_permutex2var_epi32(permutex_masks_20u_ptr[0], srcmm0, permutex_idx_ptr[0], srcmm1);
            zmm1 = _mm512_maskz_permutex2var_epi32(permutex_masks_20u_ptr[1], srcmm0, permutex_idx_ptr[1], srcmm1);
            zmm2 = _mm512_maskz_permutex2var_epi32(permutex_masks_20u_ptr[2], srcmm0, permutex_idx_ptr[2], srcmm1);
            zmm3 = _mm512_maskz_permutex2var_epi32(permutex_masks_20u_ptr[0], srcmm1, permutex_idx_ptr[3], srcmm2);
            zmm4 = _mm512_maskz_permutex2var_epi32(permutex_masks_20u_ptr[1], srcmm1, permutex_idx_ptr[4], srcmm2);
            zmm5 = _mm512_maskz_permutex2var_epi32(permutex_masks_20u_ptr[2], srcmm1, permutex_idx_ptr[5], srcmm2);

            zmm0 = _mm512_sllv_epi32(zmm0, shift_masks_ptr[0]);
            zmm1 = _mm512_sllv_epi32(zmm1, shift_masks_ptr[1]);
            zmm2 = _mm512_srlv_epi32(zmm2, shift_masks_ptr[2]);
            zmm3 = _mm512_sllv_epi32(zmm3, shift_masks_ptr[0]);
            zmm4 = _mm512_sllv_epi32(zmm4, shift_masks_ptr[1]);
            zmm5 = _mm512_srlv_epi32(zmm5, shift_masks_ptr[2]);

            zmm0 = _mm512_or_si512(zmm0, zmm1);
            zmm0 = _mm512_or_si512(zmm0, zmm2);
            zmm3 = _mm512_or_si512(zmm3, zmm4);
            zmm3 = _mm512_or_si512(zmm3, zmm5);

            _mm512_mask_storeu_epi16(dst_ptr, 0x3FFFFFFF, zmm0);
            _mm512_mask_storeu_epi16(dst_ptr + 60u, 0x3FFFFFFF, zmm3);

            src_ptr += 192u;
            dst_ptr += 20u * 6u;
        }

        for (uint32_t idx = 0; idx < num_elements_16; ++idx)
        {
            srcmm0 = _mm512_loadu_si512(src_ptr);
            zmm0 = _mm512_maskz_permutexvar_epi32(permutex_masks_20u_ptr[0], permutex_idx_ptr[0], srcmm0);
            zmm1 = _mm512_maskz_permutexvar_epi32(permutex_masks_20u_ptr[1], permutex_idx_ptr[1], srcmm0);
            zmm2 = _mm512_maskz_permutexvar_epi32(permutex_masks_20u_ptr[2], permutex_idx_ptr[2], srcmm0);

            zmm0 = _mm512_sllv_epi32(zmm0, shift_masks_ptr[0]);
            zmm1 = _mm512_sllv_epi32(zmm1, shift_masks_ptr[1]);
            zmm2 = _mm512_srlv_epi32(zmm2, shift_masks_ptr[2]);

            zmm0 = _mm512_or_si512(zmm0, zmm1);
            zmm0 = _mm512_or_si512(zmm0, zmm2);
            _mm512_mask_storeu_epi16(dst_ptr, 0x000FFFFF, zmm0);

            src_ptr += 64u;
            dst_ptr += 20u * 2u;
        }
    }

    if (tail > 0) {
        k0_qplc_pack_32u20u_tail(src_ptr, tail, dst_ptr);
    }
}

// ********************** 32u21u ****************************** //

OWN_QPLC_INLINE(void, px_qplc_pack_32u21u, (const uint8_t *src_ptr, uint32_t num_elements, uint8_t *dst_ptr, uint32_t start_bit))
{
    uint32_t bit_width = 21u;
    int32_t  bits_in_buf = (int32_t)(bit_width + start_bit);
    uint32_t *src_32u_ptr = (uint32_t *)src_ptr;
    uint32_t *dst_32u_ptr = (uint32_t *)dst_ptr;
    uint64_t src = (uint64_t)(*dst_32u_ptr) & OWN_BIT_MASK(start_bit);

    src |= ((uint64_t)(*src_32u_ptr)) << start_bit;
    src_32u_ptr++;
    num_elements--;

    while (0u < num_elements) {
        if (OWN_DWORD_WIDTH <= (uint32_t)bits_in_buf) {
            *dst_32u_ptr = (uint32_t)(src);
            dst_32u_ptr++;
            src = src >> OWN_DWORD_WIDTH;
            bits_in_buf -= OWN_DWORD_WIDTH;
        }
        src = src | (((uint64_t)(*src_32u_ptr)) << bits_in_buf);
        src_32u_ptr++;
        num_elements--;
        bits_in_buf += (int32_t)bit_width;
    }
    dst_ptr = (uint8_t *)dst_32u_ptr;
    while (0 < bits_in_buf) {
        *dst_ptr = (uint8_t)(src);
        bits_in_buf -= OWN_BYTE_WIDTH;
        dst_ptr++;
        src >>= OWN_BYTE_WIDTH;
    }
}

OWN_QPLC_INLINE(void, k0_qplc_pack_32u21u_tail, (const uint8_t *src_ptr, uint32_t num_elements, uint8_t *dst_ptr))
{
    __m512i srcmm0;
    __m512i zmm0, zmm1, zmm2;

    __mmask16 tail_mask = OWN_BIT_MASK(num_elements);
    __mmask64 store_mask = OWN_BIT_MASK(OWN_BITS_2_BYTE(num_elements * 21u));

    __m512i permutex_idx_ptr[3];
    permutex_idx_ptr[0] = _mm512_load_si512(permutex_idx_table_21u_0);
    permutex_idx_ptr[1] = _mm512_load_si512(permutex_idx_table_21u_1);
    permutex_idx_ptr[2] = _mm512_load_si512(permutex_idx_table_21u_2);

    __m512i shift_masks_ptr[3];
    shift_masks_ptr[0] = _mm512_load_si512(shift_mask_table_21u_0);
    shift_masks_ptr[1] = _mm512_load_si512(shift_mask_table_21u_1);
    shift_masks_ptr[2] = _mm512_load_si512(shift_mask_table_21u_2);

    srcmm0 = _mm512_maskz_loadu_epi32(tail_mask, src_ptr);
    zmm0 = _mm512_maskz_permutexvar_epi32(permutex_masks_21u_ptr[0], permutex_idx_ptr[0], srcmm0);
    zmm1 = _mm512_maskz_permutexvar_epi32(permutex_masks_21u_ptr[1], permutex_idx_ptr[1], srcmm0);
    zmm2 = _mm512_maskz_permutexvar_epi32(permutex_masks_21u_ptr[2], permutex_idx_ptr[2], srcmm0);

    zmm0 = _mm512_sllv_epi32(zmm0, shift_masks_ptr[0]);
    zmm1 = _mm512_sllv_epi32(zmm1, shift_masks_ptr[1]);
    zmm2 = _mm512_srlv_epi32(zmm2, shift_masks_ptr[2]);

    zmm0 = _mm512_or_si512(zmm0, zmm1);
    zmm0 = _mm512_or_si512(zmm0, zmm2);
    _mm512_mask_storeu_epi8(dst_ptr, store_mask, zmm0);
}

OWN_OPT_FUN(void, k0_qplc_pack_32u21u, (const uint8_t *src_ptr, uint32_t num_elements, uint8_t *dst_ptr, uint32_t start_bit))
{
    if (start_bit > 0u) {
        uint32_t align = own_get_align(start_bit, 21u, 32u);
        if (align > num_elements) {
            align = num_elements;
        }
        px_qplc_pack_32u21u(src_ptr, align, dst_ptr, start_bit);
        src_ptr += align * 4;
        dst_ptr += ((align * 21u) + start_bit) >> 3u;
        num_elements -= align;
    }

    uint32_t tail = num_elements % 16u;
    if (num_elements >= 16u)
    {
        uint32_t num_elements_48 = num_elements / 48u;
        uint32_t num_elements_16 = (num_elements % 48u) / 16u;
        __m512i srcmm0, srcmm1, srcmm2;
        __m512i zmm0, zmm1, zmm2, zmm3, zmm4, zmm5;

        __m512i permutex_idx_ptr[6];
        permutex_idx_ptr[0] = _mm512_load_si512(permutex_idx_table_21u_0);
        permutex_idx_ptr[1] = _mm512_load_si512(permutex_idx_table_21u_1);
        permutex_idx_ptr[2] = _mm512_load_si512(permutex_idx_table_21u_2);
        permutex_idx_ptr[3] = _mm512_load_si512(permutex_idx_table_21u_3);
        permutex_idx_ptr[4] = _mm512_load_si512(permutex_idx_table_21u_4);
        permutex_idx_ptr[5] = _mm512_load_si512(permutex_idx_table_21u_5);

        __m512i shift_masks_ptr[6];
        shift_masks_ptr[0] = _mm512_load_si512(shift_mask_table_21u_0);
        shift_masks_ptr[1] = _mm512_load_si512(shift_mask_table_21u_1);
        shift_masks_ptr[2] = _mm512_load_si512(shift_mask_table_21u_2);
        shift_masks_ptr[3] = _mm512_load_si512(shift_mask_table_21u_3);
        shift_masks_ptr[4] = _mm512_load_si512(shift_mask_table_21u_4);
        shift_masks_ptr[5] = _mm512_load_si512(shift_mask_table_21u_5);

        for (uint32_t idx = 0; idx < num_elements_48; ++idx)
        {
            srcmm0 = _mm512_loadu_si512(src_ptr);
            srcmm1 = _mm512_loadu_si512(src_ptr + 64u);
            srcmm2 = _mm512_loadu_si512(src_ptr + 128u);

            zmm0 = _mm512_maskz_permutex2var_epi32(permutex_masks_21u_ptr[0], srcmm0, permutex_idx_ptr[0], srcmm1);
            zmm1 = _mm512_maskz_permutex2var_epi32(permutex_masks_21u_ptr[1], srcmm0, permutex_idx_ptr[1], srcmm1);
            zmm2 = _mm512_maskz_permutex2var_epi32(permutex_masks_21u_ptr[2], srcmm0, permutex_idx_ptr[2], srcmm1);
            zmm3 = _mm512_maskz_permutex2var_epi32(permutex_masks_21u_ptr[3], srcmm1, permutex_idx_ptr[3], srcmm2);
            zmm4 = _mm512_maskz_permutex2var_epi32(permutex_masks_21u_ptr[4], srcmm1, permutex_idx_ptr[4], srcmm2);
            zmm5 = _mm512_maskz_permutex2var_epi32(permutex_masks_21u_ptr[5], srcmm1, permutex_idx_ptr[5], srcmm2);

            zmm0 = _mm512_sllv_epi32(zmm0, shift_masks_ptr[0]);
            zmm1 = _mm512_sllv_epi32(zmm1, shift_masks_ptr[1]);
            zmm2 = _mm512_srlv_epi32(zmm2, shift_masks_ptr[2]);
            zmm3 = _mm512_sllv_epi32(zmm3, shift_masks_ptr[3]);
            zmm4 = _mm512_sllv_epi32(zmm4, shift_masks_ptr[4]);
            zmm5 = _mm512_srlv_epi32(zmm5, shift_masks_ptr[5]);

            zmm0 = _mm512_or_si512(zmm0, zmm1);
            zmm0 = _mm512_or_si512(zmm0, zmm2);
            zmm3 = _mm512_or_si512(zmm3, zmm4);
            zmm3 = _mm512_or_si512(zmm3, zmm5);

            _mm512_storeu_si512(dst_ptr, zmm0);
            _mm512_mask_storeu_epi16(dst_ptr + 64u, 0x7FFFFFFF, zmm3);

            src_ptr += 192u;
            dst_ptr += 21u * 6u;
        }

        for (uint32_t idx = 0; idx < num_elements_16; ++idx)
        {
            srcmm0 = _mm512_loadu_si512(src_ptr);
            zmm0 = _mm512_maskz_permutexvar_epi32(permutex_masks_21u_ptr[0], permutex_idx_ptr[0], srcmm0);
            zmm1 = _mm512_maskz_permutexvar_epi32(permutex_masks_21u_ptr[1], permutex_idx_ptr[1], srcmm0);
            zmm2 = _mm512_maskz_permutexvar_epi32(permutex_masks_21u_ptr[2], permutex_idx_ptr[2], srcmm0);

            zmm0 = _mm512_sllv_epi32(zmm0, shift_masks_ptr[0]);
            zmm1 = _mm512_sllv_epi32(zmm1, shift_masks_ptr[1]);
            zmm2 = _mm512_srlv_epi32(zmm2, shift_masks_ptr[2]);

            zmm0 = _mm512_or_si512(zmm0, zmm1);
            zmm0 = _mm512_or_si512(zmm0, zmm2);
            _mm512_mask_storeu_epi16(dst_ptr, 0x001FFFFF, zmm0);

            src_ptr += 64u;
            dst_ptr += 21u * 2u;
        }
    }

    if (tail > 0) {
        k0_qplc_pack_32u21u_tail(src_ptr, tail, dst_ptr);
    }
}

// ********************** 32u22u ****************************** //

OWN_QPLC_INLINE(void, px_qplc_pack_32u22u, (const uint8_t *src_ptr, uint32_t num_elements, uint8_t *dst_ptr, uint32_t start_bit))
{
    uint32_t bit_width = 22u;
    int32_t  bits_in_buf = (int32_t)(bit_width + start_bit);
    uint32_t *src_32u_ptr = (uint32_t *)src_ptr;
    uint32_t *dst_32u_ptr = (uint32_t *)dst_ptr;
    uint64_t src = (uint64_t)(*dst_32u_ptr) & OWN_BIT_MASK(start_bit);

    src |= ((uint64_t)(*src_32u_ptr)) << start_bit;
    src_32u_ptr++;
    num_elements--;

    while (0u < num_elements) {
        if (OWN_DWORD_WIDTH <= (uint32_t)bits_in_buf) {
            *dst_32u_ptr = (uint32_t)(src);
            dst_32u_ptr++;
            src = src >> OWN_DWORD_WIDTH;
            bits_in_buf -= OWN_DWORD_WIDTH;
        }
        src = src | (((uint64_t)(*src_32u_ptr)) << bits_in_buf);
        src_32u_ptr++;
        num_elements--;
        bits_in_buf += (int32_t)bit_width;
    }
    dst_ptr = (uint8_t *)dst_32u_ptr;
    while (0 < bits_in_buf) {
        *dst_ptr = (uint8_t)(src);
        bits_in_buf -= OWN_BYTE_WIDTH;
        dst_ptr++;
        src >>= OWN_BYTE_WIDTH;
    }
}

OWN_QPLC_INLINE(void, k0_qplc_pack_32u22u_tail, (const uint8_t *src_ptr, uint32_t num_elements, uint8_t *dst_ptr))
{
    __m512i srcmm0;
    __m512i zmm0, zmm1, zmm2;

    __mmask16 tail_mask = OWN_BIT_MASK(num_elements);
    __mmask64 store_mask = OWN_BIT_MASK(OWN_BITS_2_BYTE(num_elements * 22u));

    __m512i permutex_idx_ptr[3];
    permutex_idx_ptr[0] = _mm512_load_si512(permutex_idx_table_22u_0);
    permutex_idx_ptr[1] = _mm512_load_si512(permutex_idx_table_22u_1);
    permutex_idx_ptr[2] = _mm512_load_si512(permutex_idx_table_22u_2);

    __m512i shift_masks_ptr[3];
    shift_masks_ptr[0] = _mm512_load_si512(shift_mask_table_22u_0);
    shift_masks_ptr[1] = _mm512_load_si512(shift_mask_table_22u_1);
    shift_masks_ptr[2] = _mm512_load_si512(shift_mask_table_22u_2);

    srcmm0 = _mm512_maskz_loadu_epi32(tail_mask, src_ptr);
    zmm0 = _mm512_maskz_permutexvar_epi32(permutex_masks_22u_ptr[0], permutex_idx_ptr[0], srcmm0);
    zmm1 = _mm512_maskz_permutexvar_epi32(permutex_masks_22u_ptr[1], permutex_idx_ptr[1], srcmm0);
    zmm2 = _mm512_maskz_permutexvar_epi32(permutex_masks_22u_ptr[2], permutex_idx_ptr[2], srcmm0);

    zmm0 = _mm512_sllv_epi32(zmm0, shift_masks_ptr[0]);
    zmm1 = _mm512_sllv_epi32(zmm1, shift_masks_ptr[1]);
    zmm2 = _mm512_srlv_epi32(zmm2, shift_masks_ptr[2]);

    zmm0 = _mm512_or_si512(zmm0, zmm1);
    zmm0 = _mm512_or_si512(zmm0, zmm2);
    _mm512_mask_storeu_epi8(dst_ptr, store_mask, zmm0);
}

OWN_OPT_FUN(void, k0_qplc_pack_32u22u, (const uint8_t *src_ptr, uint32_t num_elements, uint8_t *dst_ptr, uint32_t start_bit))
{
    if (start_bit > 0u) {
        uint32_t align = own_get_align(start_bit, 22u, 32u);
        if (align > num_elements) {
            align = num_elements;
        }
        px_qplc_pack_32u22u(src_ptr, align, dst_ptr, start_bit);
        src_ptr += align * 4;
        dst_ptr += ((align * 22u) + start_bit) >> 3u;
        num_elements -= align;
    }

    uint32_t tail = num_elements % 16u;
    if (num_elements >= 16u)
    {
        uint32_t num_elements_64 = num_elements / 64u;
        uint32_t num_elements_16 = (num_elements % 64u) / 16u;
        __m512i srcmm0, srcmm1, srcmm2, srcmm3;
        __m512i zmm0, zmm1, zmm2, zmm3, zmm4, zmm5, zmm6, zmm7, zmm8;

        __m512i permutex_idx_ptr[9];
        permutex_idx_ptr[0] = _mm512_load_si512(permutex_idx_table_22u_0);
        permutex_idx_ptr[1] = _mm512_load_si512(permutex_idx_table_22u_1);
        permutex_idx_ptr[2] = _mm512_load_si512(permutex_idx_table_22u_2);
        permutex_idx_ptr[3] = _mm512_load_si512(permutex_idx_table_22u_3);
        permutex_idx_ptr[4] = _mm512_load_si512(permutex_idx_table_22u_4);
        permutex_idx_ptr[5] = _mm512_load_si512(permutex_idx_table_22u_5);
        permutex_idx_ptr[6] = _mm512_load_si512(permutex_idx_table_22u_6);
        permutex_idx_ptr[7] = _mm512_load_si512(permutex_idx_table_22u_7);
        permutex_idx_ptr[8] = _mm512_load_si512(permutex_idx_table_22u_8);

        __m512i shift_masks_ptr[9];
        shift_masks_ptr[0] = _mm512_load_si512(shift_mask_table_22u_0);
        shift_masks_ptr[1] = _mm512_load_si512(shift_mask_table_22u_1);
        shift_masks_ptr[2] = _mm512_load_si512(shift_mask_table_22u_2);
        shift_masks_ptr[3] = _mm512_load_si512(shift_mask_table_22u_3);
        shift_masks_ptr[4] = _mm512_load_si512(shift_mask_table_22u_4);
        shift_masks_ptr[5] = _mm512_load_si512(shift_mask_table_22u_5);
        shift_masks_ptr[6] = _mm512_load_si512(shift_mask_table_22u_6);
        shift_masks_ptr[7] = _mm512_load_si512(shift_mask_table_22u_7);
        shift_masks_ptr[8] = _mm512_load_si512(shift_mask_table_22u_8);

        for (uint32_t idx = 0; idx < num_elements_64; ++idx)
        {
            srcmm0 = _mm512_loadu_si512(src_ptr);
            srcmm1 = _mm512_loadu_si512(src_ptr + 64u);
            srcmm2 = _mm512_loadu_si512(src_ptr + 128u);
            srcmm3 = _mm512_loadu_si512(src_ptr + 192u);

            zmm0 = _mm512_maskz_permutex2var_epi32(permutex_masks_22u_ptr[0], srcmm0, permutex_idx_ptr[0], srcmm1);
            zmm1 = _mm512_maskz_permutex2var_epi32(permutex_masks_22u_ptr[1], srcmm0, permutex_idx_ptr[1], srcmm1);
            zmm2 = _mm512_maskz_permutex2var_epi32(permutex_masks_22u_ptr[2], srcmm0, permutex_idx_ptr[2], srcmm1);
            zmm3 = _mm512_maskz_permutex2var_epi32(permutex_masks_22u_ptr[3], srcmm1, permutex_idx_ptr[3], srcmm2);
            zmm4 = _mm512_maskz_permutex2var_epi32(permutex_masks_22u_ptr[4], srcmm1, permutex_idx_ptr[4], srcmm2);
            zmm5 = _mm512_maskz_permutex2var_epi32(permutex_masks_22u_ptr[5], srcmm1, permutex_idx_ptr[5], srcmm2);
            zmm6 = _mm512_maskz_permutex2var_epi32(permutex_masks_22u_ptr[6], srcmm2, permutex_idx_ptr[6], srcmm3);
            zmm7 = _mm512_maskz_permutex2var_epi32(permutex_masks_22u_ptr[7], srcmm2, permutex_idx_ptr[7], srcmm3);
            zmm8 = _mm512_maskz_permutex2var_epi32(permutex_masks_22u_ptr[8], srcmm2, permutex_idx_ptr[8], srcmm3);

            zmm0 = _mm512_sllv_epi32(zmm0, shift_masks_ptr[0]);
            zmm1 = _mm512_sllv_epi32(zmm1, shift_masks_ptr[1]);
            zmm2 = _mm512_srlv_epi32(zmm2, shift_masks_ptr[2]);
            zmm3 = _mm512_sllv_epi32(zmm3, shift_masks_ptr[3]);
            zmm4 = _mm512_sllv_epi32(zmm4, shift_masks_ptr[4]);
            zmm5 = _mm512_srlv_epi32(zmm5, shift_masks_ptr[5]);
            zmm6 = _mm512_sllv_epi32(zmm6, shift_masks_ptr[6]);
            zmm7 = _mm512_sllv_epi32(zmm7, shift_masks_ptr[7]);
            zmm8 = _mm512_srlv_epi32(zmm8, shift_masks_ptr[8]);

            zmm0 = _mm512_or_si512(zmm0, zmm1);
            zmm0 = _mm512_or_si512(zmm0, zmm2);
            zmm3 = _mm512_or_si512(zmm3, zmm4);
            zmm3 = _mm512_or_si512(zmm3, zmm5);
            zmm6 = _mm512_or_si512(zmm6, zmm7);
            zmm6 = _mm512_or_si512(zmm6, zmm8);

            _mm512_storeu_si512(dst_ptr, zmm0);
            _mm512_storeu_si512(dst_ptr + 64u, zmm3);
            _mm512_mask_storeu_epi16(dst_ptr + 128u, 0x00FFFFFF, zmm6);

            src_ptr += 256u;
            dst_ptr += 22u * 8u;
        }

        for (uint32_t idx = 0; idx < num_elements_16; ++idx)
        {
            srcmm0 = _mm512_loadu_si512(src_ptr);
            zmm0 = _mm512_maskz_permutexvar_epi32(permutex_masks_22u_ptr[0], permutex_idx_ptr[0], srcmm0);
            zmm1 = _mm512_maskz_permutexvar_epi32(permutex_masks_22u_ptr[1], permutex_idx_ptr[1], srcmm0);
            zmm2 = _mm512_maskz_permutexvar_epi32(permutex_masks_22u_ptr[2], permutex_idx_ptr[2], srcmm0);

            zmm0 = _mm512_sllv_epi32(zmm0, shift_masks_ptr[0]);
            zmm1 = _mm512_sllv_epi32(zmm1, shift_masks_ptr[1]);
            zmm2 = _mm512_srlv_epi32(zmm2, shift_masks_ptr[2]);

            zmm0 = _mm512_or_si512(zmm0, zmm1);
            zmm0 = _mm512_or_si512(zmm0, zmm2);
            _mm512_mask_storeu_epi16(dst_ptr, 0x003FFFFF, zmm0);

            src_ptr += 64u;
            dst_ptr += 22u * 2u;
        }
    }

    if (tail > 0) {
        k0_qplc_pack_32u22u_tail(src_ptr, tail, dst_ptr);
    }
}

// ********************** 32u23u ****************************** //

OWN_QPLC_INLINE(void, px_qplc_pack_32u23u, (const uint8_t *src_ptr, uint32_t num_elements, uint8_t *dst_ptr, uint32_t start_bit))
{
    uint32_t bit_width = 23u;
    int32_t  bits_in_buf = (int32_t)(bit_width + start_bit);
    uint32_t *src_32u_ptr = (uint32_t *)src_ptr;
    uint32_t *dst_32u_ptr = (uint32_t *)dst_ptr;
    uint64_t src = (uint64_t)(*dst_32u_ptr) & OWN_BIT_MASK(start_bit);

    src |= ((uint64_t)(*src_32u_ptr)) << start_bit;
    src_32u_ptr++;
    num_elements--;

    while (0u < num_elements) {
        if (OWN_DWORD_WIDTH <= (uint32_t)bits_in_buf) {
            *dst_32u_ptr = (uint32_t)(src);
            dst_32u_ptr++;
            src = src >> OWN_DWORD_WIDTH;
            bits_in_buf -= OWN_DWORD_WIDTH;
        }
        src = src | (((uint64_t)(*src_32u_ptr)) << bits_in_buf);
        src_32u_ptr++;
        num_elements--;
        bits_in_buf += (int32_t)bit_width;
    }
    dst_ptr = (uint8_t *)dst_32u_ptr;
    while (0 < bits_in_buf) {
        *dst_ptr = (uint8_t)(src);
        bits_in_buf -= OWN_BYTE_WIDTH;
        dst_ptr++;
        src >>= OWN_BYTE_WIDTH;
    }
}

OWN_QPLC_INLINE(void, k0_qplc_pack_32u23u_tail, (const uint8_t *src_ptr, uint32_t num_elements, uint8_t *dst_ptr))
{
    __m512i srcmm0;
    __m512i zmm0, zmm1, zmm2;

    __mmask16 tail_mask = OWN_BIT_MASK(num_elements);
    __mmask64 store_mask = OWN_BIT_MASK(OWN_BITS_2_BYTE(num_elements * 23u));

    __m512i permutex_idx_ptr[3];
    permutex_idx_ptr[0] = _mm512_load_si512(permutex_idx_table_23u_0);
    permutex_idx_ptr[1] = _mm512_load_si512(permutex_idx_table_23u_1);
    permutex_idx_ptr[2] = _mm512_load_si512(permutex_idx_table_23u_2);

    __m512i shift_masks_ptr[3];
    shift_masks_ptr[0] = _mm512_load_si512(shift_mask_table_23u_0);
    shift_masks_ptr[1] = _mm512_load_si512(shift_mask_table_23u_1);
    shift_masks_ptr[2] = _mm512_load_si512(shift_mask_table_23u_2);

    srcmm0 = _mm512_maskz_loadu_epi32(tail_mask, src_ptr);
    zmm0 = _mm512_maskz_permutexvar_epi32(permutex_masks_23u_ptr[0], permutex_idx_ptr[0], srcmm0);
    zmm1 = _mm512_maskz_permutexvar_epi32(permutex_masks_23u_ptr[1], permutex_idx_ptr[1], srcmm0);
    zmm2 = _mm512_maskz_permutexvar_epi32(permutex_masks_23u_ptr[2], permutex_idx_ptr[2], srcmm0);

    zmm0 = _mm512_sllv_epi32(zmm0, shift_masks_ptr[0]);
    zmm1 = _mm512_sllv_epi32(zmm1, shift_masks_ptr[1]);
    zmm2 = _mm512_srlv_epi32(zmm2, shift_masks_ptr[2]);

    zmm0 = _mm512_or_si512(zmm0, zmm1);
    zmm0 = _mm512_or_si512(zmm0, zmm2);
    _mm512_mask_storeu_epi8(dst_ptr, store_mask, zmm0);
}

OWN_OPT_FUN(void, k0_qplc_pack_32u23u, (const uint8_t *src_ptr, uint32_t num_elements, uint8_t *dst_ptr, uint32_t start_bit))
{
    if (start_bit > 0u) {
        uint32_t align = own_get_align(start_bit, 23u, 32u);
        if (align > num_elements) {
            align = num_elements;
        }
        px_qplc_pack_32u23u(src_ptr, align, dst_ptr, start_bit);
        src_ptr += align * 4;
        dst_ptr += ((align * 23u) + start_bit) >> 3u;
        num_elements -= align;
    }

    uint32_t tail = num_elements % 16u;
    if (num_elements >= 16u)
    {
        uint32_t num_elements_64 = num_elements / 64u;
        uint32_t num_elements_16 = (num_elements % 64u) / 16u;
        __m512i srcmm0, srcmm1, srcmm2, srcmm3;
        __m512i zmm0, zmm1, zmm2, zmm3, zmm4, zmm5, zmm6, zmm7, zmm8;

        __m512i permutex_idx_ptr[9];
        permutex_idx_ptr[0] = _mm512_load_si512(permutex_idx_table_23u_0);
        permutex_idx_ptr[1] = _mm512_load_si512(permutex_idx_table_23u_1);
        permutex_idx_ptr[2] = _mm512_load_si512(permutex_idx_table_23u_2);
        permutex_idx_ptr[3] = _mm512_load_si512(permutex_idx_table_23u_3);
        permutex_idx_ptr[4] = _mm512_load_si512(permutex_idx_table_23u_4);
        permutex_idx_ptr[5] = _mm512_load_si512(permutex_idx_table_23u_5);
        permutex_idx_ptr[6] = _mm512_load_si512(permutex_idx_table_23u_6);
        permutex_idx_ptr[7] = _mm512_load_si512(permutex_idx_table_23u_7);
        permutex_idx_ptr[8] = _mm512_load_si512(permutex_idx_table_23u_8);

        __m512i shift_masks_ptr[9];
        shift_masks_ptr[0] = _mm512_load_si512(shift_mask_table_23u_0);
        shift_masks_ptr[1] = _mm512_load_si512(shift_mask_table_23u_1);
        shift_masks_ptr[2] = _mm512_load_si512(shift_mask_table_23u_2);
        shift_masks_ptr[3] = _mm512_load_si512(shift_mask_table_23u_3);
        shift_masks_ptr[4] = _mm512_load_si512(shift_mask_table_23u_4);
        shift_masks_ptr[5] = _mm512_load_si512(shift_mask_table_23u_5);
        shift_masks_ptr[6] = _mm512_load_si512(shift_mask_table_23u_6);
        shift_masks_ptr[7] = _mm512_load_si512(shift_mask_table_23u_7);
        shift_masks_ptr[8] = _mm512_load_si512(shift_mask_table_23u_8);

        for (uint32_t idx = 0; idx < num_elements_64; ++idx)
        {
            srcmm0 = _mm512_loadu_si512(src_ptr);
            srcmm1 = _mm512_loadu_si512(src_ptr + 64u);
            srcmm2 = _mm512_loadu_si512(src_ptr + 128u);
            srcmm3 = _mm512_loadu_si512(src_ptr + 192u);

            zmm0 = _mm512_maskz_permutex2var_epi32(permutex_masks_23u_ptr[0], srcmm0, permutex_idx_ptr[0], srcmm1);
            zmm1 = _mm512_maskz_permutex2var_epi32(permutex_masks_23u_ptr[1], srcmm0, permutex_idx_ptr[1], srcmm1);
            zmm2 = _mm512_maskz_permutex2var_epi32(permutex_masks_23u_ptr[2], srcmm0, permutex_idx_ptr[2], srcmm1);
            zmm3 = _mm512_maskz_permutex2var_epi32(permutex_masks_23u_ptr[3], srcmm1, permutex_idx_ptr[3], srcmm2);
            zmm4 = _mm512_maskz_permutex2var_epi32(permutex_masks_23u_ptr[4], srcmm1, permutex_idx_ptr[4], srcmm2);
            zmm5 = _mm512_maskz_permutex2var_epi32(permutex_masks_23u_ptr[5], srcmm1, permutex_idx_ptr[5], srcmm2);
            zmm6 = _mm512_maskz_permutex2var_epi32(permutex_masks_23u_ptr[6], srcmm2, permutex_idx_ptr[6], srcmm3);
            zmm7 = _mm512_maskz_permutex2var_epi32(permutex_masks_23u_ptr[7], srcmm2, permutex_idx_ptr[7], srcmm3);
            zmm8 = _mm512_maskz_permutex2var_epi32(permutex_masks_23u_ptr[8], srcmm2, permutex_idx_ptr[8], srcmm3);

            zmm0 = _mm512_sllv_epi32(zmm0, shift_masks_ptr[0]);
            zmm1 = _mm512_sllv_epi32(zmm1, shift_masks_ptr[1]);
            zmm2 = _mm512_srlv_epi32(zmm2, shift_masks_ptr[2]);
            zmm3 = _mm512_sllv_epi32(zmm3, shift_masks_ptr[3]);
            zmm4 = _mm512_sllv_epi32(zmm4, shift_masks_ptr[4]);
            zmm5 = _mm512_srlv_epi32(zmm5, shift_masks_ptr[5]);
            zmm6 = _mm512_sllv_epi32(zmm6, shift_masks_ptr[6]);
            zmm7 = _mm512_sllv_epi32(zmm7, shift_masks_ptr[7]);
            zmm8 = _mm512_srlv_epi32(zmm8, shift_masks_ptr[8]);

            zmm0 = _mm512_or_si512(zmm0, zmm1);
            zmm0 = _mm512_or_si512(zmm0, zmm2);
            zmm3 = _mm512_or_si512(zmm3, zmm4);
            zmm3 = _mm512_or_si512(zmm3, zmm5);
            zmm6 = _mm512_or_si512(zmm6, zmm7);
            zmm6 = _mm512_or_si512(zmm6, zmm8);

            _mm512_storeu_si512(dst_ptr, zmm0);
            _mm512_storeu_si512(dst_ptr + 64u, zmm3);
            _mm512_mask_storeu_epi16(dst_ptr + 128u, 0x0FFFFFFF, zmm6);

            src_ptr += 256u;
            dst_ptr += 23u * 8u;
        }

        for (uint32_t idx = 0; idx < num_elements_16; ++idx)
        {
            srcmm0 = _mm512_loadu_si512(src_ptr);
            zmm0 = _mm512_maskz_permutexvar_epi32(permutex_masks_23u_ptr[0], permutex_idx_ptr[0], srcmm0);
            zmm1 = _mm512_maskz_permutexvar_epi32(permutex_masks_23u_ptr[1], permutex_idx_ptr[1], srcmm0);
            zmm2 = _mm512_maskz_permutexvar_epi32(permutex_masks_23u_ptr[2], permutex_idx_ptr[2], srcmm0);

            zmm0 = _mm512_sllv_epi32(zmm0, shift_masks_ptr[0]);
            zmm1 = _mm512_sllv_epi32(zmm1, shift_masks_ptr[1]);
            zmm2 = _mm512_srlv_epi32(zmm2, shift_masks_ptr[2]);

            zmm0 = _mm512_or_si512(zmm0, zmm1);
            zmm0 = _mm512_or_si512(zmm0, zmm2);
            _mm512_mask_storeu_epi16(dst_ptr, 0x007FFFFF, zmm0);

            src_ptr += 64u;
            dst_ptr += 23u * 2u;
        }
    }

    if (tail > 0) {
        k0_qplc_pack_32u23u_tail(src_ptr, tail, dst_ptr);
    }
}

// ********************** 32u24u ****************************** //

OWN_QPLC_INLINE(void, px_qplc_pack_32u24u, (const uint8_t *src_ptr, uint32_t num_elements, uint8_t *dst_ptr, uint32_t start_bit))
{
    uint32_t bit_width = 24u;
    int32_t  bits_in_buf = (int32_t)(bit_width + start_bit);
    uint32_t *src_32u_ptr = (uint32_t *)src_ptr;
    uint32_t *dst_32u_ptr = (uint32_t *)dst_ptr;
    uint64_t src = (uint64_t)(*dst_32u_ptr) & OWN_BIT_MASK(start_bit);

    src |= ((uint64_t)(*src_32u_ptr)) << start_bit;
    src_32u_ptr++;
    num_elements--;

    while (0u < num_elements) {
        if (OWN_DWORD_WIDTH <= (uint32_t)bits_in_buf) {
            *dst_32u_ptr = (uint32_t)(src);
            dst_32u_ptr++;
            src = src >> OWN_DWORD_WIDTH;
            bits_in_buf -= OWN_DWORD_WIDTH;
        }
        src = src | (((uint64_t)(*src_32u_ptr)) << bits_in_buf);
        src_32u_ptr++;
        num_elements--;
        bits_in_buf += (int32_t)bit_width;
    }
    dst_ptr = (uint8_t *)dst_32u_ptr;
    while (0 < bits_in_buf) {
        *dst_ptr = (uint8_t)(src);
        bits_in_buf -= OWN_BYTE_WIDTH;
        dst_ptr++;
        src >>= OWN_BYTE_WIDTH;
    }
}


OWN_QPLC_INLINE(void, k0_qplc_pack_32u24u_tail, (const uint8_t *src_ptr, uint32_t num_elements, uint8_t *dst_ptr))
{
    __m512i srcmm0;
    __m512i zmm0, zmm1;

    __mmask16 tail_mask = OWN_BIT_MASK(num_elements);
    __mmask64 store_mask = OWN_BIT_MASK(OWN_BITS_2_BYTE(num_elements * 24u));

    __m512i permutex_idx_ptr[2];
    permutex_idx_ptr[0] = _mm512_load_si512(permutex_idx_table_24u_0);
    permutex_idx_ptr[1] = _mm512_load_si512(permutex_idx_table_24u_1);

    __m512i shift_masks_ptr[2];
    shift_masks_ptr[0] = _mm512_load_si512(shift_mask_table_24u_0);
    shift_masks_ptr[1] = _mm512_load_si512(shift_mask_table_24u_1);

    srcmm0 = _mm512_maskz_loadu_epi32(tail_mask, src_ptr);
    zmm0 = _mm512_permutexvar_epi32(permutex_idx_ptr[0], srcmm0);
    zmm1 = _mm512_permutexvar_epi32(permutex_idx_ptr[1], srcmm0);

    zmm0 = _mm512_sllv_epi32(zmm0, shift_masks_ptr[0]);
    zmm1 = _mm512_srlv_epi32(zmm1, shift_masks_ptr[1]);

    zmm0 = _mm512_or_si512(zmm0, zmm1);
    _mm512_mask_storeu_epi8(dst_ptr, store_mask, zmm0);
}

OWN_OPT_FUN(void, k0_qplc_pack_32u24u, (const uint8_t *src_ptr, uint32_t num_elements, uint8_t *dst_ptr, uint32_t start_bit))
{
    if (start_bit > 0u) {
        uint32_t align = own_get_align(start_bit, 24u, 32u);
        if (align > num_elements) {
            align = num_elements;
        }
        px_qplc_pack_32u24u(src_ptr, align, dst_ptr, start_bit);
        src_ptr += align * 4;
        dst_ptr += ((align * 24u) + start_bit) >> 3u;
        num_elements -= align;
    }

    uint32_t tail = num_elements % 16u;
    if (num_elements >= 16u)
    {
        uint32_t num_elements_64 = num_elements / 64u;
        uint32_t num_elements_16 = (num_elements % 64u) / 16u;
        __m512i srcmm0, srcmm1, srcmm2, srcmm3;
        __m512i zmm0, zmm1, zmm2, zmm3, zmm4, zmm5;

        __m512i permutex_idx_ptr[6];
        permutex_idx_ptr[0] = _mm512_load_si512(permutex_idx_table_24u_0);
        permutex_idx_ptr[1] = _mm512_load_si512(permutex_idx_table_24u_1);
        permutex_idx_ptr[2] = _mm512_load_si512(permutex_idx_table_24u_2);
        permutex_idx_ptr[3] = _mm512_load_si512(permutex_idx_table_24u_3);
        permutex_idx_ptr[4] = _mm512_load_si512(permutex_idx_table_24u_4);
        permutex_idx_ptr[5] = _mm512_load_si512(permutex_idx_table_24u_5);

        __m512i shift_masks_ptr[6];
        shift_masks_ptr[0] = _mm512_load_si512(shift_mask_table_24u_0);
        shift_masks_ptr[1] = _mm512_load_si512(shift_mask_table_24u_1);
        shift_masks_ptr[2] = _mm512_load_si512(shift_mask_table_24u_2);
        shift_masks_ptr[3] = _mm512_load_si512(shift_mask_table_24u_3);
        shift_masks_ptr[4] = _mm512_load_si512(shift_mask_table_24u_4);
        shift_masks_ptr[5] = _mm512_load_si512(shift_mask_table_24u_5);

        for (uint32_t idx = 0; idx < num_elements_64; ++idx)
        {
            srcmm0 = _mm512_loadu_si512(src_ptr);
            srcmm1 = _mm512_loadu_si512(src_ptr + 64u);
            srcmm2 = _mm512_loadu_si512(src_ptr + 128u);
            srcmm3 = _mm512_loadu_si512(src_ptr + 192u);

            zmm0 = _mm512_permutex2var_epi32(srcmm0, permutex_idx_ptr[0], srcmm1);
            zmm1 = _mm512_permutex2var_epi32(srcmm0, permutex_idx_ptr[1], srcmm1);
            zmm2 = _mm512_permutex2var_epi32(srcmm1, permutex_idx_ptr[2], srcmm2);
            zmm3 = _mm512_permutex2var_epi32(srcmm1, permutex_idx_ptr[3], srcmm2);
            zmm4 = _mm512_permutex2var_epi32(srcmm2, permutex_idx_ptr[4], srcmm3);
            zmm5 = _mm512_permutex2var_epi32(srcmm2, permutex_idx_ptr[5], srcmm3);

            zmm0 = _mm512_sllv_epi32(zmm0, shift_masks_ptr[0]);
            zmm1 = _mm512_srlv_epi32(zmm1, shift_masks_ptr[1]);
            zmm2 = _mm512_sllv_epi32(zmm2, shift_masks_ptr[2]);
            zmm3 = _mm512_srlv_epi32(zmm3, shift_masks_ptr[3]);
            zmm4 = _mm512_sllv_epi32(zmm4, shift_masks_ptr[4]);
            zmm5 = _mm512_srlv_epi32(zmm5, shift_masks_ptr[5]);

            zmm0 = _mm512_or_si512(zmm0, zmm1);
            zmm2 = _mm512_or_si512(zmm2, zmm3);
            zmm4 = _mm512_or_si512(zmm4, zmm5);

            _mm512_storeu_si512(dst_ptr, zmm0);
            _mm512_storeu_si512(dst_ptr + 64u, zmm2);
            _mm512_storeu_si512(dst_ptr + 128u, zmm4);

            src_ptr += 256u;
            dst_ptr += 24u * 8u;
        }

        for (uint32_t idx = 0; idx < num_elements_16; ++idx)
        {
            srcmm0 = _mm512_loadu_si512(src_ptr);
            zmm0 = _mm512_permutexvar_epi32(permutex_idx_ptr[0], srcmm0);
            zmm1 = _mm512_permutexvar_epi32(permutex_idx_ptr[1], srcmm0);

            zmm0 = _mm512_sllv_epi32(zmm0, shift_masks_ptr[0]);
            zmm1 = _mm512_srlv_epi32(zmm1, shift_masks_ptr[1]);

            zmm0 = _mm512_or_si512(zmm0, zmm1);
            _mm512_mask_storeu_epi16(dst_ptr, 0x00FFFFFF, zmm0);

            src_ptr += 64u;
            dst_ptr += 24u * 2u;
        }
    }

    if (tail > 0) {
        k0_qplc_pack_32u24u_tail(src_ptr, tail, dst_ptr);
    }
}

// ********************** 32u25u ****************************** //

OWN_QPLC_INLINE(void, px_qplc_pack_32u25u, (const uint8_t *src_ptr, uint32_t num_elements, uint8_t *dst_ptr, uint32_t start_bit))
{
    uint32_t bit_width = 25u;
    int32_t  bits_in_buf = (int32_t)(bit_width + start_bit);
    uint32_t *src_32u_ptr = (uint32_t *)src_ptr;
    uint32_t *dst_32u_ptr = (uint32_t *)dst_ptr;
    uint64_t src = (uint64_t)(*dst_32u_ptr) & OWN_BIT_MASK(start_bit);

    src |= ((uint64_t)(*src_32u_ptr)) << start_bit;
    src_32u_ptr++;
    num_elements--;

    while (0u < num_elements) {
        if (OWN_DWORD_WIDTH <= (uint32_t)bits_in_buf) {
            *dst_32u_ptr = (uint32_t)(src);
            dst_32u_ptr++;
            src = src >> OWN_DWORD_WIDTH;
            bits_in_buf -= OWN_DWORD_WIDTH;
        }
        src = src | (((uint64_t)(*src_32u_ptr)) << bits_in_buf);
        src_32u_ptr++;
        num_elements--;
        bits_in_buf += (int32_t)bit_width;
    }
    dst_ptr = (uint8_t *)dst_32u_ptr;
    while (0 < bits_in_buf) {
        *dst_ptr = (uint8_t)(src);
        bits_in_buf -= OWN_BYTE_WIDTH;
        dst_ptr++;
        src >>= OWN_BYTE_WIDTH;
    }
}

OWN_QPLC_INLINE(void, k0_qplc_pack_32u25u_tail, (const uint8_t *src_ptr, uint32_t num_elements, uint8_t *dst_ptr))
{
    __m512i srcmm0;
    __m512i zmm0, zmm1, zmm2;

    __mmask16 tail_mask = OWN_BIT_MASK(num_elements);
    __mmask64 store_mask = OWN_BIT_MASK(OWN_BITS_2_BYTE(num_elements * 25u));

    __m512i permutex_idx_ptr[3];
    permutex_idx_ptr[0] = _mm512_load_si512(permutex_idx_table_25u_0);
    permutex_idx_ptr[1] = _mm512_load_si512(permutex_idx_table_25u_1);
    permutex_idx_ptr[2] = _mm512_load_si512(permutex_idx_table_25u_2);

    __m512i shift_masks_ptr[3];
    shift_masks_ptr[0] = _mm512_load_si512(shift_mask_table_25u_0);
    shift_masks_ptr[1] = _mm512_load_si512(shift_mask_table_25u_1);
    shift_masks_ptr[2] = _mm512_load_si512(shift_mask_table_25u_2);

    srcmm0 = _mm512_maskz_loadu_epi32(tail_mask, src_ptr);
    zmm0 = _mm512_maskz_permutexvar_epi32(permutex_masks_25u_ptr[0], permutex_idx_ptr[0], srcmm0);
    zmm1 = _mm512_maskz_permutexvar_epi32(permutex_masks_25u_ptr[1], permutex_idx_ptr[1], srcmm0);
    zmm2 = _mm512_maskz_permutexvar_epi32(permutex_masks_25u_ptr[2], permutex_idx_ptr[2], srcmm0);

    zmm0 = _mm512_sllv_epi32(zmm0, shift_masks_ptr[0]);
    zmm1 = _mm512_sllv_epi32(zmm1, shift_masks_ptr[1]);
    zmm2 = _mm512_srlv_epi32(zmm2, shift_masks_ptr[2]);

    zmm0 = _mm512_or_si512(zmm0, zmm1);
    zmm0 = _mm512_or_si512(zmm0, zmm2);
    _mm512_mask_storeu_epi8(dst_ptr, store_mask, zmm0);
}

OWN_OPT_FUN(void, k0_qplc_pack_32u25u, (const uint8_t *src_ptr, uint32_t num_elements, uint8_t *dst_ptr, uint32_t start_bit))
{
    if (start_bit > 0u) {
        uint32_t align = own_get_align(start_bit, 25u, 32u);
        if (align > num_elements) {
            align = num_elements;
        }
        px_qplc_pack_32u25u(src_ptr, align, dst_ptr, start_bit);
        src_ptr += align * 4;
        dst_ptr += ((align * 25u) + start_bit) >> 3u;
        num_elements -= align;
    }

    uint32_t tail = num_elements % 16u;
    if (num_elements >= 16u)
    {
        uint32_t num_elements_16 = num_elements / 16u;
        __m512i srcmm0;
        __m512i zmm0, zmm1, zmm2;

        __m512i permutex_idx_ptr[3];
        permutex_idx_ptr[0] = _mm512_load_si512(permutex_idx_table_25u_0);
        permutex_idx_ptr[1] = _mm512_load_si512(permutex_idx_table_25u_1);
        permutex_idx_ptr[2] = _mm512_load_si512(permutex_idx_table_25u_2);

        __m512i shift_masks_ptr[3];
        shift_masks_ptr[0] = _mm512_load_si512(shift_mask_table_25u_0);
        shift_masks_ptr[1] = _mm512_load_si512(shift_mask_table_25u_1);
        shift_masks_ptr[2] = _mm512_load_si512(shift_mask_table_25u_2);

        for (uint32_t idx = 0; idx < num_elements_16; ++idx)
        {
            srcmm0 = _mm512_loadu_si512(src_ptr);
            zmm0 = _mm512_maskz_permutexvar_epi32(permutex_masks_25u_ptr[0], permutex_idx_ptr[0], srcmm0);
            zmm1 = _mm512_maskz_permutexvar_epi32(permutex_masks_25u_ptr[1], permutex_idx_ptr[1], srcmm0);
            zmm2 = _mm512_maskz_permutexvar_epi32(permutex_masks_25u_ptr[2], permutex_idx_ptr[2], srcmm0);

            zmm0 = _mm512_sllv_epi32(zmm0, shift_masks_ptr[0]);
            zmm1 = _mm512_sllv_epi32(zmm1, shift_masks_ptr[1]);
            zmm2 = _mm512_srlv_epi32(zmm2, shift_masks_ptr[2]);

            zmm0 = _mm512_or_si512(zmm0, zmm1);
            zmm0 = _mm512_or_si512(zmm0, zmm2);
            _mm512_mask_storeu_epi16(dst_ptr, 0x01FFFFFF, zmm0);

            src_ptr += 64u;
            dst_ptr += 25u * 2u;
        }
    }

    if (tail > 0) {
        k0_qplc_pack_32u25u_tail(src_ptr, tail, dst_ptr);
    }
}

// ********************** 32u26u ****************************** //

OWN_QPLC_INLINE(void, px_qplc_pack_32u26u, (const uint8_t *src_ptr, uint32_t num_elements, uint8_t *dst_ptr, uint32_t start_bit))
{
    uint32_t bit_width = 26u;
    int32_t  bits_in_buf = (int32_t)(bit_width + start_bit);
    uint32_t *src_32u_ptr = (uint32_t *)src_ptr;
    uint32_t *dst_32u_ptr = (uint32_t *)dst_ptr;
    uint64_t src = (uint64_t)(*dst_32u_ptr) & OWN_BIT_MASK(start_bit);

    src |= ((uint64_t)(*src_32u_ptr)) << start_bit;
    src_32u_ptr++;
    num_elements--;

    while (0u < num_elements) {
        if (OWN_DWORD_WIDTH <= (uint32_t)bits_in_buf) {
            *dst_32u_ptr = (uint32_t)(src);
            dst_32u_ptr++;
            src = src >> OWN_DWORD_WIDTH;
            bits_in_buf -= OWN_DWORD_WIDTH;
        }
        src = src | (((uint64_t)(*src_32u_ptr)) << bits_in_buf);
        src_32u_ptr++;
        num_elements--;
        bits_in_buf += (int32_t)bit_width;
    }
    dst_ptr = (uint8_t *)dst_32u_ptr;
    while (0 < bits_in_buf) {
        *dst_ptr = (uint8_t)(src);
        bits_in_buf -= OWN_BYTE_WIDTH;
        dst_ptr++;
        src >>= OWN_BYTE_WIDTH;
    }
}

OWN_QPLC_INLINE(void, k0_qplc_pack_32u26u_tail, (const uint8_t *src_ptr, uint32_t num_elements, uint8_t *dst_ptr))
{
    __m512i srcmm0;
    __m512i zmm0, zmm1, zmm2;

    __mmask16 tail_mask = OWN_BIT_MASK(num_elements);
    __mmask64 store_mask = OWN_BIT_MASK(OWN_BITS_2_BYTE(num_elements * 26u));

    __m512i permutex_idx_ptr[3];
    permutex_idx_ptr[0] = _mm512_load_si512(permutex_idx_table_26u_0);
    permutex_idx_ptr[1] = _mm512_load_si512(permutex_idx_table_26u_1);
    permutex_idx_ptr[2] = _mm512_load_si512(permutex_idx_table_26u_2);

    __m512i shift_masks_ptr[3];
    shift_masks_ptr[0] = _mm512_load_si512(shift_mask_table_26u_0);
    shift_masks_ptr[1] = _mm512_load_si512(shift_mask_table_26u_1);
    shift_masks_ptr[2] = _mm512_load_si512(shift_mask_table_26u_2);

    srcmm0 = _mm512_maskz_loadu_epi32(tail_mask, src_ptr);
    zmm0 = _mm512_maskz_permutexvar_epi32(permutex_masks_26u_ptr[0], permutex_idx_ptr[0], srcmm0);
    zmm1 = _mm512_maskz_permutexvar_epi32(permutex_masks_26u_ptr[1], permutex_idx_ptr[1], srcmm0);
    zmm2 = _mm512_maskz_permutexvar_epi32(permutex_masks_26u_ptr[2], permutex_idx_ptr[2], srcmm0);

    zmm0 = _mm512_sllv_epi32(zmm0, shift_masks_ptr[0]);
    zmm1 = _mm512_sllv_epi32(zmm1, shift_masks_ptr[1]);
    zmm2 = _mm512_srlv_epi32(zmm2, shift_masks_ptr[2]);

    zmm0 = _mm512_or_si512(zmm0, zmm1);
    zmm0 = _mm512_or_si512(zmm0, zmm2);
    _mm512_mask_storeu_epi8(dst_ptr, store_mask, zmm0);
}

OWN_OPT_FUN(void, k0_qplc_pack_32u26u, (const uint8_t *src_ptr, uint32_t num_elements, uint8_t *dst_ptr, uint32_t start_bit))
{
    if (start_bit > 0u) {
        uint32_t align = own_get_align(start_bit, 26u, 32u);
        if (align > num_elements) {
            align = num_elements;
        }
        px_qplc_pack_32u26u(src_ptr, align, dst_ptr, start_bit);
        src_ptr += align * 4;
        dst_ptr += ((align * 26u) + start_bit) >> 3u;
        num_elements -= align;
    }

    uint32_t tail = num_elements % 16u;
    if (num_elements >= 16u)
    {
        uint32_t num_elements_16 = num_elements / 16u;
        __m512i srcmm0;
        __m512i zmm0, zmm1, zmm2;

        __m512i permutex_idx_ptr[3];
        permutex_idx_ptr[0] = _mm512_load_si512(permutex_idx_table_26u_0);
        permutex_idx_ptr[1] = _mm512_load_si512(permutex_idx_table_26u_1);
        permutex_idx_ptr[2] = _mm512_load_si512(permutex_idx_table_26u_2);

        __m512i shift_masks_ptr[3];
        shift_masks_ptr[0] = _mm512_load_si512(shift_mask_table_26u_0);
        shift_masks_ptr[1] = _mm512_load_si512(shift_mask_table_26u_1);
        shift_masks_ptr[2] = _mm512_load_si512(shift_mask_table_26u_2);

        for (uint32_t idx = 0; idx < num_elements_16; ++idx)
        {
            srcmm0 = _mm512_loadu_si512(src_ptr);
            zmm0 = _mm512_maskz_permutexvar_epi32(permutex_masks_26u_ptr[0], permutex_idx_ptr[0], srcmm0);
            zmm1 = _mm512_maskz_permutexvar_epi32(permutex_masks_26u_ptr[1], permutex_idx_ptr[1], srcmm0);
            zmm2 = _mm512_maskz_permutexvar_epi32(permutex_masks_26u_ptr[2], permutex_idx_ptr[2], srcmm0);

            zmm0 = _mm512_sllv_epi32(zmm0, shift_masks_ptr[0]);
            zmm1 = _mm512_sllv_epi32(zmm1, shift_masks_ptr[1]);
            zmm2 = _mm512_srlv_epi32(zmm2, shift_masks_ptr[2]);

            zmm0 = _mm512_or_si512(zmm0, zmm1);
            zmm0 = _mm512_or_si512(zmm0, zmm2);
            _mm512_mask_storeu_epi16(dst_ptr, 0x03FFFFFF, zmm0);

            src_ptr += 64u;
            dst_ptr += 26u * 2u;
        }
    }

    if (tail > 0) {
        k0_qplc_pack_32u26u_tail(src_ptr, tail, dst_ptr);
    }
}

// ********************** 32u27u ****************************** //

OWN_QPLC_INLINE(void, px_qplc_pack_32u27u, (const uint8_t *src_ptr, uint32_t num_elements, uint8_t *dst_ptr, uint32_t start_bit))
{
    uint32_t bit_width = 27u;
    int32_t  bits_in_buf = (int32_t)(bit_width + start_bit);
    uint32_t *src_32u_ptr = (uint32_t *)src_ptr;
    uint32_t *dst_32u_ptr = (uint32_t *)dst_ptr;
    uint64_t src = (uint64_t)(*dst_32u_ptr) & OWN_BIT_MASK(start_bit);

    src |= ((uint64_t)(*src_32u_ptr)) << start_bit;
    src_32u_ptr++;
    num_elements--;

    while (0u < num_elements) {
        if (OWN_DWORD_WIDTH <= (uint32_t)bits_in_buf) {
            *dst_32u_ptr = (uint32_t)(src);
            dst_32u_ptr++;
            src = src >> OWN_DWORD_WIDTH;
            bits_in_buf -= OWN_DWORD_WIDTH;
        }
        src = src | (((uint64_t)(*src_32u_ptr)) << bits_in_buf);
        src_32u_ptr++;
        num_elements--;
        bits_in_buf += (int32_t)bit_width;
    }
    dst_ptr = (uint8_t *)dst_32u_ptr;
    while (0 < bits_in_buf) {
        *dst_ptr = (uint8_t)(src);
        bits_in_buf -= OWN_BYTE_WIDTH;
        dst_ptr++;
        src >>= OWN_BYTE_WIDTH;
    }
}

OWN_QPLC_INLINE(void, k0_qplc_pack_32u27u_tail, (const uint8_t *src_ptr, uint32_t num_elements, uint8_t *dst_ptr))
{
    __m512i srcmm0;
    __m512i zmm0, zmm1, zmm2;

    __mmask16 tail_mask = OWN_BIT_MASK(num_elements);
    __mmask64 store_mask = OWN_BIT_MASK(OWN_BITS_2_BYTE(num_elements * 27u));

    __m512i permutex_idx_ptr[3];
    permutex_idx_ptr[0] = _mm512_load_si512(permutex_idx_table_27u_0);
    permutex_idx_ptr[1] = _mm512_load_si512(permutex_idx_table_27u_1);
    permutex_idx_ptr[2] = _mm512_load_si512(permutex_idx_table_27u_2);

    __m512i shift_masks_ptr[3];
    shift_masks_ptr[0] = _mm512_load_si512(shift_mask_table_27u_0);
    shift_masks_ptr[1] = _mm512_load_si512(shift_mask_table_27u_1);
    shift_masks_ptr[2] = _mm512_load_si512(shift_mask_table_27u_2);

    srcmm0 = _mm512_maskz_loadu_epi32(tail_mask, src_ptr);
    zmm0 = _mm512_maskz_permutexvar_epi32(permutex_masks_27u_ptr[0], permutex_idx_ptr[0], srcmm0);
    zmm1 = _mm512_maskz_permutexvar_epi32(permutex_masks_27u_ptr[1], permutex_idx_ptr[1], srcmm0);
    zmm2 = _mm512_maskz_permutexvar_epi32(permutex_masks_27u_ptr[2], permutex_idx_ptr[2], srcmm0);

    zmm0 = _mm512_sllv_epi32(zmm0, shift_masks_ptr[0]);
    zmm1 = _mm512_sllv_epi32(zmm1, shift_masks_ptr[1]);
    zmm2 = _mm512_srlv_epi32(zmm2, shift_masks_ptr[2]);

    zmm0 = _mm512_or_si512(zmm0, zmm1);
    zmm0 = _mm512_or_si512(zmm0, zmm2);
    _mm512_mask_storeu_epi8(dst_ptr, store_mask, zmm0);
}

OWN_OPT_FUN(void, k0_qplc_pack_32u27u, (const uint8_t *src_ptr, uint32_t num_elements, uint8_t *dst_ptr, uint32_t start_bit))
{
    if (start_bit > 0u) {
        uint32_t align = own_get_align(start_bit, 27u, 32u);
        if (align > num_elements) {
            align = num_elements;
        }
        px_qplc_pack_32u27u(src_ptr, align, dst_ptr, start_bit);
        src_ptr += align * 4;
        dst_ptr += ((align * 27u) + start_bit) >> 3u;
        num_elements -= align;
    }

    uint32_t tail = num_elements % 16u;
    if (num_elements >= 16u)
    {
        uint32_t num_elements_16 = num_elements  / 16u;
        __m512i srcmm0;
        __m512i zmm0, zmm1, zmm2;

        __m512i permutex_idx_ptr[3];
        permutex_idx_ptr[0] = _mm512_load_si512(permutex_idx_table_27u_0);
        permutex_idx_ptr[1] = _mm512_load_si512(permutex_idx_table_27u_1);
        permutex_idx_ptr[2] = _mm512_load_si512(permutex_idx_table_27u_2);

        __m512i shift_masks_ptr[3];
        shift_masks_ptr[0] = _mm512_load_si512(shift_mask_table_27u_0);
        shift_masks_ptr[1] = _mm512_load_si512(shift_mask_table_27u_1);
        shift_masks_ptr[2] = _mm512_load_si512(shift_mask_table_27u_2);

        for (uint32_t idx = 0; idx < num_elements_16; ++idx)
        {
            srcmm0 = _mm512_loadu_si512(src_ptr);
            zmm0 = _mm512_maskz_permutexvar_epi32(permutex_masks_27u_ptr[0], permutex_idx_ptr[0], srcmm0);
            zmm1 = _mm512_maskz_permutexvar_epi32(permutex_masks_27u_ptr[1], permutex_idx_ptr[1], srcmm0);
            zmm2 = _mm512_maskz_permutexvar_epi32(permutex_masks_27u_ptr[2], permutex_idx_ptr[2], srcmm0);

            zmm0 = _mm512_sllv_epi32(zmm0, shift_masks_ptr[0]);
            zmm1 = _mm512_sllv_epi32(zmm1, shift_masks_ptr[1]);
            zmm2 = _mm512_srlv_epi32(zmm2, shift_masks_ptr[2]);

            zmm0 = _mm512_or_si512(zmm0, zmm1);
            zmm0 = _mm512_or_si512(zmm0, zmm2);
            _mm512_mask_storeu_epi16(dst_ptr, 0x07FFFFFF, zmm0);

            src_ptr += 64u;
            dst_ptr += 27u * 2u;
        }
    }

    if (tail > 0) {
        k0_qplc_pack_32u27u_tail(src_ptr, tail, dst_ptr);
    }
}

// ********************** 32u28u ****************************** //

OWN_QPLC_INLINE(void, px_qplc_pack_32u28u, (const uint8_t *src_ptr, uint32_t num_elements, uint8_t *dst_ptr, uint32_t start_bit))
{
    uint32_t bit_width = 28u;
    int32_t  bits_in_buf = (int32_t)(bit_width + start_bit);
    uint32_t *src_32u_ptr = (uint32_t *)src_ptr;
    uint32_t *dst_32u_ptr = (uint32_t *)dst_ptr;
    uint64_t src = (uint64_t)(*dst_32u_ptr) & OWN_BIT_MASK(start_bit);

    src |= ((uint64_t)(*src_32u_ptr)) << start_bit;
    src_32u_ptr++;
    num_elements--;

    while (0u < num_elements) {
        if (OWN_DWORD_WIDTH <= (uint32_t)bits_in_buf) {
            *dst_32u_ptr = (uint32_t)(src);
            dst_32u_ptr++;
            src = src >> OWN_DWORD_WIDTH;
            bits_in_buf -= OWN_DWORD_WIDTH;
        }
        src = src | (((uint64_t)(*src_32u_ptr)) << bits_in_buf);
        src_32u_ptr++;
        num_elements--;
        bits_in_buf += (int32_t)bit_width;
    }
    dst_ptr = (uint8_t *)dst_32u_ptr;
    while (0 < bits_in_buf) {
        *dst_ptr = (uint8_t)(src);
        bits_in_buf -= OWN_BYTE_WIDTH;
        dst_ptr++;
        src >>= OWN_BYTE_WIDTH;
    }
}

#if defined _MSC_VER
#if _MSC_VER <= 1916
// if MSVC <= MSVC2017
// There is the problem with compiler of MSVC2017.
#pragma optimize("", off)
#endif
#endif

OWN_QPLC_INLINE(void, k0_qplc_pack_32u28u_tail, (const uint8_t *src_ptr, uint32_t num_elements, uint8_t *dst_ptr))
{
    __m512i srcmm0;
    __m512i zmm0, zmm1;

    __mmask16 tail_mask = OWN_BIT_MASK(num_elements);
    __mmask64 store_mask = OWN_BIT_MASK(OWN_BITS_2_BYTE(num_elements * 28u));

    __m512i permutex_idx_ptr[2];
    permutex_idx_ptr[0] = _mm512_load_si512(permutex_idx_table_28u_0);
    permutex_idx_ptr[1] = _mm512_load_si512(permutex_idx_table_28u_1);

    __m512i shift_masks_ptr[2];
    shift_masks_ptr[0] = _mm512_load_si512(shift_mask_table_28u_0);
    shift_masks_ptr[1] = _mm512_load_si512(shift_mask_table_28u_1);

    srcmm0 = _mm512_maskz_loadu_epi32(tail_mask, src_ptr);
    zmm0 = _mm512_maskz_permutexvar_epi32(permutex_masks_28u_ptr[0], permutex_idx_ptr[0], srcmm0);
    zmm1 = _mm512_maskz_permutexvar_epi32(permutex_masks_28u_ptr[1], permutex_idx_ptr[1], srcmm0);

    zmm0 = _mm512_sllv_epi32(zmm0, shift_masks_ptr[0]);
    zmm1 = _mm512_srlv_epi32(zmm1, shift_masks_ptr[1]);

    zmm0 = _mm512_or_si512(zmm0, zmm1);
    _mm512_mask_storeu_epi8(dst_ptr, store_mask, zmm0);
}

#if defined _MSC_VER
#if _MSC_VER <= 1916
// if MSVC <= MSVC2017
#pragma optimize("", on)
#endif
#endif

OWN_OPT_FUN(void, k0_qplc_pack_32u28u, (const uint8_t *src_ptr, uint32_t num_elements, uint8_t *dst_ptr, uint32_t start_bit))
{
    if (start_bit > 0u) {
        uint32_t align = own_get_align(start_bit, 28u, 32u);
        if (align > num_elements) {
            align = num_elements;
        }
        px_qplc_pack_32u28u(src_ptr, align, dst_ptr, start_bit);
        src_ptr += align * 4;
        dst_ptr += ((align * 28u) + start_bit) >> 3u;
        num_elements -= align;
    }

    uint32_t tail = num_elements % 16u;
    if (num_elements >= 16u)
    {
        uint32_t num_elements_16 = num_elements / 16u;
        __m512i srcmm0;
        __m512i zmm0, zmm1;

        __m512i permutex_idx_ptr[3];
        permutex_idx_ptr[0] = _mm512_load_si512(permutex_idx_table_28u_0);
        permutex_idx_ptr[1] = _mm512_load_si512(permutex_idx_table_28u_1);

        __m512i shift_masks_ptr[3];
        shift_masks_ptr[0] = _mm512_load_si512(shift_mask_table_28u_0);
        shift_masks_ptr[1] = _mm512_load_si512(shift_mask_table_28u_1);

        for (uint32_t idx = 0; idx < num_elements_16; ++idx)
        {
            srcmm0 = _mm512_loadu_si512(src_ptr);
            zmm0 = _mm512_maskz_permutexvar_epi32(permutex_masks_28u_ptr[0], permutex_idx_ptr[0], srcmm0);
            zmm1 = _mm512_maskz_permutexvar_epi32(permutex_masks_28u_ptr[1], permutex_idx_ptr[1], srcmm0);

            zmm0 = _mm512_sllv_epi32(zmm0, shift_masks_ptr[0]);
            zmm1 = _mm512_srlv_epi32(zmm1, shift_masks_ptr[1]);

            zmm0 = _mm512_or_si512(zmm0, zmm1);
            _mm512_mask_storeu_epi16(dst_ptr, 0x0FFFFFFF, zmm0);

            src_ptr += 64u;
            dst_ptr += 28u * 2u;
        }
    }

    if (tail > 0) {
        k0_qplc_pack_32u28u_tail(src_ptr, tail, dst_ptr);
    }
}

// ********************** 32u29u ****************************** //

OWN_QPLC_INLINE(void, px_qplc_pack_32u29u, (const uint8_t *src_ptr, uint32_t num_elements, uint8_t *dst_ptr, uint32_t start_bit))
{
    uint32_t bit_width = 29u;
    int32_t  bits_in_buf = (int32_t)(bit_width + start_bit);
    uint32_t *src_32u_ptr = (uint32_t *)src_ptr;
    uint32_t *dst_32u_ptr = (uint32_t *)dst_ptr;
    uint64_t src = (uint64_t)(*dst_32u_ptr) & OWN_BIT_MASK(start_bit);

    src |= ((uint64_t)(*src_32u_ptr)) << start_bit;
    src_32u_ptr++;
    num_elements--;

    while (0u < num_elements) {
        if (OWN_DWORD_WIDTH <= (uint32_t)bits_in_buf) {
            *dst_32u_ptr = (uint32_t)(src);
            dst_32u_ptr++;
            src = src >> OWN_DWORD_WIDTH;
            bits_in_buf -= OWN_DWORD_WIDTH;
        }
        src = src | (((uint64_t)(*src_32u_ptr)) << bits_in_buf);
        src_32u_ptr++;
        num_elements--;
        bits_in_buf += (int32_t)bit_width;
    }
    dst_ptr = (uint8_t *)dst_32u_ptr;
    while (0 < bits_in_buf) {
        *dst_ptr = (uint8_t)(src);
        bits_in_buf -= OWN_BYTE_WIDTH;
        dst_ptr++;
        src >>= OWN_BYTE_WIDTH;
    }
}

OWN_QPLC_INLINE(void, k0_qplc_pack_32u29u_tail, (const uint8_t *src_ptr, uint32_t num_elements, uint8_t *dst_ptr))
{
    __m512i srcmm0;
    __m512i zmm0, zmm1, zmm2;

    __mmask16 tail_mask = OWN_BIT_MASK(num_elements);
    __mmask64 store_mask = OWN_BIT_MASK(OWN_BITS_2_BYTE(num_elements * 29u));

    __m512i permutex_idx_ptr[3];
    permutex_idx_ptr[0] = _mm512_load_si512(permutex_idx_table_29u_0);
    permutex_idx_ptr[1] = _mm512_load_si512(permutex_idx_table_29u_1);
    permutex_idx_ptr[2] = _mm512_load_si512(permutex_idx_table_29u_2);

    __m512i shift_masks_ptr[3];
    shift_masks_ptr[0] = _mm512_load_si512(shift_mask_table_29u_0);
    shift_masks_ptr[1] = _mm512_load_si512(shift_mask_table_29u_1);
    shift_masks_ptr[2] = _mm512_load_si512(shift_mask_table_29u_2);

    srcmm0 = _mm512_maskz_loadu_epi32(tail_mask, src_ptr);
    zmm0 = _mm512_maskz_permutexvar_epi32(permutex_masks_29u_ptr[0], permutex_idx_ptr[0], srcmm0);
    zmm1 = _mm512_maskz_permutexvar_epi32(permutex_masks_29u_ptr[1], permutex_idx_ptr[1], srcmm0);
    zmm2 = _mm512_maskz_permutexvar_epi32(permutex_masks_29u_ptr[2], permutex_idx_ptr[2], srcmm0);

    zmm0 = _mm512_sllv_epi32(zmm0, shift_masks_ptr[0]);
    zmm1 = _mm512_sllv_epi32(zmm1, shift_masks_ptr[1]);
    zmm2 = _mm512_srlv_epi32(zmm2, shift_masks_ptr[2]);

    zmm0 = _mm512_or_si512(zmm0, zmm1);
    zmm0 = _mm512_or_si512(zmm0, zmm2);
    _mm512_mask_storeu_epi8(dst_ptr, store_mask, zmm0);
}

OWN_OPT_FUN(void, k0_qplc_pack_32u29u, (const uint8_t *src_ptr, uint32_t num_elements, uint8_t *dst_ptr, uint32_t start_bit))
{
    if (start_bit > 0u) {
        uint32_t align = own_get_align(start_bit, 29u, 32u);
        if (align > num_elements) {
            align = num_elements;
        }
        px_qplc_pack_32u29u(src_ptr, align, dst_ptr, start_bit);
        src_ptr += align * 4;
        dst_ptr += ((align * 29u) + start_bit) >> 3u;
        num_elements -= align;
    }

    uint32_t tail = num_elements % 16u;
    if (num_elements >= 16u)
    {
        uint32_t num_elements_16 = num_elements / 16u;
        __m512i srcmm0;
        __m512i zmm0, zmm1, zmm2;

        __m512i permutex_idx_ptr[3];
        permutex_idx_ptr[0] = _mm512_load_si512(permutex_idx_table_29u_0);
        permutex_idx_ptr[1] = _mm512_load_si512(permutex_idx_table_29u_1);
        permutex_idx_ptr[2] = _mm512_load_si512(permutex_idx_table_29u_2);

        __m512i shift_masks_ptr[3];
        shift_masks_ptr[0] = _mm512_load_si512(shift_mask_table_29u_0);
        shift_masks_ptr[1] = _mm512_load_si512(shift_mask_table_29u_1);
        shift_masks_ptr[2] = _mm512_load_si512(shift_mask_table_29u_2);

        for (uint32_t idx = 0; idx < num_elements_16; ++idx)
        {
            srcmm0 = _mm512_loadu_si512(src_ptr);
            zmm0 = _mm512_maskz_permutexvar_epi32(permutex_masks_29u_ptr[0], permutex_idx_ptr[0], srcmm0);
            zmm1 = _mm512_maskz_permutexvar_epi32(permutex_masks_29u_ptr[1], permutex_idx_ptr[1], srcmm0);
            zmm2 = _mm512_maskz_permutexvar_epi32(permutex_masks_29u_ptr[2], permutex_idx_ptr[2], srcmm0);

            zmm0 = _mm512_sllv_epi32(zmm0, shift_masks_ptr[0]);
            zmm1 = _mm512_sllv_epi32(zmm1, shift_masks_ptr[1]);
            zmm2 = _mm512_srlv_epi32(zmm2, shift_masks_ptr[2]);

            zmm0 = _mm512_or_si512(zmm0, zmm1);
            zmm0 = _mm512_or_si512(zmm0, zmm2);
            _mm512_mask_storeu_epi16(dst_ptr, 0x1FFFFFFF, zmm0);

            src_ptr += 64u;
            dst_ptr += 29u * 2u;
        }
    }

    if (tail > 0) {
        k0_qplc_pack_32u29u_tail(src_ptr, tail, dst_ptr);
    }
}

// ********************** 32u30u ****************************** //

OWN_QPLC_INLINE(void, px_qplc_pack_32u30u, (const uint8_t *src_ptr, uint32_t num_elements, uint8_t *dst_ptr, uint32_t start_bit))
{
    uint32_t bit_width = 30u;
    int32_t  bits_in_buf = (int32_t)(bit_width + start_bit);
    uint32_t *src_32u_ptr = (uint32_t *)src_ptr;
    uint32_t *dst_32u_ptr = (uint32_t *)dst_ptr;
    uint64_t src = (uint64_t)(*dst_32u_ptr) & OWN_BIT_MASK(start_bit);

    src |= ((uint64_t)(*src_32u_ptr)) << start_bit;
    src_32u_ptr++;
    num_elements--;

    while (0u < num_elements) {
        if (OWN_DWORD_WIDTH <= (uint32_t)bits_in_buf) {
            *dst_32u_ptr = (uint32_t)(src);
            dst_32u_ptr++;
            src = src >> OWN_DWORD_WIDTH;
            bits_in_buf -= OWN_DWORD_WIDTH;
        }
        src = src | (((uint64_t)(*src_32u_ptr)) << bits_in_buf);
        src_32u_ptr++;
        num_elements--;
        bits_in_buf += (int32_t)bit_width;
    }
    dst_ptr = (uint8_t *)dst_32u_ptr;
    while (0 < bits_in_buf) {
        *dst_ptr = (uint8_t)(src);
        bits_in_buf -= OWN_BYTE_WIDTH;
        dst_ptr++;
        src >>= OWN_BYTE_WIDTH;
    }
}

#if defined _MSC_VER
#if _MSC_VER <= 1916
// if MSVC <= MSVC2017
// There is the problem with compiler of MSVC2017.
#pragma optimize("", off)
#endif
#endif

OWN_QPLC_INLINE(void, k0_qplc_pack_32u30u_tail, (const uint8_t *src_ptr, uint32_t num_elements, uint8_t *dst_ptr))
{
    __m512i srcmm0;
    __m512i zmm0, zmm1;

    __mmask16 tail_mask = OWN_BIT_MASK(num_elements);
    __mmask64 store_mask = OWN_BIT_MASK(OWN_BITS_2_BYTE(num_elements * 30u));

    __m512i permutex_idx_ptr[2];
    permutex_idx_ptr[0] = _mm512_load_si512(permutex_idx_table_30u_0);
    permutex_idx_ptr[1] = _mm512_load_si512(permutex_idx_table_30u_1);

    __m512i shift_masks_ptr[2];
    shift_masks_ptr[0] = _mm512_load_si512(shift_mask_table_30u_0);
    shift_masks_ptr[1] = _mm512_load_si512(shift_mask_table_30u_1);

    srcmm0 = _mm512_maskz_loadu_epi32(tail_mask, src_ptr);
    zmm0 = _mm512_maskz_permutexvar_epi32(permutex_masks_30u_ptr[0], permutex_idx_ptr[0], srcmm0);
    zmm1 = _mm512_maskz_permutexvar_epi32(permutex_masks_30u_ptr[1], permutex_idx_ptr[1], srcmm0);

    zmm0 = _mm512_sllv_epi32(zmm0, shift_masks_ptr[0]);
    zmm1 = _mm512_srlv_epi32(zmm1, shift_masks_ptr[1]);

    zmm0 = _mm512_or_si512(zmm0, zmm1);
    _mm512_mask_storeu_epi8(dst_ptr, store_mask, zmm0);
}

#if defined _MSC_VER
#if _MSC_VER <= 1916
// if MSVC <= MSVC2017
#pragma optimize("", on)
#endif
#endif

OWN_OPT_FUN(void, k0_qplc_pack_32u30u, (const uint8_t *src_ptr, uint32_t num_elements, uint8_t *dst_ptr, uint32_t start_bit))
{
    if (start_bit > 0u) {
        uint32_t align = own_get_align(start_bit, 30u, 32u);
        if (align > num_elements) {
            align = num_elements;
        }
        px_qplc_pack_32u30u(src_ptr, align, dst_ptr, start_bit);
        src_ptr += align * 4;
        dst_ptr += ((align * 30u) + start_bit) >> 3u;
        num_elements -= align;
    }

    uint32_t tail = num_elements % 16u;
    if (num_elements >= 16u)
    {
        uint32_t num_elements_16 = num_elements / 16u;
        __m512i srcmm0;
        __m512i zmm0, zmm1;

        __m512i permutex_idx_ptr[3];
        permutex_idx_ptr[0] = _mm512_load_si512(permutex_idx_table_30u_0);
        permutex_idx_ptr[1] = _mm512_load_si512(permutex_idx_table_30u_1);

        __m512i shift_masks_ptr[3];
        shift_masks_ptr[0] = _mm512_load_si512(shift_mask_table_30u_0);
        shift_masks_ptr[1] = _mm512_load_si512(shift_mask_table_30u_1);

        for (uint32_t idx = 0; idx < num_elements_16; ++idx)
        {
            srcmm0 = _mm512_loadu_si512(src_ptr);
            zmm0 = _mm512_maskz_permutexvar_epi32(permutex_masks_30u_ptr[0], permutex_idx_ptr[0], srcmm0);
            zmm1 = _mm512_maskz_permutexvar_epi32(permutex_masks_30u_ptr[1], permutex_idx_ptr[1], srcmm0);

            zmm0 = _mm512_sllv_epi32(zmm0, shift_masks_ptr[0]);
            zmm1 = _mm512_srlv_epi32(zmm1, shift_masks_ptr[1]);

            zmm0 = _mm512_or_si512(zmm0, zmm1);
            _mm512_mask_storeu_epi16(dst_ptr, 0x3FFFFFFF, zmm0);

            src_ptr += 64u;
            dst_ptr += 30u * 2u;
        }
    }

    if (tail > 0) {
        k0_qplc_pack_32u30u_tail(src_ptr, tail, dst_ptr);
    }
}

// ********************** 32u31u ****************************** //

OWN_QPLC_INLINE(void, px_qplc_pack_32u31u, (const uint8_t *src_ptr, uint32_t num_elements, uint8_t *dst_ptr, uint32_t start_bit))
{
    uint32_t bit_width = 31u;
    int32_t  bits_in_buf = (int32_t)(bit_width + start_bit);
    uint32_t *src_32u_ptr = (uint32_t *)src_ptr;
    uint32_t *dst_32u_ptr = (uint32_t *)dst_ptr;
    uint64_t src = (uint64_t)(*dst_32u_ptr) & OWN_BIT_MASK(start_bit);

    src |= ((uint64_t)(*src_32u_ptr)) << start_bit;
    src_32u_ptr++;
    num_elements--;

    while (0u < num_elements) {
        if (OWN_DWORD_WIDTH <= (uint32_t)bits_in_buf) {
            *dst_32u_ptr = (uint32_t)(src);
            dst_32u_ptr++;
            src = src >> OWN_DWORD_WIDTH;
            bits_in_buf -= OWN_DWORD_WIDTH;
        }
        src = src | (((uint64_t)(*src_32u_ptr)) << bits_in_buf);
        src_32u_ptr++;
        num_elements--;
        bits_in_buf += (int32_t)bit_width;
    }
    dst_ptr = (uint8_t *)dst_32u_ptr;
    while (0 < bits_in_buf) {
        *dst_ptr = (uint8_t)(src);
        bits_in_buf -= OWN_BYTE_WIDTH;
        dst_ptr++;
        src >>= OWN_BYTE_WIDTH;
    }
}

#if defined _MSC_VER
#if _MSC_VER <= 1916
// if MSVC <= MSVC2017
// There is the problem with compiler of MSVC2017.
#pragma optimize("", off)
#endif
#endif

OWN_QPLC_INLINE(void, k0_qplc_pack_32u31u_tail, (const uint8_t *src_ptr, uint32_t num_elements, uint8_t *dst_ptr))
{
    __m512i srcmm0;
    __m512i zmm0, zmm1;

    __mmask16 tail_mask = OWN_BIT_MASK(num_elements);
    __mmask64 store_mask = OWN_BIT_MASK(OWN_BITS_2_BYTE(num_elements * 31u));

    __m512i permutex_idx_ptr[2];
    permutex_idx_ptr[0] = _mm512_load_si512(permutex_idx_table_31u_0);
    permutex_idx_ptr[1] = _mm512_load_si512(permutex_idx_table_31u_1);

    __m512i shift_masks_ptr[2];
    shift_masks_ptr[0] = _mm512_load_si512(shift_mask_table_31u_0);
    shift_masks_ptr[1] = _mm512_load_si512(shift_mask_table_31u_1);

    srcmm0 = _mm512_maskz_loadu_epi32(tail_mask, src_ptr);
    zmm0 = _mm512_maskz_permutexvar_epi32(permutex_masks_31u_ptr[0], permutex_idx_ptr[0], srcmm0);
    zmm1 = _mm512_permutexvar_epi32(permutex_idx_ptr[1], srcmm0);

    zmm0 = _mm512_sllv_epi32(zmm0, shift_masks_ptr[0]);
    zmm1 = _mm512_srlv_epi32(zmm1, shift_masks_ptr[1]);

    zmm0 = _mm512_or_si512(zmm0, zmm1);
    _mm512_mask_storeu_epi8(dst_ptr, store_mask, zmm0);
}

#if defined _MSC_VER
#if _MSC_VER <= 1916
// if MSVC <= MSVC2017
#pragma optimize("", on)
#endif
#endif

OWN_OPT_FUN(void, k0_qplc_pack_32u31u, (const uint8_t *src_ptr, uint32_t num_elements, uint8_t *dst_ptr, uint32_t start_bit))
{
    if (start_bit > 0u) {
        uint32_t align = own_get_align(start_bit, 31u, 32u);
        if (align > num_elements) {
            align = num_elements;
        }
        px_qplc_pack_32u31u(src_ptr, align, dst_ptr, start_bit);
        src_ptr += align * 4;
        dst_ptr += ((align * 31u) + start_bit) >> 3u;
        num_elements -= align;
    }

    uint32_t tail = num_elements % 16u;
    if (num_elements >= 16u)
    {
        uint32_t num_elements_16 = num_elements / 16u;
        __m512i srcmm0;
        __m512i zmm0, zmm1;

        __m512i permutex_idx_ptr[3];
        permutex_idx_ptr[0] = _mm512_load_si512(permutex_idx_table_31u_0);
        permutex_idx_ptr[1] = _mm512_load_si512(permutex_idx_table_31u_1);

        __m512i shift_masks_ptr[3];
        shift_masks_ptr[0] = _mm512_load_si512(shift_mask_table_31u_0);
        shift_masks_ptr[1] = _mm512_load_si512(shift_mask_table_31u_1);

        for (uint32_t idx = 0; idx < num_elements_16; ++idx)
        {
            srcmm0 = _mm512_loadu_si512(src_ptr);
            zmm0 = _mm512_maskz_permutexvar_epi32(permutex_masks_31u_ptr[0], permutex_idx_ptr[0], srcmm0);
            zmm1 = _mm512_permutexvar_epi32(permutex_idx_ptr[1], srcmm0);

            zmm0 = _mm512_sllv_epi32(zmm0, shift_masks_ptr[0]);
            zmm1 = _mm512_srlv_epi32(zmm1, shift_masks_ptr[1]);

            zmm0 = _mm512_or_si512(zmm0, zmm1);
            _mm512_mask_storeu_epi16(dst_ptr, 0x7FFFFFFF, zmm0);

            src_ptr += 64u;
            dst_ptr += 31u * 2u;
        }
    }

    if (tail > 0) {
        k0_qplc_pack_32u31u_tail(src_ptr, tail, dst_ptr);
    }
}
#endif // OWN_PACK_32U_H
