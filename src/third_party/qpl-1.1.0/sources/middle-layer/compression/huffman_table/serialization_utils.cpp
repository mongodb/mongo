/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/*
 *  Intel® Query Processing Library (Intel® QPL)
 *  Middle Layer API (private C++ API)
 */

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "compression/huffman_table/serialization_utils.hpp"

namespace qpl::ml::serialization {
using namespace qpl::ml::compression;

void get_meta_size(const huffman_table_meta_t &meta, size_t *out_size) {
    size_t meta_size = 0;

    meta_size += sizeof(meta.magic_num);
    meta_size += sizeof(meta.struct_id);
    meta_size += sizeof(meta.algorithm);
    meta_size += sizeof(meta.version);
    meta_size += sizeof(meta.type);
    meta_size += sizeof(meta.path);
    meta_size += sizeof(meta.flags);

    *out_size = meta_size;
}

void serialize_meta(const huffman_table_meta_t &meta, uint8_t *buffer) {
    uint8_t *dst = buffer; // adding an offset internally

    write_impl(&dst, &(meta.magic_num));
    write_impl(&dst, &(meta.struct_id));
    write_impl(&dst, &(meta.algorithm));
    write_impl(&dst, &(meta.version));
    write_impl(&dst, &(meta.type));
    write_impl(&dst, &(meta.path));
    write_impl(&dst, &(meta.flags));
}

void deserialize_meta(const uint8_t * const buffer, huffman_table_meta_t &meta) {
    uint8_t *src = const_cast<uint8_t *>(buffer); // adding an offset internally

    read_impl(&src, &(meta.magic_num));
    read_impl(&src, &(meta.struct_id));
    read_impl(&src, &(meta.algorithm));
    read_impl(&src, &(meta.version));
    read_impl(&src, &(meta.type));
    read_impl(&src, &(meta.path));
    read_impl(&src, &(meta.flags));
}

/* List of simple routines to return the size of each flattened internal structure
   from qpl_(de)compression_huffman_table
*/

size_t flatten_table_size(const qplc_huffman_table_default_format &table) {
    size_t table_size = 0;

    table_size += sizeof(table.literals_matches);
    table_size += sizeof(table.offsets);

    return table_size;
}

size_t flatten_table_size(const isal_hufftables &table) {
    size_t table_size = 0;

    table_size += sizeof(table.deflate_hdr);
    table_size += sizeof(table.deflate_hdr_count);
    table_size += sizeof(table.deflate_hdr_extra_bits);
    table_size += sizeof(table.dist_table);
    table_size += sizeof(table.len_table);
    table_size += sizeof(table.lit_table);
    table_size += sizeof(table.lit_table_sizes);
    table_size += sizeof(table.dcodes);
    table_size += sizeof(table.dcodes_sizes);

    return table_size;
}

size_t flatten_table_size(const hw_compression_huffman_table &table) {
    size_t table_size = 0;

    table_size += sizeof(table.data);

    return table_size;
}

size_t flatten_table_size(const deflate_header &table) {
    size_t table_size = 0;

    table_size += sizeof(table.header_bit_size);
    table_size += sizeof(table.data);

    return table_size;
}

size_t flatten_table_size(const qplc_huffman_table_flat_format &table) {
    size_t table_size = 0;

    table_size += sizeof(table.number_of_codes);
    table_size += sizeof(table.first_codes);
    table_size += sizeof(table.first_table_indexes);
    table_size += sizeof(table.index_to_char);

    return table_size;
}

size_t flatten_table_size(const hw_decompression_state &table) {
    size_t table_size = 0;

    // using here actual data size without padding
    // see inflate_huffman_table.cpp
    table_size += sizeof(table.data) - HW_PATH_STRUCTURES_REQUIRED_ALIGN;

    return table_size;
}

size_t flatten_table_size(const inflate_huff_code_large &table) {
    size_t table_size = 0;

    table_size += sizeof(table.short_code_lookup);
    table_size += sizeof(table.long_code_lookup);

    return table_size;
}

size_t flatten_table_size(const inflate_huff_code_small &table) {
    size_t table_size = 0;

    table_size += sizeof(table.short_code_lookup);
    table_size += sizeof(table.long_code_lookup);

    return table_size;
}

size_t flatten_table_size(const canned_table &table) {
    size_t table_size = 0;

    table_size += flatten_table_size(table.literal_huffman_codes);
    table_size += flatten_table_size(table.distance_huffman_codes);
    table_size += sizeof(table.eob_code_and_len);
    table_size += sizeof(table.is_final_block);

    return table_size;
}

/* List of simple routines to serialize each internal structure
   from qpl_(de)compression_huffman_table
*/

void serialize_table(const qplc_huffman_table_default_format &table, uint8_t *buffer) {
    uint8_t *dst = buffer; // adding an offset internally

    write_impl(&dst, &(table.literals_matches));
    write_impl(&dst, &(table.offsets));
}

void serialize_table(const isal_hufftables &table, uint8_t *buffer) {
    uint8_t *dst = buffer; // adding an offset internally

    write_impl(&dst, &(table.deflate_hdr));
    write_impl(&dst, &(table.deflate_hdr_count));
    write_impl(&dst, &(table.deflate_hdr_extra_bits));
    write_impl(&dst, &(table.dist_table));
    write_impl(&dst, &(table.len_table));
    write_impl(&dst, &(table.lit_table));
    write_impl(&dst, &(table.lit_table_sizes));
    write_impl(&dst, &(table.dcodes));
    write_impl(&dst, &(table.dcodes_sizes));
}

void serialize_table(const hw_compression_huffman_table &table, uint8_t *buffer) {
    uint8_t *dst = buffer; // adding an offset internally

    write_impl(&dst, &(table.data));
}

void serialize_table(const deflate_header &table, uint8_t *buffer) {
    uint8_t *dst = buffer; // adding an offset internally

    write_impl(&dst, &(table.header_bit_size));
    write_impl(&dst, &(table.data));
}

void serialize_table(const qplc_huffman_table_flat_format &table, uint8_t *buffer) {
    uint8_t *dst = buffer; // adding an offset internally

    write_impl(&dst, &(table.number_of_codes));
    write_impl(&dst, &(table.first_codes));
    write_impl(&dst, &(table.first_table_indexes));
    write_impl(&dst, &(table.index_to_char));
}

void serialize_table(const hw_decompression_state &table, uint8_t *buffer) {

    // using here actual data size without padding
    // see inflate_huffman_table.cpp
    memcpy(buffer, &(table.data), sizeof(table.data) - HW_PATH_STRUCTURES_REQUIRED_ALIGN);
}

void serialize_table(const inflate_huff_code_large &table, uint8_t *buffer) {
    uint8_t *dst = buffer; // adding an offset internally

    write_impl(&dst, &(table.short_code_lookup));
    write_impl(&dst, &(table.long_code_lookup));
}

void serialize_table(const inflate_huff_code_small &table, uint8_t *buffer) {
    uint8_t *dst = buffer; // adding an offset internally

    write_impl(&dst, &(table.short_code_lookup));
    write_impl(&dst, &(table.long_code_lookup));
}

void serialize_table(const canned_table &table, uint8_t *buffer) {
    uint8_t *dst = buffer; // adding an offset internally

    serialize_table(table.literal_huffman_codes, dst);  dst += flatten_table_size(table.literal_huffman_codes);
    serialize_table(table.distance_huffman_codes, dst); dst += flatten_table_size(table.distance_huffman_codes);
    write_impl(&dst, &(table.eob_code_and_len));
    write_impl(&dst, &(table.is_final_block));
}

void deserialize_table(const uint8_t * const buffer, qplc_huffman_table_default_format &table) {
    uint8_t *src = const_cast<uint8_t *>(buffer); // adding an offset internally

    read_impl(&src, &(table.literals_matches));
    read_impl(&src, &(table.offsets));
}

void deserialize_table(const uint8_t * const buffer, isal_hufftables &table) {
    uint8_t *src = const_cast<uint8_t *>(buffer); // adding an offset internally

    read_impl(&src, &(table.deflate_hdr));
    read_impl(&src, &(table.deflate_hdr_count));
    read_impl(&src, &(table.deflate_hdr_extra_bits));
    read_impl(&src, &(table.dist_table));
    read_impl(&src, &(table.len_table));
    read_impl(&src, &(table.lit_table));
    read_impl(&src, &(table.lit_table_sizes));
    read_impl(&src, &(table.dcodes));
    read_impl(&src, &(table.dcodes_sizes));
}

void deserialize_table(const uint8_t * const buffer, hw_compression_huffman_table &table) {
    uint8_t *src = const_cast<uint8_t *>(buffer); // adding an offset internally

    read_impl(&src, &(table.data));
}

void deserialize_table(const uint8_t * const buffer, deflate_header &table) {
    uint8_t *src = const_cast<uint8_t *>(buffer); // adding an offset internally

    read_impl(&src, &(table.header_bit_size));
    read_impl(&src, &(table.data));
}

void deserialize_table(const uint8_t * const buffer, qplc_huffman_table_flat_format &table) {
    uint8_t *src = const_cast<uint8_t *>(buffer); // adding an offset internally

    read_impl(&src, &(table.number_of_codes));
    read_impl(&src, &(table.first_codes));
    read_impl(&src, &(table.first_table_indexes));
    read_impl(&src, &(table.index_to_char));
}

void deserialize_table(const uint8_t * const buffer, hw_decompression_state &table) {
    uint8_t *src = const_cast<uint8_t *>(buffer); // adding an offset internally

    // using here actual data size without padding
    // see inflate_huffman_table.cpp
    memcpy(&(table.data), src, sizeof(table.data) - HW_PATH_STRUCTURES_REQUIRED_ALIGN);
}

void deserialize_table(const uint8_t * const buffer, inflate_huff_code_large &table) {
    uint8_t *src = const_cast<uint8_t *>(buffer); // adding an offset internally

    read_impl(&src, &(table.short_code_lookup));
    read_impl(&src, &(table.long_code_lookup));
}

void deserialize_table(const uint8_t * const buffer, inflate_huff_code_small &table) {
    uint8_t *src = const_cast<uint8_t *>(buffer); // adding an offset internally

    read_impl(&src, &(table.short_code_lookup));
    read_impl(&src, &(table.long_code_lookup));
}

void deserialize_table(const uint8_t * const buffer, canned_table &table) {
    uint8_t *src = const_cast<uint8_t *>(buffer); // adding an offset internally

    deserialize_table(src, table.literal_huffman_codes);  src += flatten_table_size(table.literal_huffman_codes);
    deserialize_table(src, table.distance_huffman_codes); src += flatten_table_size(table.distance_huffman_codes);
    read_impl(&src, &(table.eob_code_and_len));
    read_impl(&src, &(table.is_final_block));
}
}
