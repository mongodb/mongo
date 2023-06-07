/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#include <utility>
#include "multi_descriptor_processing.hpp"
#include "util/util.hpp"
#include "dispatcher/hw_dispatcher.hpp"

namespace qpl::ml::analytics {
template <uint32_t splitting_factor>
auto split_elements(uint32_t number_of_elements) noexcept -> uint32_t {
    if (number_of_elements % (splitting_factor * ml::byte_bits_size) == 0) {
        return number_of_elements / splitting_factor;
    }

    uint32_t elements_in_part = ml::util::round_to_nearest_multiple(number_of_elements / splitting_factor, ml::byte_bits_size);

    return elements_in_part;
}

template <>
void split_descriptors<qpl_operation::qpl_op_scan_eq, 8>(hw_descriptor &reference_descriptor,
                                                         std::array<hw_descriptor, 8> &descriptors) noexcept {
    constexpr uint32_t number_of_descriptors = 8;
    uint8_t *current_source_ptr = nullptr;
    uint32_t source_size = 0;
    hw_iaa_descriptor_get_input_buffer(&reference_descriptor, &current_source_ptr, &source_size);

    uint8_t *current_destination_ptr = nullptr;
    uint32_t destination_size = 0;
    hw_iaa_descriptor_get_output_buffer(&reference_descriptor, &current_destination_ptr, &destination_size);

    const auto number_of_elements = hw_iaa_descriptor_get_number_of_elements(&reference_descriptor);
    const auto source_bit_width = hw_iaa_descriptor_get_source1_bit_width(&reference_descriptor);

    const auto part_element_count = split_elements<number_of_descriptors>(number_of_elements);
    const auto last_part_element_count = number_of_elements - part_element_count * (number_of_descriptors - 1);

    auto source_part_size = (part_element_count * source_bit_width) / 8;
    auto destination_part_size = part_element_count / 8;

    const auto last_source_part_size = source_size - source_part_size * (number_of_descriptors - 1);
    const auto last_destination_part_size = destination_size - destination_part_size * (number_of_descriptors - 1);

    for (uint32_t i = 0; i < number_of_descriptors - 1; i++) {
        descriptors[i] = reference_descriptor;

        // Correct input / output descriptor fields for splitting
        hw_iaa_descriptor_set_input_buffer(&descriptors[i],
                                           current_source_ptr,
                                           source_part_size);

        hw_iaa_descriptor_set_number_of_elements(&descriptors[i], part_element_count);

        hw_iaa_descriptor_set_output_buffer(&descriptors[i],
                                            current_destination_ptr,
                                            destination_part_size);

        current_source_ptr += source_part_size;
        current_destination_ptr += destination_part_size;
    }

    descriptors.back() = reference_descriptor;
    hw_iaa_descriptor_set_input_buffer(&descriptors.back(),
                                       current_source_ptr,
                                       last_source_part_size);

    hw_iaa_descriptor_set_number_of_elements(&descriptors.back(), last_part_element_count);

    hw_iaa_descriptor_set_output_buffer(&descriptors.back(),
                                        current_destination_ptr,
                                        last_destination_part_size);
}

auto is_operation_splittable(const input_stream_t &input_stream,
                             const output_stream_t<output_stream_type_t::bit_stream> &output_stream) noexcept -> bool {
    // TODO: check thread-safety
    static const auto configuration_supported = is_hw_configuration_good_for_splitting();

    if (!configuration_supported) {
        return false;
    }

    if (input_stream.is_compressed() == true || input_stream.stream_format() == stream_format_t::prle_format) {
        return false;
    }

    if (input_stream.source_size() <= 32_kb) {
        return false;
    }

    if (input_stream.are_aggregates_disabled() == false || input_stream.is_checksum_disabled() == false) {
        return false;
    }

    if (output_stream.output_bit_width_format() != output_bit_width_format_t::same_as_input) {
        return false;
    }

    return true;
}

auto is_hw_configuration_good_for_splitting() noexcept -> bool {
#if defined( linux )
    // TODO: check thread-safety
    static auto &dispatcher = dispatcher::hw_dispatcher::get_instance();
    static bool is_there_multiple_wqs_on_single_device = false;

    for (auto &device: dispatcher) {
        if (device.size() > 1) {
            is_there_multiple_wqs_on_single_device = true;
            break;
        }
    }

    return !is_there_multiple_wqs_on_single_device;
#else
    return false;
#endif
}
}
