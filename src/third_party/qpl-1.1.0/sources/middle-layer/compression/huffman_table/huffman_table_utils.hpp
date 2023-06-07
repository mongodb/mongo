/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/*
 *  Intel® Query Processing Library (Intel® QPL)
 *  Middle-level layer
 */

#ifndef QPL_MIDDLE_LAYER_COMPRESSION_CANNED_UTILS_HPP
#define QPL_MIDDLE_LAYER_COMPRESSION_CANNED_UTILS_HPP

#include <cstddef>
#include <type_traits>

#include "qpl/c_api/huffman_table.h"

#include "common/defs.hpp"

#include "deflate_huffman_table.hpp"
#include "inflate_huffman_table.hpp"

#include "util/util.hpp"

/**
 * Flag which indicates whenever hardware representation of compression/decompression table should be used
 */
#define QPL_HW_REPRESENTATION            0x01u

/**
 * Flag which indicates whenever deflate header should be used
 */
#define QPL_DEFLATE_REPRESENTATION       0x04u

/**
 * Flag which indicates whenever software representation of compression/decompression table should be used
 */
#define QPL_SW_REPRESENTATION            0x08u

/**
 * Flag which indicates whenever huffman only representation of compression/decompression table should be used
 */
#define QPL_HUFFMAN_ONLY_REPRESENTATION  0x10u

/**
 * Combine all (software, hardware, deflate) representation flags to build the complete compression table
 */
#define QPL_COMPLETE_COMPRESSION_TABLE (QPL_HW_REPRESENTATION | QPL_DEFLATE_REPRESENTATION | QPL_SW_REPRESENTATION)
/** @} */

/**
 * @brief Structure that represents information that is required for compression
 */
struct qpl_compression_huffman_table {
    /**
    * Buffer that contains Intel QPL representation of the software compression table
    */
    std::aligned_storage_t<sizeof(qplc_huffman_table_default_format),
                           qpl::ml::util::default_alignment> sw_compression_table_data;

    /**
    * Buffer that contains ISA-L representation of the software compression table
    */
    std::aligned_storage_t<sizeof(isal_hufftables),
                           qpl::ml::util::default_alignment> isal_compression_table_data;

    /**
    * Buffer that contains representation of the hardware compression table
    * @note currently this is just a stab, this field is not actually used anywhere
    */
    std::aligned_storage_t<sizeof(qpl::ml::compression::hw_compression_huffman_table),
                           qpl::ml::util::default_alignment> hw_compression_table_data;

    /**
    * Buffer that contains information about deflate header
    */
    std::aligned_storage_t<sizeof(qpl::ml::compression::deflate_header),
                           qpl::ml::util::default_alignment> deflate_header_buffer;

    /**
    * Flag that indicates which representation is used. Possible values are (or their combinations):
    * QPL_HW_REPRESENTATION
    * QPL_SW_REPRESENTATION
    * QPL_DEFLATE_REPRESENTATION
    */
    uint32_t representation_mask;
};

/**
* @brief Structure that represents information that is required for decompression
*/
struct qpl_decompression_huffman_table {
    /**
    * Buffer that contains representation of the software decompression table
    */
    std::aligned_storage_t<sizeof(qplc_huffman_table_flat_format),
                           qpl::ml::util::default_alignment> sw_flattened_table;

    /**
    * Buffer that contains representation of the hardware compression table
    * @note currently this is just a stab, this field is not actually used anywhere
    */
    std::aligned_storage_t<sizeof(qpl::ml::compression::hw_decompression_state),
                           HW_PATH_STRUCTURES_REQUIRED_ALIGN> hw_decompression_state;

    /**
    * Buffer that contains information about deflate header
    */
    std::aligned_storage_t<sizeof(qpl::ml::compression::deflate_header),
                           qpl::ml::util::default_alignment> deflate_header_buffer;

    /**
    * Flag that indicates which representation is used. Possible values are (or their combinations):
    * QPL_HW_REPRESENTATION
    * QPL_SW_REPRESENTATION
    * QPL_DEFLATE_REPRESENTATION
    */
    uint32_t representation_mask;

    /**
    * This field is used for canned mode only (software path). Contains lookup table for further decompression.
    */
    std::aligned_storage_t<sizeof(qpl::ml::compression::canned_table),
                           qpl::ml::util::default_alignment> lookup_table_buffer;
};

// todo: clean up the functions from the list below that are not used anywhere
extern "C" {
uint8_t *get_lookup_table_buffer_ptr(qpl_decompression_huffman_table *decompression_table_ptr);

uint32_t *get_literals_lengths_table_ptr(qpl_compression_huffman_table *const huffman_table_ptr);
uint32_t *get_offsets_table_ptr(qpl_compression_huffman_table *const huffman_table_ptr);

uint8_t *get_deflate_header_ptr(qpl_compression_huffman_table *const huffman_table_ptr);
uint32_t get_deflate_header_bits_size(qpl_compression_huffman_table *const huffman_table_ptr);
void set_deflate_header_bits_size(qpl_compression_huffman_table *const huffman_table_ptr, uint32_t header_bits);

uint8_t *get_isal_compression_huffman_table_ptr(qpl_compression_huffman_table *const huffman_table_ptr);

uint8_t *get_sw_decompression_table_buffer(qpl_decompression_huffman_table *const decompression_table_ptr);
uint8_t *get_hw_decompression_table_buffer(qpl_decompression_huffman_table *const decompression_table_ptr);
uint8_t *get_deflate_header_buffer(qpl_decompression_huffman_table *const decompression_table_ptr);

bool is_sw_representation_used(qpl_decompression_huffman_table *const decompression_table_ptr);
bool is_hw_representation_used(qpl_decompression_huffman_table *const decompression_table_ptr);
bool is_deflate_representation_used(qpl_decompression_huffman_table *const decompression_table_ptr);
}

namespace qpl::ml::compression {

struct qpl_triplet {
    uint8_t character;
    uint8_t code_length;
    uint16_t code;
};

template<class table_t>
auto huffman_table_init(table_t &table,
                        const qpl_triplet *const triplets_ptr,
                        const std::size_t triplets_count,
                        const uint32_t representation_flags = 0u) noexcept -> qpl_ml_status;

template<class table_t>
auto huffman_table_init(table_t &table,
                        const uint32_t *literals_lengths_histogram_ptr,
                        const uint32_t *distances_histogram_ptr,
                        const uint32_t representation_flags = 0u) noexcept -> qpl_ml_status;

template<class table_t>
auto huffman_table_init_with_stream(table_t &table,
                                    const uint8_t *const buffer,
                                    const uint32_t representation_flags) noexcept -> qpl_ml_status;

template<class first_table_t, class second_table_t>
auto huffman_table_convert(const first_table_t &first_table,
                           second_table_t &second_table,
                           const uint32_t representation_flags = 0u) noexcept -> qpl_ml_status;

template<class table_t>
auto huffman_table_write_to_stream(const table_t &table,
                                   uint8_t *const buffer,
                                   const uint32_t representation_flags) noexcept -> qpl_ml_status;

template<class first_table_t, class second_table_t>
bool is_equal(first_table_t &first_table, second_table_t &second_table) noexcept;

namespace details {

static inline auto get_path_flags(execution_path_t path) {
    switch (path) {
        case execution_path_t::hardware:
            return QPL_HW_REPRESENTATION;
        case execution_path_t::software:
            return QPL_SW_REPRESENTATION;
        default:
            return QPL_COMPLETE_COMPRESSION_TABLE;
    }
}

static inline auto get_allocator(const allocator_t allocator) {
    constexpr allocator_t default_allocator = DEFAULT_ALLOCATOR_C;
    return (allocator.allocator && allocator.deallocator) ? allocator : default_allocator;
}

} // namespace details
} // namespace qpl::ml::compression

#endif // QPL_MIDDLE_LAYER_COMPRESSION_CANNED_UTILS_HPP
