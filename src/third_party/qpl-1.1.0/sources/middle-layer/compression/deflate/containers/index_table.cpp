/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#include "index_table.hpp"

namespace qpl::ml::compression {
index_table_t::index_table_t(uint64_t *index_ptr, uint32_t current_index, uint32_t index_table_size) noexcept :
    index_ptr_(index_ptr), index_table_size_(index_table_size), current_index_(current_index) {
    // Empty constructor
}

void index_table_t::initialize(uint64_t *index_ptr, uint32_t current_index, uint32_t index_table_size) noexcept {
    index_ptr_        = index_ptr;
    current_index_    = current_index;
    index_table_size_ = index_table_size;
}

auto index_table_t::write_new_index(uint32_t bit_count, uint32_t crc) noexcept -> bool {
    // Additionaly check if index table is available to write
    if (current_index_ < index_table_size_) {
        index_ptr_[current_index_] = static_cast<uint64_t>(bit_count) + index_bit_offset;
        index_ptr_[current_index_] |= (static_cast<uint64_t>(crc) << crc_bit_length);
        current_index_++;

        return true;
    } else {
        return false;
    }
}

auto index_table_t::get_current_index() noexcept -> uint32_t {
    return current_index_;
}

auto index_table_t::size() noexcept -> uint32_t {
    return index_table_size_;
}

auto index_table_t::get_crc(uint32_t index) noexcept -> uint32_t {
    return static_cast<uint32_t>((index_ptr_[index] >> crc_bit_length));
}

auto index_table_t::get_bit_size(uint32_t index) noexcept -> uint32_t {
    return static_cast<uint32_t>(index_ptr_[index]);
}

auto index_table_t::delete_last_index() noexcept -> bool {
    if (current_index_ != 0) {
        current_index_--;
        return true;
    } else {
        return false;
    }
}
}
