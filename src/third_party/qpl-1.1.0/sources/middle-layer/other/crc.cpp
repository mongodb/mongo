/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#include "crc.hpp"
#include "dispatcher/dispatcher.hpp"
#include "util/descriptor_processing.hpp"

#include "hw_descriptors_api.h"

namespace qpl::ml::other {

// auxiliary table to perform bits reversing in byte
static const uint8_t bit_rev_8[0x100] = {
        0x00, 0x80, 0x40, 0xC0, 0x20, 0xA0, 0x60, 0xE0, 0x10, 0x90, 0x50, 0xD0, 0x30, 0xB0, 0x70, 0xF0,
        0x08, 0x88, 0x48, 0xC8, 0x28, 0xA8, 0x68, 0xE8, 0x18, 0x98, 0x58, 0xD8, 0x38, 0xB8, 0x78, 0xF8,
        0x04, 0x84, 0x44, 0xC4, 0x24, 0xA4, 0x64, 0xE4, 0x14, 0x94, 0x54, 0xD4, 0x34, 0xB4, 0x74, 0xF4,
        0x0C, 0x8C, 0x4C, 0xCC, 0x2C, 0xAC, 0x6C, 0xEC, 0x1C, 0x9C, 0x5C, 0xDC, 0x3C, 0xBC, 0x7C, 0xFC,
        0x02, 0x82, 0x42, 0xC2, 0x22, 0xA2, 0x62, 0xE2, 0x12, 0x92, 0x52, 0xD2, 0x32, 0xB2, 0x72, 0xF2,
        0x0A, 0x8A, 0x4A, 0xCA, 0x2A, 0xAA, 0x6A, 0xEA, 0x1A, 0x9A, 0x5A, 0xDA, 0x3A, 0xBA, 0x7A, 0xFA,
        0x06, 0x86, 0x46, 0xC6, 0x26, 0xA6, 0x66, 0xE6, 0x16, 0x96, 0x56, 0xD6, 0x36, 0xB6, 0x76, 0xF6,
        0x0E, 0x8E, 0x4E, 0xCE, 0x2E, 0xAE, 0x6E, 0xEE, 0x1E, 0x9E, 0x5E, 0xDE, 0x3E, 0xBE, 0x7E, 0xFE,
        0x01, 0x81, 0x41, 0xC1, 0x21, 0xA1, 0x61, 0xE1, 0x11, 0x91, 0x51, 0xD1, 0x31, 0xB1, 0x71, 0xF1,
        0x09, 0x89, 0x49, 0xC9, 0x29, 0xA9, 0x69, 0xE9, 0x19, 0x99, 0x59, 0xD9, 0x39, 0xB9, 0x79, 0xF9,
        0x05, 0x85, 0x45, 0xC5, 0x25, 0xA5, 0x65, 0xE5, 0x15, 0x95, 0x55, 0xD5, 0x35, 0xB5, 0x75, 0xF5,
        0x0D, 0x8D, 0x4D, 0xCD, 0x2D, 0xAD, 0x6D, 0xED, 0x1D, 0x9D, 0x5D, 0xDD, 0x3D, 0xBD, 0x7D, 0xFD,
        0x03, 0x83, 0x43, 0xC3, 0x23, 0xA3, 0x63, 0xE3, 0x13, 0x93, 0x53, 0xD3, 0x33, 0xB3, 0x73, 0xF3,
        0x0B, 0x8B, 0x4B, 0xCB, 0x2B, 0xAB, 0x6B, 0xEB, 0x1B, 0x9B, 0x5B, 0xDB, 0x3B, 0xBB, 0x7B, 0xFB,
        0x07, 0x87, 0x47, 0xC7, 0x27, 0xA7, 0x67, 0xE7, 0x17, 0x97, 0x57, 0xD7, 0x37, 0xB7, 0x77, 0xF7,
        0x0F, 0x8F, 0x4F, 0xCF, 0x2F, 0xAF, 0x6F, 0xEF, 0x1F, 0x9F, 0x5F, 0xDF, 0x3F, 0xBF, 0x7F, 0xFF,
};

/**
 * @brief set of helpers bits/bytes reflecting
 */
static uint64_t bit_byte_swap_64(uint64_t x) {
    uint64_t y = bit_rev_8[x >> 56];
    y |= ((uint64_t) bit_rev_8[(x >> 48) & 0xFF]) << 8;
    y |= ((uint64_t) bit_rev_8[(x >> 40) & 0xFF]) << 16;
    y |= ((uint64_t) bit_rev_8[(x >> 32) & 0xFF]) << 24;
    y |= ((uint64_t) bit_rev_8[(x >> 24) & 0xFF]) << 32;
    y |= ((uint64_t) bit_rev_8[(x >> 16) & 0xFF]) << 40;
    y |= ((uint64_t) bit_rev_8[(x >> 8) & 0xFF]) << 48;
    y |= ((uint64_t) bit_rev_8[(x >> 0) & 0xFF]) << 56;

    return y;
}

/**
 * @brief table initializer
 */
static void crc64_init_table(uint64_t *table, uint64_t poly, bool is_big_endian) {
    uint64_t crc = 0;
    table[0] = 0;

    if (is_big_endian) {
        poly = bit_byte_swap_64(poly);
        for (uint64_t i = 1; i < 256; i++) {
            crc = i;
            for (uint32_t j = 0; j < 8; j++) {
                if (crc & 0x0000000000000001ULL) {
                    crc = (crc >> 1) ^ poly;
                } else {
                    crc = (crc >> 1);
                }
            }
            table[i] = crc;
        }
    } else {
        for (uint64_t i = 1; i < 256; i++) {
            crc = i << 56;
            for (uint32_t j = 0; j < 8; j++) {
                if (crc & 0x8000000000000000ULL) {
                    crc = (crc << 1) ^ poly;
                } else {
                    crc = (crc << 1);
                }
            }
            table[i] = crc;
        }
    }
}

/**
 * @brief CRC initializer
 */
static uint64_t crc64_init_crc(uint64_t polynomial, bool is_big_endian, bool is_inverse) {
    if (!is_inverse) {
        return 0;
    }

    polynomial |= (polynomial << 1);
    polynomial |= (polynomial << 2);
    polynomial |= (polynomial << 4);
    polynomial |= (polynomial << 8);
    polynomial |= (polynomial << 16);
    polynomial |= (polynomial << 32);

    if (!is_big_endian) {
        return polynomial;
    }

    return bit_byte_swap_64(polynomial);
}

/**
 * @brief CRC calculator
 */
static uint64_t crc64_update(uint8_t data, const uint64_t *table, uint64_t crc, bool is_big_endian) {
    if (is_big_endian) {
        return table[data ^ (crc & 0xFF)] ^ (crc >> 8);
    } else {
        return table[data ^ (crc >> 56)] ^ (crc << 8);
    }
}

/**
 * @brief Final CRC step
 */
static uint64_t crc64_finalize(uint64_t crc, uint64_t polynomial, bool is_big_endian, bool is_inverse) {
    if (!is_inverse) {
        return crc;
    }
    polynomial |= (polynomial << 1);
    polynomial |= (polynomial << 2);
    polynomial |= (polynomial << 4);
    polynomial |= (polynomial << 8);
    polynomial |= (polynomial << 16);
    polynomial |= (polynomial << 32);
    if (!is_big_endian) {
        return crc ^ polynomial;
    }

    return crc ^ bit_byte_swap_64(polynomial);
}

auto perform_crc(const uint8_t *src_ptr,
                 uint32_t length,
                 uint64_t polynomial,
                 bool is_be_bit_order,
                 bool is_inverse) -> uint64_t {
    uint64_t table[256];

    crc64_init_table(table, polynomial, is_be_bit_order);
    auto crc = crc64_init_crc(polynomial, is_be_bit_order, is_inverse);

    for (uint32_t i = 0; i < length; i++) {
        crc = crc64_update(src_ptr[i], table, crc, is_be_bit_order);
    }

    return crc64_finalize(crc, polynomial, is_be_bit_order, is_inverse);
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstack-usage=4096"
#endif

template <>
auto call_crc<execution_path_t::hardware>(const uint8_t *src_ptr,
                                          uint32_t length,
                                          uint64_t polynomial,
                                          bool is_be_bit_order,
                                          bool is_inverse,
                                          int32_t numa_id) noexcept -> crc_operation_result_t {
    HW_PATH_VOLATILE hw_completion_record HW_PATH_ALIGN_STRUCTURE completion_record{};
    hw_descriptor HW_PATH_ALIGN_STRUCTURE                         descriptor{};

    hw_iaa_descriptor_init_crc64(&descriptor,
                                 src_ptr,
                                 length,
                                 polynomial,
                                 is_be_bit_order,
                                 is_inverse);

    return util::process_descriptor<crc_operation_result_t, util::execution_mode_t::sync>(&descriptor,
                                                                                          &completion_record,
                                                                                          numa_id);
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

template <>
auto call_crc<execution_path_t::software>(const uint8_t *src_ptr,
                                          uint32_t length,
                                          uint64_t polynomial,
                                          bool is_be_bit_order,
                                          bool is_inverse,
                                          int32_t UNREFERENCED_PARAMETER(numa_id)) noexcept -> crc_operation_result_t {
    crc_operation_result_t operation_result{};
    uint32_t               status_code = status_list::ok;

    operation_result.crc_             = perform_crc(src_ptr, length, polynomial, is_be_bit_order, is_inverse);
    operation_result.status_code_     = status_code;
    operation_result.processed_bytes_ = length;

    return operation_result;
}

template <>
auto call_crc<execution_path_t::auto_detect>(const uint8_t *src_ptr,
                                             uint32_t length,
                                             uint64_t polynomial,
                                             bool is_be_bit_order,
                                             bool is_inverse,
                                             int32_t numa_id) noexcept -> crc_operation_result_t {
    auto hw_result = call_crc<execution_path_t::hardware>(src_ptr,
                                                          length,
                                                          polynomial,
                                                          is_be_bit_order,
                                                          is_inverse,
                                                          numa_id);

    if (hw_result.status_code_ != status_list::ok) {
        return call_crc<execution_path_t::software>(src_ptr,
                                                    length,
                                                    polynomial,
                                                    is_be_bit_order,
                                                    is_inverse);
    }

    return hw_result;
}

} // namespace qpl::ml::other
