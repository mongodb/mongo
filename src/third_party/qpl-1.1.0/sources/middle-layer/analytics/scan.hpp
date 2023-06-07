/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#ifndef SCAN_OPERATION_HPP
#define SCAN_OPERATION_HPP

#include "input_stream.hpp"
#include "output_stream.hpp"
#include "descriptor_builder.hpp"
#include "util/descriptor_processing.hpp"
#include "util/multi_descriptor_processing.hpp"

namespace qpl::ml::analytics {

enum comparator_t {
    equals         = 0,
    not_equals     = 1,
    less_than      = 2,
    less_equals    = 3,
    greater_than   = 4,
    greater_equals = 5,
    in_range       = 6,
    out_of_range   = 7
};

struct scan_range_t {
    uint32_t low;
    uint32_t high;
};

static inline auto correct_input_param(const uint32_t source_bit_width,
                                       const uint32_t input_param) noexcept -> uint32_t {
    return (input_param & (std::numeric_limits<uint32_t>::max() >> (limits::max_bit_width - source_bit_width)));
}

template <analytic_pipeline pipeline_t, comparator_t comparator>
static inline auto scan(input_stream_t &input_stream,
                        limited_buffer_t &buffer,
                        output_stream_t<output_stream_type_t::bit_stream> &output_stream,
                        dispatcher::aggregates_function_ptr_t aggregates_callback,
                        aggregates_t &aggregates,
                        uint32_t param_low,
                        uint32_t param_high) noexcept -> uint32_t {
    auto table     = dispatcher::kernels_dispatcher::get_instance().get_scan_i_table();
    auto index     = dispatcher::get_scan_index(input_stream.bit_width(), static_cast<uint32_t>(comparator));
    auto scan_impl = table[index];

    auto drop_initial_bytes_status = input_stream.skip_prologue(buffer);
    if (QPL_STS_OK != drop_initial_bytes_status) {
        return drop_initial_bytes_status;
    }

    while (!input_stream.is_processed()) {
        auto unpack_result = input_stream.unpack<pipeline_t>(buffer);

        if (status_list::ok != unpack_result.status) {
            return unpack_result.status;
        }

        const uint32_t elements_to_process = unpack_result.unpacked_elements;

        scan_impl(buffer.data(), elements_to_process, param_low, param_high);

        aggregates_callback(buffer.data(),
                            elements_to_process,
                            &aggregates.min_value_,
                            &aggregates.max_value_,
                            &aggregates.sum_,
                            &aggregates.index_);

        auto status = output_stream.perform_pack(buffer.data(),
                                                 elements_to_process);

        if (status_list::ok != status) {
            return status;
        }
    }

    return status_list::ok;
}

template <analytic_pipeline = analytic_pipeline::simple>
static inline auto scan(input_stream_t &input_stream,
                        limited_buffer_t &buffer,
                        output_stream_t<bit_stream> &output_stream,
                        dispatcher::scan_function_ptr scan_kernel,
                        dispatcher::aggregates_function_ptr_t aggregates_callback,
                        aggregates_t &aggregates,
                        uint32_t param_low,
                        uint32_t param_high) noexcept -> uint32_t {
    auto drop_initial_bytes_status = input_stream.skip_prologue(buffer);
    if (QPL_STS_OK != drop_initial_bytes_status) {
        return drop_initial_bytes_status;
    }

    while (!input_stream.is_processed()) {
        auto elements_to_process = std::min(buffer.max_elements_count(), input_stream.elements_left());
        scan_kernel(input_stream.current_ptr(), buffer.data(), elements_to_process, param_low, param_high);

        aggregates_callback(buffer.data(),
                            elements_to_process,
                            &aggregates.min_value_,
                            &aggregates.max_value_,
                            &aggregates.sum_,
                            &aggregates.index_);

        auto status = output_stream.perform_pack(buffer.data(),
                                                 elements_to_process);

        if (status_list::ok != status) {
            return status;
        }

        uint32_t length_in_bytes = util::bit_to_byte(elements_to_process * input_stream.bit_width());

        input_stream.shift_current_ptr(length_in_bytes);
        input_stream.add_elements_processed(elements_to_process);

    }

    return status_list::ok;
}

template <comparator_t comparator>
static inline auto call_scan_sw(input_stream_t &input_stream,
                                output_stream_t<bit_stream> &output_stream,
                                const uint32_t param_low,
                                const uint32_t param_high,
                                limited_buffer_t &temporary_buffer) noexcept -> analytic_operation_result_t {
    auto input_bit_width    = input_stream.bit_width();
    auto output_bit_width   = output_stream.bit_width();
    auto number_of_elements = input_stream.elements_left();

    analytic_operation_result_t operation_result{};
    aggregates_t                aggregates{};

    uint32_t status_code = status_list::ok;

    auto corrected_param_low  = correct_input_param(input_bit_width, param_low);
    auto corrected_param_high = correct_input_param(input_bit_width, param_high);

    auto aggregates_table    = dispatcher::kernels_dispatcher::get_instance().get_aggregates_table();
    auto aggregates_index    = dispatcher::get_aggregates_index(1u);
    auto aggregates_callback = (input_stream.are_aggregates_disabled()) ?
                                &aggregates_empty_callback :
                                aggregates_table[aggregates_index];

    if ((input_bit_width == 8 || input_bit_width == 16 || input_bit_width == 32) &&
        input_stream.stream_format() == stream_format_t::le_format &&
        !input_stream.is_compressed()) {

        auto scan_table  = dispatcher::kernels_dispatcher::get_instance().get_scan_table();
        auto scan_index  = dispatcher::get_scan_index(input_bit_width, (uint32_t) comparator);
        auto scan_kernel = scan_table[scan_index];

        status_code = scan<analytic_pipeline::simple>(input_stream,
                                                      temporary_buffer,
                                                      output_stream,
                                                      scan_kernel,
                                                      aggregates_callback,
                                                      aggregates,
                                                      corrected_param_low,
                                                      corrected_param_high);
    } else {
        if (input_stream.stream_format() == stream_format_t::prle_format) {
            if (input_stream.is_compressed()) {
                status_code = scan<analytic_pipeline::inflate_prle, comparator>(input_stream,
                                                                                temporary_buffer,
                                                                                output_stream,
                                                                                aggregates_callback,
                                                                                aggregates,
                                                                                corrected_param_low,
                                                                                corrected_param_high);
            } else {
                status_code = scan<analytic_pipeline::prle, comparator>(input_stream,
                                                                        temporary_buffer,
                                                                        output_stream,
                                                                        aggregates_callback,
                                                                        aggregates,
                                                                        corrected_param_low,
                                                                        corrected_param_high);
            }
        } else {
            if (input_stream.is_compressed()) {
                status_code = scan<analytic_pipeline::inflate, comparator>(input_stream,
                                                                           temporary_buffer,
                                                                           output_stream,
                                                                           aggregates_callback,
                                                                           aggregates,
                                                                           corrected_param_low,
                                                                           corrected_param_high);
            } else {
                status_code = scan<analytic_pipeline::simple, comparator>(input_stream,
                                                                          temporary_buffer,
                                                                          output_stream,
                                                                          aggregates_callback,
                                                                          aggregates,
                                                                          corrected_param_low,
                                                                          corrected_param_high);
            }
        }
    }

    input_stream.calculate_checksums();

    operation_result.status_code_      = status_code;
    operation_result.aggregates_       = aggregates;
    operation_result.checksums_.crc32_ = input_stream.crc_checksum();
    operation_result.checksums_.xor_   = input_stream.xor_checksum();
    operation_result.last_bit_offset_  = (1u == output_bit_width) ? number_of_elements & max_bit_index : 0u;
    operation_result.output_bytes_     = output_stream.bytes_written();

    return operation_result;
}

template <comparator_t comparator>
constexpr static inline auto own_get_scan_range(const uint32_t low_limit,
                                                const uint32_t high_limit,
                                                const uint32_t element_bit_width) noexcept -> scan_range_t {
    scan_range_t   range{};
    const auto     range_mask = (uint32_t) ((1ULL << element_bit_width) - 1u);
    const uint32_t param_low  = low_limit & range_mask;

    if constexpr (comparator == equals || comparator == not_equals) {
        range.low  = param_low;
        range.high = param_low;
    }

    if constexpr (comparator == less_equals) {
        range.low  = 0;
        range.high = param_low;
    }

    if constexpr (comparator == less_than) {
        if (0 == param_low) {
            range.low  = 1;
            range.high = 0;
        } else {
            range.low  = 0;
            range.high = param_low - 1;
        }
    }

    if constexpr (comparator == greater_equals) {
        range.low  = param_low;
        range.high = std::numeric_limits<uint32_t>::max();
    }

    if constexpr (comparator == greater_than) {
        if (param_low == range_mask) {
            range.low  = 1;
            range.high = 0;
        } else {
            range.low  = param_low + 1;
            range.high = std::numeric_limits<uint32_t>::max();
        }
    }

    if constexpr (comparator == in_range || comparator == out_of_range) {
        const uint32_t param_high = high_limit & range_mask;
        range.low  = param_low;
        range.high = param_high;
    }

    return range;
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstack-usage=4096"
#endif

template <comparator_t comparator>
auto call_scan_hw(input_stream_t &input_stream,
                  output_stream_t<bit_stream> &output_stream,
                  const uint32_t param_low,
                  const uint32_t param_high,
                  limited_buffer_t &UNREFERENCED_PARAMETER(temporary_buffer),
                  int32_t numa_id) noexcept -> analytic_operation_result_t {
    hw_iaa_aecs_analytic HW_PATH_ALIGN_STRUCTURE aecs_analytic{};
    HW_PATH_VOLATILE hw_completion_record HW_PATH_ALIGN_STRUCTURE completion_record{};
    hw_descriptor HW_PATH_ALIGN_STRUCTURE                         descriptor{};

    const auto range = own_get_scan_range<comparator>(param_low, param_high, input_stream.bit_width());

    if constexpr (comparator == not_equals || comparator == out_of_range) {
        output_stream.invert_data();
    }

    descriptor_builder<qpl_op_scan_eq>(&completion_record, &aecs_analytic).operation(range.low, range.high)
                                                                          .input(input_stream)
                                                                          .output(output_stream)
                                                                          .build(&descriptor);

    return ml::util::process_descriptor<analytic_operation_result_t, ml::util::execution_mode_t::sync>(&descriptor,
                                                                                                       &completion_record,
                                                                                                       numa_id);
}



template <comparator_t comparator>
auto call_scan_multidescriptor(input_stream_t &input_stream,
                               output_stream_t<bit_stream> &output_stream,
                               const uint32_t param_low,
                               const uint32_t param_high,
                               limited_buffer_t &UNREFERENCED_PARAMETER(temporary_buffer),
                               int32_t numa_id) noexcept -> analytic_operation_result_t {
    hw_iaa_aecs_analytic HW_PATH_ALIGN_STRUCTURE reference_aecs{};
    HW_PATH_VOLATILE hw_completion_record HW_PATH_ALIGN_STRUCTURE reference_completion_record{};
    hw_descriptor HW_PATH_ALIGN_STRUCTURE                         reference_descriptor{};

    constexpr uint32_t number_of_descriptors = 8;
    alignas(HW_PATH_STRUCTURES_REQUIRED_ALIGN) std::array<hw_completion_record, number_of_descriptors> completion_records{};
    alignas(HW_PATH_STRUCTURES_REQUIRED_ALIGN) std::array<hw_descriptor, number_of_descriptors> descriptors{};


    const auto range = own_get_scan_range<comparator>(param_low, param_high, input_stream.bit_width());

    if constexpr (comparator == not_equals || comparator == out_of_range) {
        output_stream.invert_data();
    }

    // Initialize first descriptor from builder
    descriptor_builder<qpl_op_scan_eq>(&reference_completion_record, &reference_aecs).operation(range.low, range.high)
                                                                                     .input(input_stream)
                                                                                     .output(output_stream)
                                                                                     .build(&reference_descriptor);

    split_descriptors<qpl_op_scan_eq, number_of_descriptors>(reference_descriptor,
                                                             descriptors);

    auto result = ml::util::process_descriptor<analytic_operation_result_t, number_of_descriptors>(descriptors,
                                                                                                   completion_records,
                                                                                                   numa_id);

    return result;
}

template <comparator_t comparator, execution_path_t path>
auto call_scan(input_stream_t &input_stream,
               output_stream_t<bit_stream> &output_stream,
               const uint32_t param_low,
               const uint32_t param_high,
               limited_buffer_t &temporary_buffer,
               int32_t UNREFERENCED_PARAMETER(numa_id) = -1) noexcept -> analytic_operation_result_t {
    if constexpr (path == execution_path_t::auto_detect) {
        analytic_operation_result_t hw_result{};

        if (is_operation_splittable(input_stream, output_stream)) {
            hw_result = call_scan_multidescriptor<comparator>(input_stream,
                                                              output_stream,
                                                              param_low,
                                                              param_high,
                                                              temporary_buffer,
                                                              numa_id);
        } else {
            hw_result = call_scan_hw<comparator>(input_stream,
                                                  output_stream,
                                                  param_low,
                                                  param_high,
                                                  temporary_buffer,
                                                  numa_id);
        }

        if (hw_result.status_code_ != status_list::ok) {
            return call_scan_sw<comparator>(input_stream, output_stream, param_low, param_high, temporary_buffer);
        }

        return hw_result;
    } else if constexpr (path == execution_path_t::hardware) {
        if (is_operation_splittable(input_stream, output_stream)) {
            return call_scan_multidescriptor<comparator>(input_stream,
                                                         output_stream,
                                                         param_low,
                                                         param_high,
                                                         temporary_buffer,
                                                         numa_id);
        } else {
            return call_scan_hw<comparator>(input_stream,
                                            output_stream,
                                            param_low,
                                            param_high,
                                            temporary_buffer,
                                            numa_id);
        }
    } else {
        return call_scan_sw<comparator>(input_stream, output_stream, param_low, param_high, temporary_buffer);
    }
}


#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

} // namespace qpl::ml::analytics

#endif // SCAN_OPERATION_HPP
