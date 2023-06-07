/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#include <memory>
#include "inflate_huffman_table.hpp"
#include "hw_definitions.h"

namespace qpl::ml::compression{
decompression_huffman_table::decompression_huffman_table(uint8_t *sw_table_ptr,
                                                         uint8_t *hw_table_ptr,
                                                         uint8_t *deflate_header_ptr,
                                                         uint8_t *canned_table_ptr) {
    sw_decompression_table_ptr = reinterpret_cast<qplc_huffman_table_flat_format *>(sw_table_ptr);

    size_t aecs_buffer_size = sizeof(qpl::ml::compression::hw_decompression_state);
    void * pointer_to_be_aligned_ptr = hw_table_ptr;

    // making sure that the ptr to hw_decompression_table is properly aligned
    // note: the call to std::align would change the ptr and the buffer size
    // at the end size of actual data stored in hw_decompression_table_ptr is HW_AECS_ANALYTICS_SIZE
    // and sizeof(qpl::ml::compression::hw_decompression_state) =
    // = HW_PATH_STRUCTURES_REQUIRED_ALIGN + HW_AECS_ANALYTICS_SIZE
    auto aligned_aecs_ptr = std::align(HW_PATH_STRUCTURES_REQUIRED_ALIGN,
                                       HW_AECS_ANALYTICS_SIZE,
                                       pointer_to_be_aligned_ptr,
                                       aecs_buffer_size);

    hw_decompression_table_ptr = reinterpret_cast<hw_decompression_state *>(aligned_aecs_ptr);
    deflate_header_ptr_ = reinterpret_cast<deflate_header *>(deflate_header_ptr);
    canned_table_ptr_ = reinterpret_cast<canned_table *>(canned_table_ptr);

    sw_decompression_table_flag = false;
    hw_decompression_table_flag = false;
    deflate_header_flag         = false;
}

auto decompression_huffman_table::get_deflate_header() noexcept -> deflate_header * {
    return deflate_header_ptr_;
}

auto decompression_huffman_table::get_deflate_header_data() noexcept -> uint8_t * {
    return deflate_header_ptr_->data;
}

auto decompression_huffman_table::get_sw_decompression_table() noexcept -> qplc_huffman_table_flat_format * {
    return sw_decompression_table_ptr;
}

auto decompression_huffman_table::get_hw_decompression_state() noexcept -> hw_decompression_state * {
    return hw_decompression_table_ptr;
}

auto decompression_huffman_table::get_deflate_header_bit_size() noexcept -> uint32_t {
    return deflate_header_ptr_->header_bit_size;
}

auto decompression_huffman_table::get_canned_table() noexcept -> canned_table * {
    return canned_table_ptr_;
}

void decompression_huffman_table::set_deflate_header_bit_size(uint32_t value) noexcept {
    deflate_header_ptr_->header_bit_size = value;
}

void decompression_huffman_table::enable_sw_decompression_table() noexcept {
    sw_decompression_table_flag = true;
}

void decompression_huffman_table::enable_hw_decompression_table() noexcept {
    hw_decompression_table_flag = true;
}

void decompression_huffman_table::enable_deflate_header() noexcept {
    deflate_header_flag = true;
}

auto decompression_huffman_table::is_hw_decompression_table_used() noexcept -> bool {
    return hw_decompression_table_flag;
}

auto decompression_huffman_table::is_sw_decompression_table_used() noexcept -> bool {
    return sw_decompression_table_flag;
}

auto decompression_huffman_table::is_deflate_header_used() noexcept -> bool {
    return deflate_header_flag;
}
}
