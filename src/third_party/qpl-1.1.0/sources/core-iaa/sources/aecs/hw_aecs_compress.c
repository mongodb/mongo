/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#include <assert.h>

#include "own_hw_definitions.h"
#include "hw_aecs_api.h"
#include "own_compress.h"
#include "own_checkers.h"

#define PLATFORM 2
#include "qplc_memop.h"
#include "qplc_compression_consts.h"

#define OWN_STATUS_OK 0u
#define OWN_STATUS_ERROR 1u

uint32_t hw_create_huff_tables(uint32_t *ll_codes_ptr,
                               uint32_t *d_codes_ptr,
                               uint8_t *out_accum_ptr,
                               uint32_t oa_size,
                               uint32_t oa_valid_bits,
                               uint32_t *ll_hist_ptr,
                               uint32_t *d_hist_ptr);

void hw_create_huff_tables_no_hdr(uint32_t *ll_codes_ptr, uint32_t *ll_hist_ptr);

static_assert(sizeof(hw_iaa_aecs_compress) == HW_AECS_COMPRESSION_SIZE, "hw_aecs_analytic size is not correct");

HW_PATH_IAA_AECS_API(void, compress_accumulator_insert_eob, (hw_iaa_aecs_compress *const eacs_deflate_ptr,
        const hw_huffman_code eob_symbol)) {
    uint32_t offset = eacs_deflate_ptr->num_output_accum_bits;
    uint32_t data   = *(uint32_t *) (eacs_deflate_ptr->output_accum + (offset / 8u));
    data &= (1u << (offset & 7u)) - 1u;
    data |= eob_symbol.code << (offset & 7u);
    *(uint32_t *) (eacs_deflate_ptr->output_accum + (offset / 8u)) = data;
    eacs_deflate_ptr->num_output_accum_bits = offset + eob_symbol.length;
}

HW_PATH_IAA_AECS_API(void, compress_accumulator_flush, (hw_iaa_aecs_compress *const aecs_ptr,
        uint8_t **const next_out_pptr,
        const uint32_t bit_offset)) {
    const uint32_t bytes_to_flush = (bit_offset + 7u) / 8u;

    for (uint32_t i = 0u; i < bytes_to_flush; i++) {
        *next_out_pptr[i] = aecs_ptr->output_accum[i];
    }

    aecs_ptr->num_output_accum_bits = 0u;
    *(uint64_t *) aecs_ptr->output_accum = 0u;
}

HW_PATH_IAA_AECS_API(uint32_t, compress_write_deflate_fixed_header, (hw_iaa_aecs_compress *const aecs_ptr,
        const uint32_t b_final)) {
    uint32_t byte_offset;
    uint32_t bit_offset;
    uint32_t data;

    HW_IMMEDIATELY_RET((2 <= b_final), OWN_STATUS_ERROR);
    // 34 = 32 bytes of prev output accum + 2 bytes of EOB
    HW_IMMEDIATELY_RET((aecs_ptr->num_output_accum_bits >= 34u * 8u), OWN_STATUS_ERROR);

    if (0u == (aecs_ptr->num_output_accum_bits & 7u)) {
        // Byte aligned
        byte_offset = aecs_ptr->num_output_accum_bits / 8u;
        aecs_ptr->output_accum[byte_offset] = b_final | DYNAMIC_HDR;
        aecs_ptr->num_output_accum_bits += DYNAMIC_HDR_SIZE;
    } else {
        // Not byte aligned
        byte_offset = aecs_ptr->num_output_accum_bits / 8u;
        bit_offset  = aecs_ptr->num_output_accum_bits & 7u;
        data        = (b_final | DYNAMIC_HDR) << bit_offset;
        // Do we need to worry about high order bits being set???
        *(uint32_t *) (aecs_ptr->output_accum + byte_offset) |= data;
        aecs_ptr->num_output_accum_bits += DYNAMIC_HDR_SIZE;
    }

    return 0u;
}

HW_PATH_IAA_AECS_API(uint32_t, compress_write_deflate_dynamic_header, (hw_iaa_aecs_compress *const aecs_ptr,
        const uint8_t *const header_ptr,
        const uint32_t header_bit_size,
        const uint32_t b_final)) {
    uint32_t byte_offset;
    uint32_t bit_offset;
    uint32_t idx;
    uint32_t write_bits;
    uint64_t bit_buf;
    uint64_t hdr;
    uint64_t *bit_ptr;

    HW_IMMEDIATELY_RET((2 <= b_final), OWN_STATUS_ERROR);
    // 34 = 32 bytes of prev output accum + 2 bytes of EOB
    HW_IMMEDIATELY_RET((aecs_ptr->num_output_accum_bits >= 34u * 8u), OWN_STATUS_ERROR);

    if (0u == (aecs_ptr->num_output_accum_bits & 7u)) {
        // Byte aligned
        byte_offset = aecs_ptr->num_output_accum_bits / 8u;
        avx512_qplc_copy_8u(header_ptr, aecs_ptr->output_accum + byte_offset, (header_bit_size + 7u) / 8u);

        // Clear bfinal bit and set corresponding value to it
        aecs_ptr->output_accum[byte_offset] &= ~(BFINAL_BIT);
        aecs_ptr->output_accum[byte_offset] |= b_final;

        aecs_ptr->num_output_accum_bits += header_bit_size;
    } else {
        // Not byte aligned
        byte_offset = aecs_ptr->num_output_accum_bits / 8u;
        bit_offset  = aecs_ptr->num_output_accum_bits & 7u;
        bit_ptr     = (uint64_t *) (aecs_ptr->output_accum + byte_offset);
        bit_buf     = *bit_ptr;
        bit_buf &= (1u << bit_offset) - 1u;
        write_bits  = bit_offset + header_bit_size;
        uint32_t tmp0 = aecs_ptr->histogram.ll_sym[0];
        uint32_t tmp1 = aecs_ptr->histogram.ll_sym[1];

        for (idx = 0u; 64u * idx < write_bits; idx++) {
            hdr = *(idx + (uint64_t *) (header_ptr));
            bit_buf |= hdr << bit_offset;
            bit_ptr[idx] = bit_buf;
            bit_buf = hdr >> (64u - bit_offset);
        }

        aecs_ptr->histogram.ll_sym[0] = tmp0;
        aecs_ptr->histogram.ll_sym[1] = tmp1;
        bit_ptr[idx] = bit_buf;

        // Clear bfinal bit and set corresponding value to it
        aecs_ptr->output_accum[byte_offset] &= ~(BFINAL_BIT << bit_offset);
        aecs_ptr->output_accum[byte_offset] |= b_final << bit_offset;

        aecs_ptr->num_output_accum_bits += header_bit_size;
    }

    return OWN_STATUS_OK;
}

HW_PATH_IAA_AECS_API(void, compress_set_deflate_huffman_table, (hw_iaa_aecs_compress *const aecs_ptr,
        const hw_iaa_huffman_codes *const literal_length_codes,
        const hw_iaa_huffman_codes *const distance_codes_ptr)) {
    avx512_qplc_copy_8u((const uint8_t *) literal_length_codes,
                  (uint8_t *) aecs_ptr->histogram.ll_sym,
                  MAX_HEAP * sizeof(uint32_t));
    avx512_qplc_copy_8u((const uint8_t *) distance_codes_ptr,
                  (uint8_t *) aecs_ptr->histogram.d_sym,
                  30u * sizeof(uint32_t));
}

HW_PATH_IAA_AECS_API(void, compress_write_deflate_dynamic_header_from_histogram, (hw_iaa_aecs_compress *const aecs_ptr,
        hw_iaa_histogram *const histogram_ptr,
        const uint32_t b_final)) {
    uint32_t num_output_accum_bits = aecs_ptr->num_output_accum_bits;

    aecs_ptr->num_output_accum_bits = hw_create_huff_tables(aecs_ptr->histogram.ll_sym,
                                                            aecs_ptr->histogram.d_sym,
                                                            aecs_ptr->output_accum,
                                                            sizeof(aecs_ptr->output_accum),
                                                            num_output_accum_bits,
                                                            histogram_ptr->ll_sym,
                                                            histogram_ptr->d_sym);
    if (b_final) {
        aecs_ptr->output_accum[num_output_accum_bits / 8u] |= 1ULL << (num_output_accum_bits & 7u);
    }
}

HW_PATH_IAA_AECS_API(void, compress_set_huffman_only_huffman_table, (hw_iaa_aecs_compress *const aecs_ptr,
                                                                     hw_iaa_huffman_codes *const literal_length_codes)) {
    avx512_qplc_copy_8u((const uint8_t *) literal_length_codes,
                  (uint8_t *) aecs_ptr->histogram.ll_sym,
                  MAX_HEAP * sizeof(uint32_t));
}

HW_PATH_IAA_AECS_API(void, compress_set_huffman_only_huffman_table_from_histogram, (hw_iaa_aecs_compress *const aecs_ptr,
                                                                                    hw_iaa_histogram *const histogram_ptr)) {
    hw_create_huff_tables_no_hdr(aecs_ptr->histogram.ll_sym, histogram_ptr->ll_sym);
}

HW_PATH_IAA_AECS_API(void, compress_store_huffman_only_huffman_table, (const hw_iaa_aecs_compress *const aecs_ptr,
                                                                       hw_iaa_c_huffman_only_table *const huffman_table_ptr)) {
    avx512_qplc_copy_8u((uint8_t *) aecs_ptr->histogram.ll_sym,
                        (uint8_t *) huffman_table_ptr->literals_matches,
                        QPLC_DEFLATE_LITERALS_COUNT * sizeof(uint32_t));
    avx512_qplc_zero_8u((uint8_t *) huffman_table_ptr->offsets,
                        QPLC_DEFLATE_OFFSETS_COUNT * sizeof(uint32_t));
}
