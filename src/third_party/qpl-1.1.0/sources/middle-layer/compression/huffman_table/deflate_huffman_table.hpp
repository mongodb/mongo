/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#ifndef QPL_MIDDLE_LAYER_COMPRESSION_COMPRESSION_TABLE_HPP
#define QPL_MIDDLE_LAYER_COMPRESSION_COMPRESSION_TABLE_HPP

#include <cstdint>

#include "igzip_lib.h"
#include "qplc_huffman_table.h"

namespace qpl::ml::compression {
/**
 * @brief Structure that holds Huffman codes for compression
 *
 * There are two different Huffman tables:
 *  One for literals and match lengths
 *  One for offsets
 *
 * Both of them have the same format:
 *  Bits [14:0] - code itself
 *  Bits [18:15] - code length
 *
 * Code is not bit-reversed, stored in LE
 */
constexpr uint32_t hw_compression_huffman_table_size = 1u;
constexpr uint32_t deflate_header_size = 218u;

/**
 * @brief Structure that represents hardware compression table
 * This is just a stab and is not used anywhere yet
 */
struct hw_compression_huffman_table {
    uint8_t data[hw_compression_huffman_table_size];
};

/**
 * @brief Structure that holds information about deflate header
 */
struct deflate_header {
    uint32_t header_bit_size;            /**< Deflate header bit size */
    uint8_t  data[deflate_header_size];  /**< Deflate header content */
};

class compression_huffman_table {
public:
    compression_huffman_table(uint8_t *sw_table_ptr,
                              uint8_t *isal_table_ptr,
                              uint8_t *hw_table_ptr,
                              uint8_t *deflate_header_ptr) noexcept;

    auto get_sw_compression_table() const noexcept -> qplc_huffman_table_default_format *;
    auto get_isal_compression_table() const noexcept -> isal_hufftables *;
    auto get_hw_compression_table() const noexcept -> hw_compression_huffman_table *;
    auto get_deflate_header() const noexcept -> deflate_header *;
    auto get_deflate_header_data() const noexcept -> uint8_t *;
    auto get_deflate_header_bit_size() const noexcept -> uint32_t;

    void set_deflate_header_bit_size(uint32_t value) noexcept ;

    void enable_sw_compression_table() noexcept;
    void enable_hw_compression_table() noexcept;
    void enable_deflate_header() noexcept;
    void make_huffman_only() noexcept;

    auto is_sw_compression_table_used() const noexcept -> bool;
    auto is_hw_compression_table_used() const noexcept -> bool;
    auto is_deflate_header_used() const noexcept -> bool;
    auto is_huffman_only() const noexcept -> bool;

private:
    hw_compression_huffman_table      *hw_compression_table_ptr_;
    qplc_huffman_table_default_format *sw_compression_table_ptr_;
    isal_hufftables                   *isal_compression_table_ptr_;
    deflate_header                    *deflate_header_ptr_;

    bool sw_compression_table_flag_;
    bool hw_compression_table_flag_;
    bool deflate_header_flag_;
    bool huffman_only_flag_;
};
}

#endif // QPL_MIDDLE_LAYER_COMPRESSION_COMPRESSION_TABLE_HPP
