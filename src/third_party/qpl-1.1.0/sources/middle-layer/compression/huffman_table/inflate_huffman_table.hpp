/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#ifndef QPL_MIDDLE_LAYER_COMPRESSION_DECOMPRESSION_TABLE_HPP
#define QPL_MIDDLE_LAYER_COMPRESSION_DECOMPRESSION_TABLE_HPP

#include "igzip_lib.h"
#include "deflate_huffman_table.hpp"
#include <cstdint>
#include "hw_definitions.h"
#include "hw_aecs_api.h"

typedef struct inflate_state isal_inflate_state;

namespace qpl::ml::compression {
constexpr uint32_t hw_state_data_size = HW_AECS_ANALYTICS_SIZE + HW_PATH_STRUCTURES_REQUIRED_ALIGN;

/**
 * @brief The following structure stores lookup table, which can be used for canned mode decompression later
 */
struct canned_table {
    /**
     * Lookup table for literal huffman codes in ISA-L format
     */
    inflate_huff_code_large literal_huffman_codes;

    /**
     * Lookup table for distance huffman codes in ISA-L format
     */
    inflate_huff_code_small distance_huffman_codes;

    /**
     * Field containing eob symbol code and length
     */
    uint32_t eob_code_and_len;

    /**
     * The following flag indicates if this is final deflate block (i.e. if bfinal flag set in deflate header or not)
     */
    bool is_final_block;
};

/**
 * @brief Structure that represents hardware decompression table
 * This is just a stab and is not used anywhere yet
 */
struct hw_decompression_state {
    uint8_t data[hw_state_data_size];
};

class decompression_huffman_table {
public:
    decompression_huffman_table(uint8_t *sw_table_ptr,
                                uint8_t *hw_table_ptr,
                                uint8_t *deflate_header_ptr,
                                uint8_t *canned_table_ptr);

    auto get_sw_decompression_table() noexcept -> qplc_huffman_table_flat_format *;
    auto get_hw_decompression_state() noexcept -> hw_decompression_state *;
    auto get_deflate_header() noexcept -> deflate_header *;
    auto get_deflate_header_data() noexcept -> uint8_t *;
    auto get_deflate_header_bit_size() noexcept -> uint32_t;
    auto get_canned_table() noexcept -> canned_table *;

    void set_deflate_header_bit_size(uint32_t value) noexcept;

    void enable_sw_decompression_table() noexcept;
    void enable_hw_decompression_table() noexcept;
    void enable_deflate_header() noexcept;

    auto is_sw_decompression_table_used() noexcept -> bool;
    auto is_hw_decompression_table_used() noexcept -> bool;
    auto is_deflate_header_used() noexcept -> bool;

private:
    hw_decompression_state         *hw_decompression_table_ptr;
    qplc_huffman_table_flat_format *sw_decompression_table_ptr;
    canned_table                   *canned_table_ptr_;
    deflate_header                 *deflate_header_ptr_;

    bool sw_decompression_table_flag;
    bool hw_decompression_table_flag;
    bool deflate_header_flag;
};
}

#endif // QPL_MIDDLE_LAYER_COMPRESSION_DECOMPRESSION_TABLE_HPP
