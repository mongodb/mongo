/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#include <assert.h>

#include "hw_aecs_api.h"
#include "own_compress.h"
#include "own_hw_definitions.h"
#include "own_checkers.h"

#define PLATFORM 2
#include "qplc_memop.h"

#define OWN_INFLATE_INPUT_ACCUMULATOR_DQ_COUNT 32u
#define OWN_MAX_BIT_IDX               7u

static_assert(sizeof(hw_iaa_aecs_analytic) == HW_AECS_ANALYTICS_SIZE, "hw_aecs_analytic size is not correct");

#define OWN_STATUS_OK 0u
#define OWN_STATUS_ERROR 1u

/**
 * @brief Helper for packing Huffman table
 *
 * @param[out]  out  output packed values
 * @param[in]   in   input unpacked values
 *
 */
static inline void hw_pack(uint32_t out[5], const uint16_t in[15]) {
    out[0] = ((uint32_t) (in[0] & ((1u << 2u) - 1u)));           // mask = 00000000000000000000000000000011
    out[0] |= ((uint32_t) (in[1] & ((1u << 3u) - 1u))) << 2u;    // mask = 00000000000000000000000000011100
    out[0] |= ((uint32_t) (in[2] & ((1u << 4u) - 1u))) << 5u;    // mask = 00000000000000000000000111100000
    out[0] |= ((uint32_t) (in[3] & ((1u << 5u) - 1u))) << 9u;    // mask = 00000000000000000011111000000000
    out[0] |= ((uint32_t) (in[4] & ((1u << 6u) - 1u))) << 14u;   // mask = 00000000000011111100000000000000
    out[0] |= ((uint32_t) (in[5] & ((1u << 7u) - 1u))) << 20u;   // mask = 00000111111100000000000000000000
    out[1] = ((uint32_t) (in[6] & ((1u << 8u) - 1u)));           // mask = 00000000000000000000000011111111
    out[1] |= ((uint32_t) (in[7] & ((1u << 9u) - 1u))) << 8u;    // mask = 00000000000000011111111100000000
    out[1] |= ((uint32_t) (in[8] & ((1u << 10u) - 1u))) << 17u;  // mask = 00000111111111100000000000000000
    out[2] = ((uint32_t) (in[9] & ((1u << 11u) - 1u)));          // mask = 00000000000000000000011111111111
    out[2] |= ((uint32_t) (in[10] & ((1u << 12u) - 1u))) << 11u; // mask = 00000000011111111111100000000000
    out[3] = ((uint32_t) (in[11] & ((1u << 13u) - 1u)));         // mask = 00000000000000000001111111111111
    out[3] |= ((uint32_t) (in[12] & ((1u << 14u) - 1u))) << 13u; // mask = 00000111111111111110000000000000
    out[4] = ((uint32_t) (in[13] & ((1u << 15u) - 1u)));         // mask = 00000000000000000111111111111111
    out[4] |= ((uint32_t) (in[14] & ((1u << 16u) - 1u))) << 15u; // mask = 01111111111111111000000000000000
}

HW_PATH_IAA_AECS_API(void, decompress_set_huffman_only_huffman_table, (hw_iaa_aecs_decompress *const aecs_ptr,
                                                                       hw_iaa_d_huffman_only_table *const huffman_table_ptr)) {
    avx512_qplc_zero_8u((uint8_t *) aecs_ptr, sizeof(hw_iaa_aecs_analytic));

    hw_pack(aecs_ptr->lit_len_first_tbl_idx, huffman_table_ptr->first_table_indexes);
    hw_pack(aecs_ptr->lit_len_num_codes, huffman_table_ptr->number_of_codes);
    hw_pack(aecs_ptr->lit_len_first_code, huffman_table_ptr->first_codes);

    aecs_ptr->lit_len_first_len_code[0] = 0x07FFFFFFu;
    aecs_ptr->lit_len_first_len_code[1] = 0x07FFFFFFu;
    aecs_ptr->lit_len_first_len_code[2] = 0x007FFFFFu;
    aecs_ptr->lit_len_first_len_code[3] = 0x07FFFFFFu;
    aecs_ptr->lit_len_first_len_code[4] = 0x7FFFFFFFu;
    avx512_qplc_copy_8u((uint8_t *) huffman_table_ptr->index_to_char, (uint8_t *) aecs_ptr->lit_len_sym, 256u);
}

HW_PATH_IAA_AECS_API(uint32_t, decompress_set_huffman_only_huffman_table_from_histogram, (hw_iaa_aecs_decompress *const aecs_ptr,
                                                                                          const hw_iaa_histogram *const histogram_ptr)) {
    uint16_t num_codes[16];
    uint16_t first_code[16];
    uint16_t next_code[16];
    uint16_t first_tbl_idx[16];
    uint32_t idx;
    uint32_t len;
    uint32_t code;
    uint8_t  *lit_len_sym;

    const uint32_t *const ll_huffman_table = histogram_ptr->ll_sym;
    aecs_ptr->decompress_state = DEF_STATE_LL_TOKEN;

    num_codes[0] = 0u;
    for (idx = 1u; idx <= 15u; idx++) {
        num_codes[idx] = first_code[idx] = next_code[idx] = 0u;
    }

    first_tbl_idx[0] = 0u;

    for (idx = 0u; idx < 256u; idx++) {
        code = ll_huffman_table[idx];
        len  = code >> 15u;
        if (0u == len) {
            continue;
        }
        if (15u < len) {
            return 2u;
        }
        code &= 0x7FFFu;
        if (0u == num_codes[len]) {
            // first time
            num_codes[len]  = 1u;
            first_code[len] = code;
            next_code[len]  = code + 1u;
        } else {
            num_codes[len]++;
            if (code != next_code[len]) {
                return 1u;
            }
            next_code[len]++;
        }
    }

    for (idx = 0u; idx < 15u; idx++) {
        first_tbl_idx[idx + 1] = first_tbl_idx[idx] + num_codes[idx];
    }
    hw_pack(aecs_ptr->lit_len_first_tbl_idx, first_tbl_idx + 1u);
    hw_pack(aecs_ptr->lit_len_num_codes, num_codes + 1u);
    hw_pack(aecs_ptr->lit_len_first_code, first_code + 1u);
    aecs_ptr->lit_len_first_len_code[0] = 0x07FFFFFFu;
    aecs_ptr->lit_len_first_len_code[1] = 0x07FFFFFFu;
    aecs_ptr->lit_len_first_len_code[2] = 0x007FFFFFu;
    aecs_ptr->lit_len_first_len_code[3] = 0x07FFFFFFu;
    aecs_ptr->lit_len_first_len_code[4] = 0x7FFFFFFFu;

    lit_len_sym = aecs_ptr->lit_len_sym;
    for (idx    = 0u; idx < 256u; idx++) {
        len = ll_huffman_table[idx] >> 15u;
        if (len != 0u) {
            lit_len_sym[first_tbl_idx[len]++] = idx;
        }
    }

    return 0;
}

HW_PATH_IAA_AECS_API(void, decompress_set_dictionary, (hw_iaa_aecs_decompress *const aecs_ptr,
                                                       const uint8_t *const raw_dictionary_ptr,
                                                       const size_t dictionary_length)) {
    for (uint32_t i = 0; i < dictionary_length; i++) {
        aecs_ptr->history_buffer[i] = raw_dictionary_ptr[i];
    }

    aecs_ptr->history_buffer_params.history_buffer_write_offset = (uint16_t) dictionary_length;

    aecs_ptr->history_buffer_params.is_history_buffer_overflowed = 1;
}

HW_PATH_IAA_AECS_API(uint32_t, decompress_set_input_accumulator, (hw_iaa_aecs_decompress *const aecs_ptr,
                                                                  const uint8_t *const source_ptr,
                                                                  const uint32_t source_size,
                                                                  const uint8_t ignore_start_bits,
                                                                  const uint8_t ignore_end_bits)){
    uint32_t i;
    for (i = 0u; i < OWN_INFLATE_INPUT_ACCUMULATOR_DQ_COUNT - 1u; i++) {
        if (0u == aecs_ptr->input_accum_size[i]) {
            break;
        }
    }

    HW_IMMEDIATELY_RET((0u != aecs_ptr->input_accum_size[i]), OWN_STATUS_ERROR)

    if (1u < source_size) {
        aecs_ptr->input_accum[i]      = (*source_ptr) >> (ignore_start_bits & OWN_MAX_BIT_IDX);
        aecs_ptr->input_accum_size[i] = 8u - (ignore_start_bits & 7u);
    } else {
        HW_IMMEDIATELY_RET((1 > source_size), OWN_STATUS_ERROR)

        aecs_ptr->input_accum[i]      = (*source_ptr) >> (ignore_start_bits & OWN_MAX_BIT_IDX);
        aecs_ptr->input_accum_size[i] = OWN_MAX_BIT_IDX
                                               & (0u
                                                   - (int32_t) ignore_start_bits
                                                   - (int32_t) ignore_end_bits
                                                );
        aecs_ptr->input_accum[i] &= (1u << aecs_ptr->input_accum_size[i]) - 1u;
    }

    return OWN_STATUS_OK;
}
