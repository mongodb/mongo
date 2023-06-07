/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring> // memcmp

#include "common/bit_reverse.hpp"
#include "common/allocation_buffer_t.hpp"

#include "compression/deflate/utils/compression_defs.hpp"
#include "compression/huffman_table/deflate_huffman_table.hpp"
#include "compression/huffman_table/inflate_huffman_table.hpp"
#include "compression/huffman_table/huffman_table_utils.hpp"
#include "compression/huffman_table/serialization_utils.hpp"

#include "util/util.hpp"
#include "util/memory.hpp"
#include "util/descriptor_processing.hpp"

#include "qplc_compression_consts.h"
#include "qplc_huffman_table.h"
#include "deflate_histogram.h"

#include "compression/inflate/inflate.hpp"
#include "compression/inflate/inflate_state.hpp"

namespace qpl::ml::compression {

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstack-usage=4096"
#endif

namespace details {

static_assert(sizeof(qplc_huffman_table_default_format) <=
              sizeof(qpl_compression_huffman_table::sw_compression_table_data));

static const uint8_t match_length_codes_bases[29] = {
        0x00, 0x01, 0x02, 0x03,
        0x04, 0x05, 0x06, 0x07,
        0x08, 0x0A, 0x0C, 0x0E,
        0x10, 0x14, 0x18, 0x1C,
        0x20, 0x28, 0x30, 0x38,
        0x40, 0x50, 0x60, 0x70,
        0x80, 0xA0, 0xC0, 0xE0,
        0xFF
};

static const uint8_t match_lengths_extra_bits[29] = {
        0, 0, 0, 0,
        0, 0, 0, 0,
        1, 1, 1, 1,
        2, 2, 2, 2,
        3, 3, 3, 3,
        4, 4, 4, 4,
        5, 5, 5, 5,
        0
};

struct own_huffman_code {
    uint16_t code;               /**< Huffman code */
    uint8_t  extra_bit_count;    /**< Number of extra bits */
    uint8_t  length;             /**< Huffman code length */
};

static inline auto validate_representation_flags(const compression_huffman_table &compression_table,
                                                 decompression_huffman_table &decompression_table) noexcept -> qpl_ml_status {
    if (decompression_table.is_deflate_header_used() &&
        !compression_table.is_deflate_header_used()) {
        return status_list::status_invalid_params;
    }

    if (decompression_table.is_sw_decompression_table_used() &&
        !compression_table.is_sw_compression_table_used()) {
        return status_list::status_invalid_params;
    }

    if (decompression_table.is_hw_decompression_table_used() &&
        !compression_table.is_hw_compression_table_used()) {
        return status_list::status_invalid_params;
    }

    return status_list::ok;
}

static inline void create_code_tables(uint16_t *const code_table_ptr,
                                      uint8_t *const code_length_table_ptr,
                                      const uint32_t length,
                                      const struct own_huffman_code *const huffman_table_ptr) {
    for (uint32_t i = 0; i < length; i++) {
        code_table_ptr[i]        = huffman_table_ptr[i].code;
        code_length_table_ptr[i] = huffman_table_ptr[i].length;
    }
}

static inline void create_packed_match_lengths_table(uint32_t *const packed_table_ptr,
                                                     const struct own_huffman_code *const huffman_table_ptr) {
    // Variables
    uint8_t  count            = 0;
    uint16_t extra_bits_count = 0;
    uint16_t gain_extra_bits  = QPLC_DEFLATE_EXTRA_BITS_START_POSITION;

    // Main cycle
    for (uint32_t i = 257; i < QPLC_DEFLATE_LL_TABLE_SIZE - 1; i++) {
        for (uint16_t extra_bits = 0; extra_bits < (1u << extra_bits_count); extra_bits++) {
            if (count > 254) {
                break;
            }

            packed_table_ptr[count++] = (extra_bits << (huffman_table_ptr[i].length + QPLC_CODE_LENGTH_BIT_LENGTH)) |
                                        (huffman_table_ptr[i].code << QPLC_CODE_LENGTH_BIT_LENGTH) |
                                        (huffman_table_ptr[i].length + extra_bits_count);
        }

        if (i == gain_extra_bits) {
            gain_extra_bits += QPLC_DEFLATE_LENGTH_EXTRA_BITS_INTERVAL;
            extra_bits_count += 1;
        }
    }

    packed_table_ptr[count] =
            (huffman_table_ptr[QPLC_DEFLATE_LL_TABLE_SIZE - 1].code << QPLC_CODE_LENGTH_BIT_LENGTH) |
            (huffman_table_ptr[QPLC_DEFLATE_LL_TABLE_SIZE - 1].length);
}

static inline void create_packed_offset_table(uint32_t *const packed_table_ptr,
                                              const uint32_t length,
                                              const struct own_huffman_code *const huffman_table_ptr) {
    // Variables
    uint32_t count            = 0;
    uint16_t extra_bits_count = 0;
    uint16_t gain_extra_bits  = QPLC_DEFLATE_EXTRA_OFFSETS_BEGIN_VALUE;

    // Main cycle
    for (uint32_t i = 0; i < QPLC_DEFLATE_OFFSETS_COUNT; i++) {
        for (uint16_t extra_bits = 0; extra_bits < (1u << extra_bits_count); extra_bits++) {
            if (count >= length) {
                return;
            }

            packed_table_ptr[count++] = (extra_bits << (huffman_table_ptr[i].length + QPLC_CODE_LENGTH_BIT_LENGTH)) |
                                        (huffman_table_ptr[i].code << QPLC_CODE_LENGTH_BIT_LENGTH) |
                                        (huffman_table_ptr[i].length + extra_bits_count);
        }

        if (i == gain_extra_bits) {
            gain_extra_bits += QPLC_DEFLATE_OFFSETS_EXTRA_BITS_INTERVAL;
            extra_bits_count += 1;
        }
    }
}

static inline void fill_histogram(const uint32_t *literals_lengths_histogram_ptr,
                                  const uint32_t *distances_histogram_ptr,
                                  isal_huff_histogram *histogram) {
    for (uint32_t i = 0u; i < QPLC_DEFLATE_LL_TABLE_SIZE; i++) {
        histogram->lit_len_histogram[i] = static_cast<uint32_t>(literals_lengths_histogram_ptr[i]);
    }

    for (uint32_t i = 0u; i < QPLC_DEFLATE_D_TABLE_SIZE; i++) {
        histogram->dist_histogram[i] = static_cast<uint32_t>(distances_histogram_ptr[i]);
    }
}

static inline void fill_histogram_literals_only(const uint32_t *literals_lengths_histogram_ptr,
                                                isal_huff_histogram *histogram) {
    for (uint32_t i = 0u; i < QPLC_LITERALS_COUNT; i++) {
        histogram->lit_len_histogram[i] = static_cast<uint32_t>(literals_lengths_histogram_ptr[i]);
    }
}

static inline void store_isal_deflate_header(isal_hufftables *isal_huffman_table,
                                             compression_huffman_table &compression_table) noexcept {
    auto header_complete_byte_size = isal_huffman_table->deflate_hdr_count;
    header_complete_byte_size += (0u == isal_huffman_table->deflate_hdr_extra_bits) ? 0u : 1u;

    // Use copy kernel to copy deflate header from isal huffman tables
    auto copy_kernel = dispatcher::kernels_dispatcher::get_instance().get_memory_copy_table();
    copy_kernel[0]((uint8_t *) isal_huffman_table->deflate_hdr,
                   compression_table.get_deflate_header_data(),
                   header_complete_byte_size);

    // Calculate and store deflate header bits size
    const auto header_bit_size = isal_huffman_table->deflate_hdr_count * byte_bits_size
                                 + (isal_huffman_table->deflate_hdr_extra_bits);
    compression_table.set_deflate_header_bit_size(header_bit_size);
}

static inline void qpl_huffman_table_to_isal(const qpl_compression_huffman_table *qpl_table_ptr,
                                             struct isal_hufftables *const isal_table_ptr,
                                             const endianness_t endian) {
    auto table_ptr = const_cast<qpl_compression_huffman_table *>(qpl_table_ptr);

    // Variables
    const uint32_t qpl_code_mask     = (1u << QPLC_HUFFMAN_CODE_BIT_LENGTH) - 1u;
    // First 15 bits [14:0]
    const uint32_t qpl_length_mask   = QPLC_HUFFMAN_CODE_LENGTH_MASK << QPLC_HUFFMAN_CODE_BIT_LENGTH; // Bits [18:15]
    uint32_t       header_byte_size  = 0;
    uint32_t       header_extra_bits = 0;

    struct own_huffman_code literals_matches_table[QPLC_DEFLATE_LL_TABLE_SIZE] = {{0u, 0u, 0u}};
    struct own_huffman_code offsets_huffman_table[QPLC_DEFLATE_D_TABLE_SIZE]   = {{0u, 0u, 0u}};

    // Memory initialization
    qpl::ml::util::set_zeros((uint8_t *) isal_table_ptr, sizeof(struct isal_hufftables));

    // Copying literals and match lengths Huffman table
    for (uint32_t i = 0; i < QPLC_DEFLATE_LL_TABLE_SIZE; i++) {
        const auto code   = static_cast<uint16_t>(get_literals_lengths_table_ptr(table_ptr)[i] & qpl_code_mask);
        const auto length = static_cast<uint8_t>((get_literals_lengths_table_ptr(table_ptr)[i] & qpl_length_mask)
                >> 15u);

        literals_matches_table[i].length = length;
        literals_matches_table[i].code   = endian == little_endian ? reverse_bits(code, length) : code;
    }

    // Copying offsets Huffman table
    for (uint32_t i = 0; i < QPLC_DEFLATE_D_TABLE_SIZE; i++) {
        const auto code   = static_cast<uint16_t>(get_offsets_table_ptr(table_ptr)[i] & qpl_code_mask);
        const auto length = static_cast<uint8_t>((get_offsets_table_ptr(table_ptr)[i] & qpl_length_mask) >> 15u);

        offsets_huffman_table[i].length = length;
        offsets_huffman_table[i].code   = endian == little_endian ? reverse_bits(code, length) : code;
    }

    // Generating ISA-L tables
    create_code_tables(isal_table_ptr->dcodes,
                       isal_table_ptr->dcodes_sizes,
                       QPLC_DEFLATE_D_TABLE_SIZE,
                       offsets_huffman_table);
    create_code_tables(isal_table_ptr->lit_table,
                       isal_table_ptr->lit_table_sizes,
                       QPLC_DEFLATE_LITERALS_COUNT,
                       literals_matches_table);

    create_packed_match_lengths_table(isal_table_ptr->len_table, literals_matches_table);
    create_packed_offset_table(isal_table_ptr->dist_table, QPLC_OFFSET_TABLE_SIZE, offsets_huffman_table);

    // Setting header information
    header_extra_bits = get_deflate_header_bits_size(table_ptr) % 8;
    header_byte_size  = (get_deflate_header_bits_size(table_ptr) / 8) + (header_extra_bits == 0 ? 0 : 1);

    isal_table_ptr->deflate_hdr_count      = get_deflate_header_bits_size(table_ptr) / 8;
    isal_table_ptr->deflate_hdr_extra_bits = header_extra_bits;

    uint8_t *deflate_header_ptr = get_deflate_header_ptr(table_ptr);

    for (uint32_t i = 0; i < header_byte_size; i++) {
        isal_table_ptr->deflate_hdr[i] = deflate_header_ptr[i];
    }

    // Forcedly set final bit of header, ISA-L will reset it if current block not final
    isal_table_ptr->deflate_hdr[0] |= 1u;
}

static inline auto initialize_inflate_state_from_deflate_header(uint8_t *deflate_header_data_ptr,
                                                                uint32_t deflate_header_bit_size,
                                                                isal_inflate_state *isal_state_ptr) noexcept -> qpl_ml_status {
    // Prepare inflate state to parse Deflate header
    constexpr auto end_processing_condition = end_processing_condition_t::stop_and_check_for_bfinal_eob;

    uint32_t deflate_header_byte_size = util::bit_to_byte(deflate_header_bit_size);

    isal_state_ptr->next_in   = deflate_header_data_ptr;
    isal_state_ptr->avail_in  = deflate_header_byte_size;
    isal_state_ptr->next_out  = deflate_header_data_ptr; // No rewrites
    isal_state_ptr->avail_out = 0u;

    allocation_buffer_t buffer(reinterpret_cast<uint8_t *>(isal_state_ptr),
                               reinterpret_cast<uint8_t *>(isal_state_ptr) + sizeof(isal_inflate_state));

    auto state = inflate_state<qpl::ml::execution_path_t::software>::create(buffer);

    auto status = inflate<execution_path_t::software>(state, end_processing_condition).status_code_;

    // This is work-around, current inflate function can perform not just deflate header decompression
    // but go further and perform decompression of deflate block, which may cause the following error. Ignore it.
    // TODO: fix
    if (status_list::compression_reference_before_start == status) {
        status = status_list::ok;
    }

    isal_state_ptr->tmp_out_valid = 0;

    return status;
}

static inline auto triplets_to_sw_compression_table(const qpl_triplet *triplets_ptr,
                                                    std::size_t triplets_count,
                                                    qplc_huffman_table_default_format *compression_table) -> qpl_ml_status {
    for (std::size_t i = 0; i < triplets_count; i++) {
        qpl_triplet current_triplet = triplets_ptr[i];

        uint32_t literal_length_table_index = current_triplet.character;

        qplc_huffman_table_write_ll_code(compression_table, literal_length_table_index, current_triplet.code);
        qplc_huffman_table_write_ll_code_length(compression_table,
                                                literal_length_table_index,
                                                current_triplet.code_length);
    }

    return status_list::ok;
}

static inline auto triplets_code_values_comparator(const void *a, const void *b) noexcept -> int {
    auto first_triplet  = reinterpret_cast<const qpl_triplet *>(a);
    auto second_triplet = reinterpret_cast<const qpl_triplet *>(b);

    return (int) first_triplet->code > second_triplet->code;
}

static inline void triplets_to_sw_decompression_table(const qpl_triplet *triplets_ptr,
                                                      size_t triplets_count,
                                                      qplc_huffman_table_flat_format *decompression_table_ptr) noexcept {
    // Variables
    uint32_t empty_position = 0u;

    // Calculate code lengths histogram
    std::for_each(triplets_ptr, triplets_ptr + triplets_count,
                  [decompression_table_ptr](const qpl_triplet &item) {
                      if (item.code_length != 0) {
                          decompression_table_ptr->number_of_codes[item.code_length - 1u]++;
                      }
                  });

    // Calculate first codes
    for (uint32_t i = 1u; i <= 15u; i++) {
        std::array<qpl_triplet, 256> filtered{};

        if (decompression_table_ptr->number_of_codes[i - 1u] == 0) {
            continue;
        }

        // Filtering by code length
        const auto last_filtered = std::copy_if(triplets_ptr,
                                                triplets_ptr + triplets_count,
                                                filtered.begin(),
                                                [i](const qpl_triplet triplet) {
                                                    return triplet.code_length == i;
                                                });

        // Sorting to get the right order for mapping table (charToSortedCode)
        size_t number_of_elements_to_sort = std::distance(filtered.begin(), last_filtered);
        qsort(filtered.data(), number_of_elements_to_sort, sizeof(qpl_triplet), triplets_code_values_comparator);

        decompression_table_ptr->first_codes[i - 1u]         = filtered[0].code;
        decompression_table_ptr->first_table_indexes[i - 1u] = empty_position;

        // Writing of sorted codes
        const uint32_t start_position = empty_position;

        while (empty_position < (start_position + std::distance(filtered.begin(), last_filtered))) {
            decompression_table_ptr->index_to_char[empty_position] = filtered[empty_position -
                                                                              start_position].character;
            empty_position++;
        }
    }
}

static inline void convert_software_tables(qplc_huffman_table_default_format *compression_table_ptr,
                                           qplc_huffman_table_flat_format *decompression_table_ptr) noexcept {
    std::array<qpl_triplet, 256u> triplets_array = {};

    for (uint32_t i = 0; i < 256u; i++) {
        triplets_array[i].character   = i;
        triplets_array[i].code_length = qplc_huffman_table_get_ll_code_length(compression_table_ptr, i);
        triplets_array[i].code        = qplc_huffman_table_get_ll_code(compression_table_ptr, i);
    }

    triplets_to_sw_decompression_table(triplets_array.data(), triplets_array.size(), decompression_table_ptr);
}

static inline void isal_compression_table_to_qpl(const isal_hufftables *isal_table_ptr,
                                                 qplc_huffman_table_default_format *qpl_table_ptr) noexcept {
    // Variables
    const auto isal_match_lengths_mask = util::build_mask<uint16_t, 15u>();

    // Convert literals codes
    for (uint32_t i = 0; i < QPLC_DEFLATE_LITERALS_COUNT; i++) {
        const uint16_t code   = isal_table_ptr->lit_table[i];
        const uint8_t  length = isal_table_ptr->lit_table_sizes[i];

        qpl_table_ptr->literals_matches[i] = reverse_bits(code, length) | (uint32_t) (length << 15u);
    }

    // Convert match lengths codes
    for (uint32_t i = 0; i < QPLC_DEFLATE_MATCHES_COUNT; i++) {
        const uint16_t code   = isal_table_ptr->len_table[details::match_length_codes_bases[i]] >> 5u;
        uint8_t        length = isal_table_ptr->len_table[details::match_length_codes_bases[i]]
                                & isal_match_lengths_mask;

        // Normally, (in all cases except for huffman only) ISAL assignes code for every match length token, but
        // this can be a huffman only table, without match lengths codes, so additionaly check if code length is more than zero
        // to prevent the overflow of code's length
        if (0u != length) {
            length -= details::match_lengths_extra_bits[i];
            qpl_table_ptr->literals_matches[i + QPLC_DEFLATE_LITERALS_COUNT] =
                    reverse_bits(code, length) | (uint32_t) (length << 15u);
        } else {
            // Write zero otherwise
            qpl_table_ptr->literals_matches[i + QPLC_DEFLATE_LITERALS_COUNT] = 0u;
        }
    }

    // Convert offsets codes
    for (uint32_t i = 0; i < QPLC_DEFLATE_OFFSETS_COUNT; i++) {
        const uint16_t code   = isal_table_ptr->dcodes[i];
        const uint8_t  length = isal_table_ptr->dcodes_sizes[i];

        qpl_table_ptr->offsets[i] = reverse_bits(code, length) | (uint32_t) (length << 15u);
    }
}

static inline auto build_compression_table(const uint32_t *literals_lengths_histogram_ptr,
                                           const uint32_t *distances_histogram_ptr,
                                           compression_huffman_table &compression_table) noexcept -> qpl_ml_status {
    if (compression_table.is_huffman_only() &&
        compression_table.is_deflate_header_used()) {
        return status_list::status_invalid_params;
    }

    if (compression_table.is_hw_compression_table_used()) {
        // HW table format is equal to SW table
        compression_table.enable_sw_compression_table();

        auto increment_zeros = [](const uint32_t &n) {
            const_cast<uint32_t &>(n) += (n == 0) ? 1u : 0u;
        };

        std::for_each(literals_lengths_histogram_ptr,
                      literals_lengths_histogram_ptr + ISAL_DEF_LIT_LEN_SYMBOLS,
                      increment_zeros);

        std::for_each(distances_histogram_ptr,
                      distances_histogram_ptr + ISAL_DEF_DIST_SYMBOLS,
                      increment_zeros);
    }

    if (compression_table.is_sw_compression_table_used() ||
        compression_table.is_deflate_header_used()) {
        // Create isal huffman table and histograms
        isal_huff_histogram histogram = {{0u},
                                         {0u},
                                         {0u}};

        if (compression_table.is_huffman_only()) {
            // Copy literals (except for EOB symbol) histogram to ISAL histogram
            details::fill_histogram_literals_only(literals_lengths_histogram_ptr, &histogram);

            // Main pipeline here, use ISAL to create huffman tables
            isal_create_hufftables_literals_only(compression_table.get_isal_compression_table(), &histogram);
            compression_table.set_deflate_header_bit_size(0);
        } else {
            // Fill isal histogram from the given one
            details::fill_histogram(literals_lengths_histogram_ptr, distances_histogram_ptr, &histogram);

            // Main pipeline here, use ISAL to create huffman tables
            isal_create_hufftables(compression_table.get_isal_compression_table(), &histogram);

        }
        // Store huffman codes if required
        if (compression_table.is_sw_compression_table_used()) {
            isal_compression_table_to_qpl(compression_table.get_isal_compression_table(),
                                          compression_table.get_sw_compression_table());
        }

        // Store deflate header content if required
        if (compression_table.is_deflate_header_used()) {
            details::store_isal_deflate_header(compression_table.get_isal_compression_table(), compression_table);
        }
    }

    return status_list::ok;
}

static inline auto comp_to_decompression_table(const compression_huffman_table &compression_table,
                                               decompression_huffman_table &decompression_table) noexcept -> qpl_ml_status {
    auto validation_status = details::validate_representation_flags(compression_table, decompression_table);

    if (status_list::ok != validation_status) {
        return validation_status;
    }

    if (decompression_table.is_deflate_header_used()) {
        // Copy deflate header from compression to decompression table
        auto deflate_header_byte_size = util::bit_to_byte(compression_table.get_deflate_header_bit_size());

        util::copy(compression_table.get_deflate_header_data(),
                   compression_table.get_deflate_header_data() + deflate_header_byte_size,
                   decompression_table.get_deflate_header_data());

        decompression_table.set_deflate_header_bit_size(compression_table.get_deflate_header_bit_size());

        isal_inflate_state temporary_state = {nullptr, 0u, 0u, nullptr, 0u, 0u, 0, {{0u}, {0u}},
                                              {{0u}, {0u}}, (isal_block_state) 0, 0u, 0u, 0u, 0u, 0u,
                                              0u, 0, 0, 0, 0, 0u, 0, 0, 0, 0, {0u}, {0u}, 0u, 0u, 0u};

        // Parse deflate header and load it into the temporary state
        auto status =
                     details::initialize_inflate_state_from_deflate_header(decompression_table.get_deflate_header_data(),
                                                                           decompression_table.get_deflate_header_bit_size(),
                                                                           &temporary_state);

        // Copy lookup tables from temporary state to decompression table
        auto *lit_huff_code_ptr = reinterpret_cast<uint8_t *>(&temporary_state.lit_huff_code);

        util::copy(lit_huff_code_ptr,
                   lit_huff_code_ptr + sizeof(temporary_state.lit_huff_code),
                   reinterpret_cast<uint8_t *>(&decompression_table.get_canned_table()->literal_huffman_codes));

        auto *dist_huff_code_ptr = reinterpret_cast<uint8_t *>(&temporary_state.dist_huff_code);

        util::copy(dist_huff_code_ptr,
                   dist_huff_code_ptr + sizeof(temporary_state.dist_huff_code),
                   reinterpret_cast<uint8_t *>(&decompression_table.get_canned_table()->distance_huffman_codes));

        // Copy eob symbol properties
        decompression_table.get_canned_table()->eob_code_and_len = temporary_state.eob_code_and_len;

        decompression_table.get_canned_table()->is_final_block = (temporary_state.bfinal == 1);

        if (status_list::ok != status) {
            return status;
        }
    }

    if (decompression_table.is_sw_decompression_table_used()) {
        details::convert_software_tables(compression_table.get_sw_compression_table(),
                                         decompression_table.get_sw_decompression_table());
    }

    if (decompression_table.is_hw_decompression_table_used()) {
        hw_descriptor HW_PATH_ALIGN_STRUCTURE descriptor;
        HW_PATH_VOLATILE hw_completion_record HW_PATH_ALIGN_STRUCTURE completion_record;

        std::fill(descriptor.data, descriptor.data + HW_PATH_DESCRIPTOR_SIZE, 0u);

        auto *const header_ptr = compression_table.get_deflate_header_data();
        const auto header_bit_size = compression_table.get_deflate_header_bit_size();
        hw_iaa_aecs *const aecs_ptr = decompression_table.get_hw_decompression_state();

        util::set_zeros(aecs_ptr, sizeof(hw_iaa_aecs_analytic));

        uint32_t input_bytes_count = (header_bit_size + 7u) >> 3u;
        uint8_t  ignore_end_bits   = max_bit_index & (0u - header_bit_size);

        hw_iaa_descriptor_set_input_buffer(&descriptor, header_ptr, input_bytes_count);

        hw_iaa_descriptor_init_inflate_header(&descriptor,
                                              reinterpret_cast<hw_iaa_aecs_analytic *>(aecs_ptr),
                                              ignore_end_bits,
                                              hw_aecs_toggle_rw);

        hw_iaa_descriptor_set_completion_record(&descriptor, &completion_record);

        return ml::util::process_descriptor<qpl_ml_status,
                                            ml::util::execution_mode_t::sync>(&descriptor,
                                                                              &completion_record);
    }

    return status_list::ok;
}

static inline auto init_compression_table_with_stream(const uint8_t *const buffer,
                                                      compression_huffman_table compression_table) noexcept -> qpl_ml_status {
    using namespace qpl::ml::serialization;

    uint8_t *src = const_cast<uint8_t *>(buffer); // adding an offset internally

    deserialize_table(src, *compression_table.get_sw_compression_table());   src += flatten_table_size(*compression_table.get_sw_compression_table());
    deserialize_table(src, *compression_table.get_isal_compression_table()); src += flatten_table_size(*compression_table.get_isal_compression_table());
    deserialize_table(src, *compression_table.get_hw_compression_table());   src += flatten_table_size(*compression_table.get_hw_compression_table());
    deserialize_table(src, *compression_table.get_deflate_header());

    return status_list::ok;
}

static inline auto init_decompression_table_with_stream(const uint8_t *const buffer,
                                                        decompression_huffman_table decompression_table) noexcept -> qpl_ml_status {
    using namespace qpl::ml::serialization;

    uint8_t *src = const_cast<uint8_t *>(buffer); // adding an offset internally

    deserialize_table(src, *decompression_table.get_sw_decompression_table()); src += flatten_table_size(*decompression_table.get_sw_decompression_table());
    deserialize_table(src, *decompression_table.get_hw_decompression_state()); src += flatten_table_size(*decompression_table.get_hw_decompression_state());
    deserialize_table(src, *decompression_table.get_deflate_header());         src += flatten_table_size(*decompression_table.get_deflate_header());
    deserialize_table(src, *decompression_table.get_canned_table());

    return status_list::ok;
}

static inline auto write_compression_table_to_stream(uint8_t *const buffer,
                                                     compression_huffman_table compression_table) noexcept -> qpl_ml_status {
    using namespace qpl::ml::serialization;

    uint8_t *dst = buffer; // adding an offset internally

    serialize_table(*compression_table.get_sw_compression_table(), dst);   dst += flatten_table_size(*compression_table.get_sw_compression_table());
    serialize_table(*compression_table.get_isal_compression_table(), dst); dst += flatten_table_size(*compression_table.get_isal_compression_table());
    serialize_table(*compression_table.get_hw_compression_table(), dst);   dst += flatten_table_size(*compression_table.get_hw_compression_table());
    serialize_table(*compression_table.get_deflate_header(), dst);

    return status_list::ok;
}

static inline auto write_decompression_table_to_stream(uint8_t *const buffer,
                                                       decompression_huffman_table decompression_table) noexcept -> qpl_ml_status {
    using namespace qpl::ml::serialization;

    uint8_t *dst = buffer; // adding an offset internally

    serialize_table(*decompression_table.get_sw_decompression_table(), dst); dst += flatten_table_size(*decompression_table.get_sw_decompression_table());
    serialize_table(*decompression_table.get_hw_decompression_state(), dst); dst += flatten_table_size(*decompression_table.get_hw_decompression_state());
    serialize_table(*decompression_table.get_deflate_header(), dst);         dst += flatten_table_size(*decompression_table.get_deflate_header());
    serialize_table(*decompression_table.get_canned_table(), dst);

    return status_list::ok;
}


} // namespace details

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

} // namespace qpl::ml::compression

extern "C" {

uint8_t *get_sw_decompression_table_buffer(qpl_decompression_huffman_table *const decompression_table_ptr) {
    return reinterpret_cast<uint8_t *>(&decompression_table_ptr->sw_flattened_table);
}

uint8_t *get_hw_decompression_table_buffer(qpl_decompression_huffman_table *const decompression_table_ptr) {
    return reinterpret_cast<uint8_t *>(&decompression_table_ptr->hw_decompression_state);
}

uint8_t *get_deflate_header_buffer(qpl_decompression_huffman_table *const decompression_table_ptr) {
    return reinterpret_cast<uint8_t *>(&decompression_table_ptr->deflate_header_buffer);
}

bool is_sw_representation_used(qpl_decompression_huffman_table *const decompression_table_ptr) {
    return decompression_table_ptr->representation_mask & QPL_SW_REPRESENTATION ? true : false;
}

bool is_hw_representation_used(qpl_decompression_huffman_table *const decompression_table_ptr) {
    return decompression_table_ptr->representation_mask & QPL_HW_REPRESENTATION ? true : false;
}

bool is_deflate_representation_used(qpl_decompression_huffman_table *const decompression_table_ptr) {
    return decompression_table_ptr->representation_mask & QPL_DEFLATE_REPRESENTATION ? true : false;
}

uint8_t *get_lookup_table_buffer_ptr(qpl_decompression_huffman_table *decompression_table_ptr) {
    return reinterpret_cast<uint8_t *>(&decompression_table_ptr->lookup_table_buffer);
}

uint32_t *get_literals_lengths_table_ptr(qpl_compression_huffman_table *const huffman_table_ptr) {
    using namespace qpl::ml::compression;
    auto sw_compression_table =
                 reinterpret_cast<qplc_huffman_table_default_format *>(&huffman_table_ptr->sw_compression_table_data);

    return sw_compression_table->literals_matches;
}

uint32_t *get_offsets_table_ptr(qpl_compression_huffman_table *const huffman_table_ptr) {
    using namespace qpl::ml::compression;
    auto sw_compression_table =
                 reinterpret_cast<qplc_huffman_table_default_format *>(&huffman_table_ptr->sw_compression_table_data);

    return sw_compression_table->offsets;
}

uint8_t *get_deflate_header_ptr(qpl_compression_huffman_table *const huffman_table_ptr) {
    using namespace qpl::ml::compression;
    return reinterpret_cast<deflate_header *>(&huffman_table_ptr->deflate_header_buffer)->data;
}

uint32_t get_deflate_header_bits_size(qpl_compression_huffman_table *const huffman_table_ptr) {
    using namespace qpl::ml::compression;
    return reinterpret_cast<deflate_header *>(&huffman_table_ptr->deflate_header_buffer)->header_bit_size;
}

uint8_t *get_sw_compression_huffman_table_ptr(qpl_compression_huffman_table *const huffman_table_ptr) {
    return reinterpret_cast<uint8_t *>(&huffman_table_ptr->sw_compression_table_data);
}

uint8_t *get_isal_compression_huffman_table_ptr(qpl_compression_huffman_table *const huffman_table_ptr) {
    return reinterpret_cast<uint8_t *>(&huffman_table_ptr->isal_compression_table_data);
}

uint8_t *get_hw_compression_huffman_table_ptr(qpl_compression_huffman_table *const huffman_table_ptr) {
    return reinterpret_cast<uint8_t *>(&huffman_table_ptr->hw_compression_table_data);
}

void set_deflate_header_bits_size(qpl_compression_huffman_table *const huffman_table_ptr, uint32_t header_bits) {
    using namespace qpl::ml::compression;
    reinterpret_cast<deflate_header *>(&huffman_table_ptr->deflate_header_buffer)->header_bit_size = header_bits;
}
}

namespace qpl::ml::compression {

// --- Initialization functions group (triplets, histogram, other table) --- //

template <>
auto huffman_table_init(compression_huffman_table &table,
                        const qpl_triplet *const triplets_ptr,
                        const std::size_t triplets_count,
                        const uint32_t UNREFERENCED_PARAMETER(representation_flags)) noexcept -> qpl_ml_status {
    if (table.is_sw_compression_table_used()) {
        details::triplets_to_sw_compression_table(triplets_ptr,
                                                  triplets_count,
                                                  table.get_sw_compression_table());
    }

    if (table.get_hw_compression_table()) {
        details::triplets_to_sw_compression_table(triplets_ptr,
                                                  triplets_count,
                                                  table.get_sw_compression_table());
        // @todo implement one
        // just a stab there
    }

    return status_list::ok;
}

template <>
auto huffman_table_init(decompression_huffman_table &table,
                        const qpl_triplet *const triplets_ptr,
                        const std::size_t triplets_count,
                        const uint32_t UNREFERENCED_PARAMETER(representation_flags)) noexcept -> qpl_ml_status {
    if (table.is_sw_decompression_table_used()) {
        details::triplets_to_sw_decompression_table(triplets_ptr,
                                                    triplets_count,
                                                    table.get_sw_decompression_table());
    }

    if (table.is_hw_decompression_table_used()) {
        details::triplets_to_sw_decompression_table(triplets_ptr,
                                                    triplets_count,
                                                    table.get_sw_decompression_table());
        // @todo implement one
        // just a stab there
    }

    return status_list::ok;
}

template <>
auto huffman_table_init(qpl_compression_huffman_table &table,
                        const qpl_triplet *const triplets_ptr,
                        const std::size_t triplets_count,
                        const uint32_t representation_flags) noexcept -> qpl_ml_status {
    using namespace qpl::ml::compression;

    auto sw_compression_table_data_ptr   = reinterpret_cast<uint8_t *>(&table.sw_compression_table_data);
    auto isal_compression_table_data_ptr = reinterpret_cast<uint8_t *>(&table.sw_compression_table_data);
    auto hw_compression_table_data_ptr   = reinterpret_cast<uint8_t *>(&table.hw_compression_table_data);
    auto deflate_header_buffer_ptr       = reinterpret_cast<uint8_t *>(&table.deflate_header_buffer);

    compression_huffman_table compression_table(sw_compression_table_data_ptr,
                                                isal_compression_table_data_ptr,
                                                hw_compression_table_data_ptr,
                                                deflate_header_buffer_ptr);

    if (representation_flags & QPL_DEFLATE_REPRESENTATION) {
        return QPL_STS_INVALID_PARAM_ERR;
    }

    if (representation_flags & QPL_SW_REPRESENTATION) {
        compression_table.enable_sw_compression_table();
        table.representation_mask |= QPL_SW_REPRESENTATION;
    }

    if (representation_flags & QPL_HW_REPRESENTATION) {
        compression_table.enable_hw_compression_table();
        table.representation_mask |= QPL_HW_REPRESENTATION;
    }

    auto status = huffman_table_init(compression_table, triplets_ptr, triplets_count);

    return status;
}

template <>
auto huffman_table_init(qpl_decompression_huffman_table &table,
                        const qpl_triplet *const triplets_ptr,
                        const std::size_t triplets_count,
                        const uint32_t representation_flags) noexcept -> qpl_ml_status {
    using namespace qpl::ml::compression;

    auto sw_flattened_table_ptr     = reinterpret_cast<uint8_t *>(&table.sw_flattened_table);
    auto hw_decompression_state_ptr = reinterpret_cast<uint8_t *>(&table.hw_decompression_state);
    auto deflate_header_buffer_ptr  = reinterpret_cast<uint8_t *>(&table.deflate_header_buffer);
    auto lookup_table_buffer_ptr    = reinterpret_cast<uint8_t *>(&table.lookup_table_buffer);

    decompression_huffman_table decompression_table(sw_flattened_table_ptr,
                                                    hw_decompression_state_ptr,
                                                    deflate_header_buffer_ptr,
                                                    lookup_table_buffer_ptr);

    if (representation_flags & QPL_DEFLATE_REPRESENTATION) {
        return QPL_STS_INVALID_PARAM_ERR;
    }

    if (representation_flags & QPL_SW_REPRESENTATION) {
        decompression_table.enable_sw_decompression_table();
        table.representation_mask |= QPL_SW_REPRESENTATION;
    }

    if (representation_flags & QPL_HW_REPRESENTATION) {
        decompression_table.enable_hw_decompression_table();
        table.representation_mask |= QPL_HW_REPRESENTATION;
    }

    auto status = huffman_table_init(decompression_table, triplets_ptr, triplets_count);

    return status;
}

template <>
auto huffman_table_init(compression_huffman_table &table,
                        const uint32_t *literals_lengths_histogram_ptr,
                        const uint32_t *distances_histogram_ptr,
                        const uint32_t UNREFERENCED_PARAMETER(representation_flags)) noexcept -> qpl_ml_status {
    return details::build_compression_table(literals_lengths_histogram_ptr, distances_histogram_ptr, table);
}

template <>
auto huffman_table_init(decompression_huffman_table &UNREFERENCED_PARAMETER(table),
                        const uint32_t *UNREFERENCED_PARAMETER(literals_lengths_histogram_ptr),
                        const uint32_t *UNREFERENCED_PARAMETER(distances_histogram_ptr),
                        const uint32_t UNREFERENCED_PARAMETER(representation_flags)) noexcept -> qpl_ml_status {
    return status_list::internal_error;
}

template <>
auto huffman_table_init(qpl_compression_huffman_table &table,
                        const uint32_t *literals_lengths_histogram_ptr,
                        const uint32_t *distances_histogram_ptr,
                        const uint32_t representation_flags) noexcept -> qpl_ml_status {
    using namespace qpl::ml;
    using namespace qpl::ml::compression;

    auto sw_compression_table_data_ptr   = reinterpret_cast<uint8_t *>(&table.sw_compression_table_data);
    auto isal_compression_table_data_ptr = reinterpret_cast<uint8_t *>(&table.isal_compression_table_data);
    auto hw_compression_table_data_ptr   = reinterpret_cast<uint8_t *>(&table.hw_compression_table_data);
    auto deflate_header_buffer_ptr       = reinterpret_cast<uint8_t *>(&table.deflate_header_buffer);

    compression_huffman_table compression_table(sw_compression_table_data_ptr,
                                                isal_compression_table_data_ptr,
                                                hw_compression_table_data_ptr,
                                                deflate_header_buffer_ptr);

    if (representation_flags & QPL_DEFLATE_REPRESENTATION) {
        compression_table.enable_deflate_header();
        table.representation_mask |= QPL_DEFLATE_REPRESENTATION;
    }

    if (representation_flags & QPL_SW_REPRESENTATION) {
        compression_table.enable_sw_compression_table();
        table.representation_mask |= QPL_SW_REPRESENTATION;
    }

    if (representation_flags & QPL_HW_REPRESENTATION) {
        compression_table.enable_hw_compression_table();
        table.representation_mask |= QPL_HW_REPRESENTATION;
    }

    if (representation_flags & QPL_HUFFMAN_ONLY_REPRESENTATION) {
        compression_table.make_huffman_only();
        table.representation_mask |= QPL_HUFFMAN_ONLY_REPRESENTATION;
    }

    return details::build_compression_table(literals_lengths_histogram_ptr, distances_histogram_ptr, compression_table);
}

template <>
auto huffman_table_init(qpl_decompression_huffman_table &UNREFERENCED_PARAMETER(table),
                        const uint32_t *UNREFERENCED_PARAMETER(literals_lengths_histogram_ptr),
                        const uint32_t *UNREFERENCED_PARAMETER(distances_histogram_ptr),
                        const uint32_t UNREFERENCED_PARAMETER(representation_flags)) noexcept -> qpl_ml_status {
    return status_list::internal_error;
}

// --- Initialization from the memory stream (i.e. deserialized table) --- //

template <>
auto huffman_table_init_with_stream(qpl_compression_huffman_table &table,
                                    const uint8_t *const buffer,
                                    const uint32_t representation_flags) noexcept -> qpl_ml_status {
    using namespace qpl::ml;
    using namespace qpl::ml::compression;

    auto sw_compression_table_data_ptr   = reinterpret_cast<uint8_t *>(&table.sw_compression_table_data);
    auto isal_compression_table_data_ptr = reinterpret_cast<uint8_t *>(&table.isal_compression_table_data);
    auto hw_compression_table_data_ptr   = reinterpret_cast<uint8_t *>(&table.hw_compression_table_data);
    auto deflate_header_buffer_ptr       = reinterpret_cast<uint8_t *>(&table.deflate_header_buffer);

    compression_huffman_table compression_table(sw_compression_table_data_ptr,
                                                isal_compression_table_data_ptr,
                                                hw_compression_table_data_ptr,
                                                deflate_header_buffer_ptr);

    if (representation_flags & QPL_DEFLATE_REPRESENTATION) {
        compression_table.enable_deflate_header();
        table.representation_mask |= QPL_DEFLATE_REPRESENTATION;
    }

    if (representation_flags & QPL_SW_REPRESENTATION) {
        compression_table.enable_sw_compression_table();
        table.representation_mask |= QPL_SW_REPRESENTATION;
    }

    if (representation_flags & QPL_HW_REPRESENTATION) {
        compression_table.enable_hw_compression_table();
        table.representation_mask |= QPL_HW_REPRESENTATION;
    }

    return details::init_compression_table_with_stream(buffer, compression_table);
}

template <>
auto huffman_table_init_with_stream(qpl_decompression_huffman_table &table,
                                    const uint8_t *const buffer,
                                    const uint32_t representation_flags) noexcept -> qpl_ml_status {
    using namespace qpl::ml;
    using namespace qpl::ml::compression;

    auto sw_flattened_table_ptr           = reinterpret_cast<uint8_t *>(&table.sw_flattened_table);
    auto hw_decompression_state_ptr       = reinterpret_cast<uint8_t *>(&table.hw_decompression_state);
    auto decomp_deflate_header_buffer_ptr = reinterpret_cast<uint8_t *>(&table.deflate_header_buffer);
    auto lookup_table_buffer_ptr          = reinterpret_cast<uint8_t *>(&table.lookup_table_buffer);

    decompression_huffman_table decompression_table(sw_flattened_table_ptr,
                                                    hw_decompression_state_ptr,
                                                    decomp_deflate_header_buffer_ptr,
                                                    lookup_table_buffer_ptr);

    if (representation_flags & QPL_DEFLATE_REPRESENTATION) {
        decompression_table.enable_deflate_header();
        table.representation_mask |= QPL_DEFLATE_REPRESENTATION;
    }

    if (representation_flags & QPL_SW_REPRESENTATION) {
        decompression_table.enable_sw_decompression_table();
        table.representation_mask |= QPL_SW_REPRESENTATION;
    }

    if (representation_flags & QPL_HW_REPRESENTATION) {
        decompression_table.enable_hw_decompression_table();
        table.representation_mask |= QPL_HW_REPRESENTATION;
    }

    return details::init_decompression_table_with_stream(buffer, decompression_table);
}

// --- Convert functions group --- //

template <>
auto huffman_table_convert(const compression_huffman_table &compression_table,
                           decompression_huffman_table &decompression_table,
                           const uint32_t UNREFERENCED_PARAMETER(representation_flags)) noexcept -> qpl_ml_status {
    return details::comp_to_decompression_table(compression_table, decompression_table);
}

template <>
auto huffman_table_convert(const qpl_compression_huffman_table &compression_table,
                           qpl_decompression_huffman_table &decompression_table,
                           const uint32_t representation_flags) noexcept -> qpl_ml_status {
    using namespace qpl::ml::compression;

    // This is a workaround, because currently compression_huffman_table cannot accept pointer to const data
    // as argument in it's constructor, TODO: remove const cast
    auto casted_compression_table = const_cast<qpl_compression_huffman_table *>(&compression_table);

    // Setup compression table
    auto sw_compression_table_data_ptr   =
                 reinterpret_cast<uint8_t *>(&casted_compression_table->sw_compression_table_data);
    auto isal_compression_table_data_ptr =
                 reinterpret_cast<uint8_t *>(&casted_compression_table->isal_compression_table_data);
    auto hw_compression_table_data_ptr   =
                 reinterpret_cast<uint8_t *>(&casted_compression_table->hw_compression_table_data);
    auto comp_deflate_header_buffer_ptr  =
                 reinterpret_cast<uint8_t *>(&casted_compression_table->deflate_header_buffer);

    compression_huffman_table int_compression_table(sw_compression_table_data_ptr,
                                                    isal_compression_table_data_ptr,
                                                    hw_compression_table_data_ptr,
                                                    comp_deflate_header_buffer_ptr);

    if (compression_table.representation_mask & QPL_DEFLATE_REPRESENTATION) {
        int_compression_table.enable_deflate_header();
    }

    if (compression_table.representation_mask & QPL_SW_REPRESENTATION) {
        int_compression_table.enable_sw_compression_table();
    }

    if (compression_table.representation_mask & QPL_HW_REPRESENTATION) {
        int_compression_table.enable_hw_compression_table();
    }

    auto sw_flattened_table_ptr           = reinterpret_cast<uint8_t *>(&decompression_table.sw_flattened_table);
    auto hw_decompression_state_ptr       = reinterpret_cast<uint8_t *>(&decompression_table.hw_decompression_state);
    auto decomp_deflate_header_buffer_ptr = reinterpret_cast<uint8_t *>(&decompression_table.deflate_header_buffer);
    auto lookup_table_buffer_ptr          = reinterpret_cast<uint8_t *>(&decompression_table.lookup_table_buffer);

    // Setup decompression table
    decompression_huffman_table int_decompression_table(sw_flattened_table_ptr,
                                                        hw_decompression_state_ptr,
                                                        decomp_deflate_header_buffer_ptr,
                                                        lookup_table_buffer_ptr);

    if (representation_flags & QPL_DEFLATE_REPRESENTATION) {
        int_decompression_table.enable_deflate_header();
        decompression_table.representation_mask |= QPL_DEFLATE_REPRESENTATION;
    }

    if (representation_flags & QPL_SW_REPRESENTATION) {
        int_decompression_table.enable_sw_decompression_table();
        decompression_table.representation_mask |= QPL_SW_REPRESENTATION;
    }

    if (representation_flags & QPL_HW_REPRESENTATION) {
        int_decompression_table.enable_hw_decompression_table();
        decompression_table.representation_mask |= QPL_HW_REPRESENTATION;
    }

    return details::comp_to_decompression_table(int_compression_table, int_decompression_table);
}

template <>
auto huffman_table_convert(const isal_hufftables &isal_table,
                           qplc_huffman_table_default_format &qpl_default_format_table,
                           const uint32_t UNREFERENCED_PARAMETER(representation_flags)) noexcept -> qpl_ml_status {
    details::isal_compression_table_to_qpl(&isal_table, &qpl_default_format_table);

    return status_list::ok;
}

template <>
auto huffman_table_convert(const qpl_compression_huffman_table &qpl_table,
                           isal_hufftables &isal_table,
                           const uint32_t UNREFERENCED_PARAMETER(representation_flags)) noexcept -> qpl_ml_status {
    details::qpl_huffman_table_to_isal(&qpl_table, &isal_table, little_endian);

    return status_list::ok;
}

template <>
auto huffman_table_convert(const qplc_huffman_table_default_format &qpl_default_format_table,
                           isal_hufftables &isal_table,
                           const uint32_t UNREFERENCED_PARAMETER(representation_flags)) noexcept -> qpl_ml_status {
    //@todo Danger pointers speculation
    auto *table = reinterpret_cast<const qpl_compression_huffman_table *>(&qpl_default_format_table);

    huffman_table_convert(*table, isal_table, little_endian);

    return status_list::ok;
}

// --- Serialize/write to memory stream functions group --- //

template <>
auto huffman_table_write_to_stream(const qpl_compression_huffman_table &table,
                                   uint8_t *const buffer,
                                   const uint32_t representation_flags) noexcept -> qpl_ml_status {
    using namespace qpl::ml;
    using namespace qpl::ml::compression;

    auto casted_table = const_cast<qpl_compression_huffman_table *>(&table);

    auto sw_compression_table_data_ptr   = reinterpret_cast<uint8_t *>(&casted_table->sw_compression_table_data);
    auto isal_compression_table_data_ptr = reinterpret_cast<uint8_t *>(&casted_table->isal_compression_table_data);
    auto hw_compression_table_data_ptr   = reinterpret_cast<uint8_t *>(&casted_table->hw_compression_table_data);
    auto deflate_header_buffer_ptr       = reinterpret_cast<uint8_t *>(&casted_table->deflate_header_buffer);

    compression_huffman_table compression_table(sw_compression_table_data_ptr,
                                                isal_compression_table_data_ptr,
                                                hw_compression_table_data_ptr,
                                                deflate_header_buffer_ptr);

    if (representation_flags & QPL_DEFLATE_REPRESENTATION) {
        compression_table.enable_deflate_header();
    }

    if (representation_flags & QPL_SW_REPRESENTATION) {
        compression_table.enable_sw_compression_table();
    }

    if (representation_flags & QPL_HW_REPRESENTATION) {
        compression_table.enable_hw_compression_table();
    }

    if (representation_flags & QPL_HUFFMAN_ONLY_REPRESENTATION) {
        compression_table.make_huffman_only();
    }

    return details::write_compression_table_to_stream(buffer, compression_table);
}

template <>
auto huffman_table_write_to_stream(const qpl_decompression_huffman_table &table,
                                   uint8_t *const buffer,
                                   const uint32_t representation_flags) noexcept -> qpl_ml_status {
    using namespace qpl::ml;
    using namespace qpl::ml::compression;

    auto casted_table = const_cast<qpl_decompression_huffman_table *>(&table);

    auto sw_flattened_table_ptr           = reinterpret_cast<uint8_t *>(&casted_table->sw_flattened_table);
    auto hw_decompression_state_ptr       = reinterpret_cast<uint8_t *>(&casted_table->hw_decompression_state);
    auto decomp_deflate_header_buffer_ptr = reinterpret_cast<uint8_t *>(&casted_table->deflate_header_buffer);
    auto lookup_table_buffer_ptr          = reinterpret_cast<uint8_t *>(&casted_table->lookup_table_buffer);

    decompression_huffman_table decompression_table(sw_flattened_table_ptr,
                                                    hw_decompression_state_ptr,
                                                    decomp_deflate_header_buffer_ptr,
                                                    lookup_table_buffer_ptr);

    if (representation_flags & QPL_DEFLATE_REPRESENTATION) {
        decompression_table.enable_deflate_header();
    }

    if (representation_flags & QPL_SW_REPRESENTATION) {
        decompression_table.enable_sw_decompression_table();;
    }

    if (representation_flags & QPL_HW_REPRESENTATION) {
        decompression_table.enable_hw_decompression_table();
    }

    return details::write_decompression_table_to_stream(buffer, decompression_table);
}


// --- Functions to compare two (de)compression tables --- //

template <>
bool is_equal(qpl_compression_huffman_table &table,
              qpl_compression_huffman_table &other_table) noexcept {

    auto sw_table    = reinterpret_cast<uint8_t *>(&table.sw_compression_table_data);
    auto isal_table  = reinterpret_cast<uint8_t *>(&table.isal_compression_table_data);
    auto hw_table    = reinterpret_cast<uint8_t *>(&table.hw_compression_table_data);
    auto deflate_buf = reinterpret_cast<uint8_t *>(&table.deflate_header_buffer);

    auto other_sw_table    = reinterpret_cast<uint8_t *>(&other_table.sw_compression_table_data);
    auto other_isal_table  = reinterpret_cast<uint8_t *>(&other_table.isal_compression_table_data);
    auto other_hw_table    = reinterpret_cast<uint8_t *>(&other_table.hw_compression_table_data);
    auto other_deflate_buf = reinterpret_cast<uint8_t *>(&other_table.deflate_header_buffer);

    auto sw_table_diff    = std::memcmp(sw_table,    other_sw_table,    sizeof(qplc_huffman_table_default_format));
    auto isal_table_diff  = std::memcmp(isal_table,  other_isal_table,  sizeof(isal_hufftables));
    auto hw_table_diff    = std::memcmp(hw_table,    other_hw_table,    sizeof(qpl::ml::compression::hw_compression_huffman_table));
    auto deflate_buf_diff = std::memcmp(deflate_buf, other_deflate_buf, sizeof(qpl::ml::compression::deflate_header));

    return (sw_table_diff == 0) && (isal_table_diff == 0) && (hw_table_diff == 0) && (deflate_buf_diff == 0);
}

template <>
bool is_equal(qpl_decompression_huffman_table &table,
              qpl_decompression_huffman_table &other_table) noexcept {

    auto sw_table    = reinterpret_cast<uint8_t *>(&table.sw_flattened_table);
    auto hw_table    = reinterpret_cast<uint8_t *>(&table.hw_decompression_state);
    auto deflate_buf = reinterpret_cast<uint8_t *>(&table.deflate_header_buffer);
    auto lookup_buf  = reinterpret_cast<uint8_t *>(&table.lookup_table_buffer);

    auto other_sw_table    = reinterpret_cast<uint8_t *>(&other_table.sw_flattened_table);
    auto other_hw_table    = reinterpret_cast<uint8_t *>(&other_table.hw_decompression_state);
    auto other_deflate_buf = reinterpret_cast<uint8_t *>(&other_table.deflate_header_buffer);
    auto other_lookup_buf  = reinterpret_cast<uint8_t *>(&other_table.lookup_table_buffer);

    auto sw_table_diff    = std::memcmp(sw_table,    other_sw_table,    sizeof(qplc_huffman_table_flat_format));
    auto deflate_buf_diff = std::memcmp(deflate_buf, other_deflate_buf, sizeof(qpl::ml::compression::deflate_header));
    auto lookup_buf_diff  = std::memcmp(lookup_buf,  other_lookup_buf,  sizeof(qpl::ml::compression::canned_table));

    // TODO: there is an issue in HW representation that doesn't affect the actual data,
    // but some garbage appears even though we do memset at the beginning.
    // Because of that we compare only data that is being used
    // and not the whole buffer (that includes also extra space for padding/alignment/etc).
    // To access the data we "convert" the qpl_decompression_huffman_table to the representation
    // that is being used internally - decompression_huffman_table.

    /*
    // This doesn't work currently, see TODO above
    auto hw_table_diff = std::memcmp(hw_table, other_hw_table, sizeof(qpl::ml::compression::hw_decompression_state));
    */

    decompression_huffman_table decompression_table(sw_table,
                                                    hw_table,
                                                    deflate_buf,
                                                    lookup_buf);

    decompression_huffman_table other_decompression_table(other_sw_table,
                                                          other_hw_table,
                                                          other_deflate_buf,
                                                          other_lookup_buf);

    hw_decompression_state*       hw = decompression_table.get_hw_decompression_state();
    hw_decompression_state* other_hw = other_decompression_table.get_hw_decompression_state();

    // Comparing actual data stored in hw state here
    auto hw_table_diff = std::memcmp(hw, other_hw, HW_AECS_ANALYTICS_SIZE);

    return (sw_table_diff == 0) && (hw_table_diff == 0) && (deflate_buf_diff == 0) && (lookup_buf_diff == 0);
}

};

