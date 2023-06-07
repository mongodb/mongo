/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#include "deflate_huffman_table.hpp"

namespace qpl::ml::compression {
compression_huffman_table::compression_huffman_table(uint8_t *sw_table_ptr,
                                                     uint8_t *isal_table_ptr,
                                                     uint8_t *hw_table_ptr,
                                                     uint8_t *deflate_header_ptr) noexcept {
    sw_compression_table_ptr_   = reinterpret_cast<qplc_huffman_table_default_format *>(sw_table_ptr);
    isal_compression_table_ptr_ = reinterpret_cast<isal_hufftables *>(isal_table_ptr);
    hw_compression_table_ptr_   = reinterpret_cast<hw_compression_huffman_table *>(hw_table_ptr);
    deflate_header_ptr_         = reinterpret_cast<deflate_header *>(deflate_header_ptr);

    sw_compression_table_flag_ = false;
    hw_compression_table_flag_ = false;
    deflate_header_flag_       = false;
    huffman_only_flag_         = false;
}

auto compression_huffman_table::get_deflate_header() const noexcept -> deflate_header * {
    return deflate_header_ptr_;
}

auto compression_huffman_table::get_deflate_header_data() const noexcept -> uint8_t * {
    return deflate_header_ptr_->data;
}

auto compression_huffman_table::get_sw_compression_table() const noexcept -> qplc_huffman_table_default_format * {
    return sw_compression_table_ptr_;
}

auto compression_huffman_table::get_isal_compression_table() const noexcept -> isal_hufftables * {
    return isal_compression_table_ptr_;
}

auto compression_huffman_table::get_hw_compression_table() const noexcept -> hw_compression_huffman_table * {
    return hw_compression_table_ptr_;
}

auto compression_huffman_table::get_deflate_header_bit_size() const noexcept -> uint32_t {
    return deflate_header_ptr_->header_bit_size;
}

auto compression_huffman_table::set_deflate_header_bit_size(uint32_t value) noexcept -> void {
    deflate_header_ptr_->header_bit_size = value;
}

void compression_huffman_table::enable_sw_compression_table() noexcept {
    sw_compression_table_flag_ = true;
}

void compression_huffman_table::enable_hw_compression_table() noexcept {
    hw_compression_table_flag_ = true;
}

void compression_huffman_table::enable_deflate_header() noexcept {
    deflate_header_flag_ = true;
}

void compression_huffman_table::make_huffman_only() noexcept {
    huffman_only_flag_ = true;
}

auto compression_huffman_table::is_sw_compression_table_used() const noexcept -> bool {
    return sw_compression_table_flag_;
}
auto compression_huffman_table::is_hw_compression_table_used() const noexcept -> bool {
    return hw_compression_table_flag_;
}

auto compression_huffman_table::is_deflate_header_used() const noexcept-> bool {
    return deflate_header_flag_;
}

auto compression_huffman_table::is_huffman_only() const noexcept-> bool {
    return huffman_only_flag_;
}
}
