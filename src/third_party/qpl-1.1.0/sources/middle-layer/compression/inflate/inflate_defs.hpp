/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#ifndef QPL_MIDDLE_LAYER_COMPRESSION_INFLATE_INFLATE_PROPERTIES_HPP
#define QPL_MIDDLE_LAYER_COMPRESSION_INFLATE_INFLATE_PROPERTIES_HPP

#include <cstdint>
#include "common/defs.hpp"
#include "compression/huffman_table/inflate_huffman_table.hpp"

namespace qpl::ml::compression {
enum inflate_mode_t {
    inflate_default,
    inflate_header,
    inflate_body,
};

template<execution_path_t path>
class inflate_state;

enum compressed_data_format_t {
    raw_data,
    gzip,
    zlib,
    gzip_no_header,
    zlib_no_header
};

enum deflate_block_type_e : uint16_t {
    undefined,
    stored,
    coded
};

enum end_processing_condition_t : uint8_t {
    stop_and_check_for_bfinal_eob = 0, /**< Stop condition: b_final EOB; Check condition: b_final EOB */
    dont_stop_or_check = 1,            /**< Stop condition: none;       Check condition: none */
    stop_and_check_any_eob = 2,        /**< Stop condition: EOB;        Check condition: EOB */
    stop_on_any_eob = 3,               /**< Stop condition: EOB;        Check condition: none */
    stop_on_bfinal_eob = 4,            /**< Stop condition: b_final EOB; Check condition: none */
    check_for_any_eob = 5,             /**< Stop condition: none;       Check condition: EOB */
    check_for_bfinal_eob = 6,          /**< Stop condition: none; Check condition: b_final EOB */
    check_on_nonlast_block = 8         /**< Stop condition: disabled; Check condition: not last block */
};

struct access_properties {
    bool    is_random;
    uint8_t ignore_start_bits;
    uint8_t ignore_end_bits;
};

struct huffman_code {
    uint16_t code;               /**< Huffman code */
    uint8_t  extra_bit_count;    /**< Number of extra bits */
    uint8_t  length;             /**< Huffman code length */
};
}

#endif // QPL_MIDDLE_LAYER_COMPRESSION_INFLATE_INFLATE_PROPERTIES_HPP
