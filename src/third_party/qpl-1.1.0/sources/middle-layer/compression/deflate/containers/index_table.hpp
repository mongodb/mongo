/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#ifndef QPL_MIDDLE_LAYER_COMPRESSION_DEFLATE_CONTAINERS_INDEX_TABLE_HPP
#define QPL_MIDDLE_LAYER_COMPRESSION_DEFLATE_CONTAINERS_INDEX_TABLE_HPP

#include <cstdint>
#include <common/defs.hpp>

namespace qpl::ml::compression {
constexpr uint32_t crc_bit_length = 32u;

class index_table_t {
    template <execution_path_t path>
    friend class deflate_state;

public:
    index_table_t(uint64_t *index_ptr, uint32_t current_index, uint32_t index_table_size) noexcept;

    void initialize(uint64_t *index_ptr, uint32_t current_index, uint32_t index_table_size) noexcept;

    auto write_new_index(uint32_t bit_count, uint32_t crc) noexcept -> bool;

    auto get_current_index() noexcept -> uint32_t;

    auto size() noexcept -> uint32_t;

    auto get_crc(uint32_t index) noexcept -> uint32_t;

    auto get_bit_size(uint32_t index) noexcept -> uint32_t;

    auto delete_last_index() noexcept -> bool;

protected:
    index_table_t() noexcept = default;

    uint64_t *index_ptr_;
    uint32_t  index_table_size_;

    uint32_t  current_index_;
    uint32_t  index_bit_offset = 0u;
};
}

#endif // QPL_MIDDLE_LAYER_COMPRESSION_DEFLATE_CONTAINERS_INDEX_TABLE_HPP
