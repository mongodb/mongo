/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#include "huffman_only.hpp"
#include "common/bit_reverse.hpp"
#include "util/util.hpp"
#include "util/descriptor_processing.hpp"
#include "util/memory.hpp"
#include "util/checksum.hpp"

namespace qpl::ml::compression {

static void restore_huffman_table(
        const qplc_huffman_table_flat_format &huffman_table,
        std::array<huffman_code, huffman_only_number_of_literals> &result_huffman_table) noexcept {
    // Main cycle
    for (uint32_t current_code_length = 1u; current_code_length < 16u; current_code_length++) {
        // Getting number of codes, first code, index of first literal and symbol with Huffman code length "i"
        const uint16_t number_of_codes   = huffman_table.number_of_codes[current_code_length - 1];
        const uint16_t first_code        = huffman_table.first_codes[current_code_length - 1];
        const uint16_t first_table_index = huffman_table.first_table_indexes[current_code_length - 1u];
        uint8_t        symbol            = huffman_table.index_to_char[first_table_index];

        if (0u == number_of_codes) {
            // We have no reason to continue this iteration
            continue;
        }

        // First iteration in outer scope
        result_huffman_table[symbol].code   = first_code;
        result_huffman_table[symbol].length = current_code_length;

        // Generate other codes and lengths
        for (uint32_t code_number = 1u; code_number < number_of_codes; code_number++) {
            symbol = huffman_table.index_to_char[first_table_index + code_number];

            result_huffman_table[symbol].code   = first_code + code_number;
            result_huffman_table[symbol].length = current_code_length;
        }
    }
}

static void build_lookup_table(
        const std::array<huffman_code, huffman_only_number_of_literals> &huffman_table,
        uint8_t *lookup_table_ptr) noexcept {
    // Main cycle
    for (uint16_t symbol = 0u; symbol < huffman_table.size(); symbol++) {
        const uint8_t  code_length             = huffman_table[symbol].length;
        const uint8_t  offset                  = 16u - code_length;
        const uint16_t code                    = reverse_bits(huffman_table[symbol].code, code_length);
        const uint16_t low_lookup_table_index  = 0u;
        const uint16_t high_lookup_table_index = util::build_mask<uint16_t>(offset);

        if (0u == code_length) {
            continue;
        }

        // Filling lookup table
        for (uint32_t i = low_lookup_table_index; i < high_lookup_table_index + 1u; i++) {
            const uint16_t symbol_position = (i << code_length) | code;

            lookup_table_ptr[symbol_position] = (uint8_t) symbol;
        }
    }
}

static auto perform_huffman_only_decompression(
        bit_reader &reader,
        uint8_t *destination_ptr,
        uint32_t destination_length,
        const uint8_t *lookup_table_ptr,
        const std::array<huffman_code, huffman_only_number_of_literals> &huffman_table,
        bool forse_flush_last_bits) noexcept -> decompression_operation_result_t {
    // Main cycle
    uint32_t current_symbol_index = 0u;

    decompression_operation_result_t result{};
    bool decode_next_symbol = true;

    result.status_code_ = status_list::ok;

    do {
        if (current_symbol_index >= destination_length) {
            if (!reader.is_source_end()) {
                result.status_code_ = status_list::more_output_needed;
            }
            break;
        }

        // Decoding next symbol
        const uint16_t next_bits = reader.peak_bits(huffman_code_bit_length);

        if (forse_flush_last_bits || !reader.is_overflowed()) {
            const uint8_t  symbol                     = lookup_table_ptr[next_bits];
            const uint8_t  current_symbol_code_length = huffman_table[symbol].length;

            // Shifting bit buffer by code length
            reader.shift_bits(current_symbol_code_length);

            // Writing symbol to output
            destination_ptr[current_symbol_index++] = symbol;

            if (forse_flush_last_bits) {
                decode_next_symbol = !reader.is_overflowed() || 
                                     (reader.get_buffer_bit_count() > 0);
            } else {
                decode_next_symbol = !reader.is_overflowed();
            }
        } else {
            decode_next_symbol = !reader.is_overflowed();
        }
    } while (decode_next_symbol);

    result.completed_bytes_ = current_symbol_index;
    result.output_bytes_    = current_symbol_index;

    return result;
}

template <>
auto decompress_huffman_only<execution_path_t::software>(
        huffman_only_decompression_state<execution_path_t::software> &decompression_state,
        decompression_huffman_table &decompression_table) noexcept -> decompression_operation_result_t {
    std::array<huffman_code, huffman_only_number_of_literals> restored_huffman_table;

    const auto *source_ptr = decompression_state.get_fields().current_source_ptr;
    const auto *source_end_ptr = source_ptr + decompression_state.get_fields().source_available;

    auto *destination_ptr = decompression_state.get_fields().current_destination_ptr;
    auto *destination_begin_ptr = decompression_state.get_fields().current_destination_ptr;
    auto available_out = decompression_state.get_fields().destination_available;

    const auto last_byte_valid_bits = decompression_state.get_fields().last_bits_offset;

    bit_reader reader(source_ptr, source_end_ptr);

    // Restore huffman table and build the lookup table
    restore_huffman_table(*decompression_table.get_sw_decompression_table(), restored_huffman_table);

    build_lookup_table(restored_huffman_table, decompression_state.get_lookup_table());

    decompression_operation_result_t result{};

    if (decompression_state.get_endianness() == endianness_t::little_endian) {
        reader.set_last_bits_offset(last_byte_valid_bits);
        result = perform_huffman_only_decompression(reader,
                                                    destination_ptr,
                                                    available_out,
                                                    decompression_state.get_lookup_table(),
                                                    restored_huffman_table,
                                                    true);

        result.completed_bytes_ = reader.get_total_bytes_read();
    } else {
        auto big_endian_buffer_ptr = reinterpret_cast<uint16_t *>(decompression_state.get_buffer());

        util::set_zeros(big_endian_buffer_ptr, huffman_only_be_buffer_size);

        uint32_t total_bytes_read = 0u;
        uint32_t source_size = static_cast<uint32_t>(std::distance(source_ptr, source_end_ptr));

        auto current_source_ptr = reinterpret_cast<const uint16_t *>(source_ptr);

        // Perform decompression while there are enough bytes in the source
        while (total_bytes_read < source_size) {
            uint32_t source_bytes_available = source_size - total_bytes_read;

            bool is_last_chunk = false;

            // Check if this is the last chunk of the input data
            uint32_t actual_temporary_buffer_size = huffman_only_be_buffer_size;
            if (source_bytes_available <= huffman_only_be_buffer_size) {
                actual_temporary_buffer_size = source_bytes_available;
                is_last_chunk = true;
            }

            // Perform bit reversing
            uint32_t current_buffer_index = 0;
            for (; current_buffer_index < actual_temporary_buffer_size / 2; current_buffer_index++) {
                big_endian_buffer_ptr[current_buffer_index] = reverse_bits<uint16_t>(*current_source_ptr);
                current_source_ptr++;
            }

            // The last chunk can have odd byte length
            if (is_last_chunk && actual_temporary_buffer_size % 2 == 1) {
                // If so, read the last byte and perform bit reversing
                uint16_t temporary_source = *current_source_ptr;
                big_endian_buffer_ptr[current_buffer_index] = reverse_bits<uint16_t>(temporary_source);
            }

            if (is_last_chunk) {
                reader.set_last_bits_offset(last_byte_valid_bits);
            }

            // Set reversed stream to the bit reader
            reader.set_source(decompression_state.get_buffer(), decompression_state.get_buffer() + actual_temporary_buffer_size);

            auto iteration_result = perform_huffman_only_decompression(reader,
                                                                       destination_ptr,
                                                                       available_out,
                                                                       decompression_state.get_lookup_table(),
                                                                       restored_huffman_table,
                                                                       is_last_chunk);

            destination_ptr += iteration_result.completed_bytes_;
            available_out   -= iteration_result.completed_bytes_;
            result.completed_bytes_ += iteration_result.completed_bytes_;
            result.output_bytes_    += iteration_result.output_bytes_;

            if (result.status_code_ != status_list::ok) {
                break;
            }

            total_bytes_read += reader.get_total_bytes_read();
        }

        result.completed_bytes_ = total_bytes_read;
    }

    decompression_state.get_fields().crc_value = util::crc32_gzip(destination_begin_ptr,
                                                                  destination_begin_ptr + result.output_bytes_,
                                                                  decompression_state.get_fields().crc_value);

    decompression_state.get_fields().source_available   -= result.completed_bytes_;
    decompression_state.get_fields().current_source_ptr += result.completed_bytes_;
    decompression_state.get_fields().current_destination_ptr += result.output_bytes_;
    decompression_state.get_fields().destination_available   -= result.output_bytes_;


    return result;
}

template <>
auto verify_huffman_only<execution_path_t::software>(huffman_only_decompression_state<execution_path_t::software> &state,
                                                     decompression_huffman_table &decompression_table,
                                                     uint32_t required_crc) noexcept -> qpl_ml_status {
    auto *destination_begin_ptr = state.get_fields().current_destination_ptr;
    auto *destination_end_ptr   = destination_begin_ptr + state.get_fields().destination_available;
    decompression_operation_result_t decompression_result {};

    do {
        state.output(destination_begin_ptr, destination_end_ptr);
        decompression_result = decompress_huffman_only<execution_path_t::software>(state, decompression_table);
    } while ((decompression_result.status_code_ == status_list::ok ||
              decompression_result.status_code_ == status_list::more_output_needed) &&
             state.get_input_size() > 0);

    if (required_crc != state.get_fields().crc_value) {
        return status_list::verify_error;
    } else {
        return status_list::ok;
    }
}

template <>
auto decompress_huffman_only<execution_path_t::hardware>(
        huffman_only_decompression_state<execution_path_t::hardware> &decompression_state,
        decompression_huffman_table &decompression_table) noexcept -> decompression_operation_result_t {
    auto completion_record = decompression_state.handler();
    auto descriptor        = decompression_state.decompress_table(decompression_table)
                                                .build_descriptor();

    auto result = util::process_descriptor<decompression_operation_result_t,
                                           util::execution_mode_t::sync>(descriptor, completion_record);

    if (result.status_code_ == status_list::ok) {
        result.completed_bytes_ = decompression_state.get_input_size();
    }

    return result;
}

}
