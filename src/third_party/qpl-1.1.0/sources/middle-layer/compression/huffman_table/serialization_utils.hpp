/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/*
 *  Intel® Query Processing Library (Intel® QPL)
 *  Middle Layer API (private C++ API)
 */

#ifndef QPL_SERIALIZATION_UTILS_HPP_
#define QPL_SERIALIZATION_UTILS_HPP_

#include <cstring>
#include <stdint.h>
#include "compression/huffman_table/huffman_table.hpp"
#include "compression/huffman_table/huffman_table_utils.hpp"
#include "qplc_huffman_table.h" // struct qplc_huffman_table_default_format
#include "igzip_lib.h" // struct isal_hufftabless
#include "compression/huffman_table/deflate_huffman_table.hpp" // struct hw_compression_huffman_table, struct deflate_header
#include "compression/huffman_table/inflate_huffman_table.hpp" // struct canned_table

namespace qpl::ml::serialization {

using namespace qpl::ml::compression;

// utils

template <typename T>
void write_impl(uint8_t **dst, const T src) {
    using actual_type = typename std::remove_pointer<T>::type;

    memcpy(*dst, reinterpret_cast<const uint8_t *>(src), sizeof(actual_type));
    *dst += sizeof(actual_type);
}

template <typename T>
void read_impl(uint8_t **src, T dst) {
    using actual_type = typename std::remove_pointer<T>::type;

    memcpy(reinterpret_cast<uint8_t *>(dst), *src, sizeof(actual_type));
    *src += sizeof(actual_type);
}

// (de)serialization-related functions for meta header

void get_meta_size(const huffman_table_meta_t &meta, size_t *out_size);
void serialize_meta(const huffman_table_meta_t &meta, uint8_t *buffer);
void deserialize_meta(const uint8_t * const buffer, huffman_table_meta_t &meta);

// (de)serialization-related functions for internal representations used
// in qpl_(de)compression_table structures

[[nodiscard]] size_t flatten_table_size(const qplc_huffman_table_default_format &table);
[[nodiscard]] size_t flatten_table_size(const isal_hufftables &table);
[[nodiscard]] size_t flatten_table_size(const hw_compression_huffman_table &table);
[[nodiscard]] size_t flatten_table_size(const deflate_header &table);
[[nodiscard]] size_t flatten_table_size(const qplc_huffman_table_flat_format &table);
[[nodiscard]] size_t flatten_table_size(const hw_decompression_state &table);
[[nodiscard]] size_t flatten_table_size(const inflate_huff_code_large &table);
[[nodiscard]] size_t flatten_table_size(const inflate_huff_code_small &table);
[[nodiscard]] size_t flatten_table_size(const canned_table &table);

void serialize_table(const qplc_huffman_table_default_format &table, uint8_t *buffer);
void serialize_table(const isal_hufftables &table, uint8_t *buffer);
void serialize_table(const hw_compression_huffman_table &table, uint8_t *buffer);
void serialize_table(const deflate_header &table, uint8_t *buffer);
void serialize_table(const qplc_huffman_table_flat_format &table, uint8_t *buffer);
void serialize_table(const hw_decompression_state &table, uint8_t *buffer);
void serialize_table(const inflate_huff_code_large &table, uint8_t *buffer);
void serialize_table(const inflate_huff_code_small &table, uint8_t *buffer);
void serialize_table(const canned_table &table, uint8_t *buffer);

void deserialize_table(const uint8_t * const buffer, qplc_huffman_table_default_format &table);
void deserialize_table(const uint8_t * const buffer, isal_hufftables &table);
void deserialize_table(const uint8_t * const buffer, hw_compression_huffman_table &table);
void deserialize_table(const uint8_t * const buffer, deflate_header &table);
void deserialize_table(const uint8_t * const buffer, qplc_huffman_table_flat_format &table);
void deserialize_table(const uint8_t * const buffer, hw_decompression_state &table);
void deserialize_table(const uint8_t * const buffer, inflate_huff_code_large &table);
void deserialize_table(const uint8_t * const buffer, inflate_huff_code_small &table);
void deserialize_table(const uint8_t * const buffer, canned_table &table);

}

#endif // QPL_SERIALIZATION_UTILS_HPP_
