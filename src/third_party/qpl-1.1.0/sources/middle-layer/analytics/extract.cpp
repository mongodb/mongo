/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#include "extract.hpp"
#include "descriptor_builder.hpp"
#include "util/descriptor_processing.hpp"

namespace qpl::ml::analytics {

template <analytic_pipeline pipeline_t>
static inline auto extract(input_stream_t &input_stream,
                           limited_buffer_t &buffer,
                           output_stream_t<array_stream> &output_stream,
                           const dispatcher::aggregates_function_ptr_t aggregates_callback,
                           aggregates_t &aggregates,
                           const uint32_t param_low,
                           const uint32_t param_high) noexcept -> uint32_t {
    auto     table        = dispatcher::kernels_dispatcher::get_instance().get_extract_i_table();
    uint32_t index        = dispatcher::get_extract_index(input_stream.bit_width());
    auto     extract_impl = table[index];

    uint32_t source_index = 0;

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

        auto extracted_elements = extract_impl(buffer.data(),
                                               elements_to_process,
                                               &source_index,
                                               param_low,
                                               param_high);

        if (0u != extracted_elements) {
            aggregates_callback(buffer.data(),
                                extracted_elements,
                                &aggregates.min_value_,
                                &aggregates.max_value_,
                                &aggregates.sum_,
                                &aggregates.index_);

            auto status = output_stream.perform_pack(buffer.data(),
                                                     extracted_elements);

            if (status_list::ok != status) {
                return status;
            }
        }
    }

    return status_list::ok;
}

template <analytic_pipeline = analytic_pipeline::simple>
static inline auto extract(input_stream_t &input_stream,
                           limited_buffer_t &buffer,
                           output_stream_t<array_stream> &output_stream,
                           const dispatcher::extract_function_ptr_t extract_kernel,
                           const dispatcher::aggregates_function_ptr_t aggregates_callback,
                           aggregates_t &aggregates,
                           const uint32_t param_low,
                           const uint32_t param_high) noexcept -> uint32_t {
    uint32_t source_index = 0;

    while (!input_stream.is_processed()) {
        auto elements_to_process = std::min(buffer.max_elements_count(),
                                            input_stream.elements_left());
        auto extracted_elements  = extract_kernel(input_stream.current_ptr(),
                                                  buffer.data(),
                                                  elements_to_process,
                                                  &source_index,
                                                  param_low,
                                                  param_high);

        if (0 != extracted_elements) {
            aggregates_callback(buffer.data(),
                                extracted_elements,
                                &aggregates.min_value_,
                                &aggregates.max_value_,
                                &aggregates.sum_,
                                &aggregates.index_);

            auto status = output_stream.perform_pack(buffer.data(),
                                                     extracted_elements);

            if (status_list::ok != status) {
                return status;
            }
        }

        auto length_in_bytes = util::bit_to_byte(elements_to_process * input_stream.bit_width());

        input_stream.shift_current_ptr(length_in_bytes);
        input_stream.add_elements_processed(elements_to_process);
    }

    return status_list::ok;
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstack-usage=4096"
#endif

template <>
auto call_extract<execution_path_t::hardware>(input_stream_t &input_stream,
                                              output_stream_t<array_stream> &output_stream,
                                              uint32_t param_low,
                                              uint32_t param_high,
                                              limited_buffer_t &UNREFERENCED_PARAMETER(temporary_buffer),
                                              int32_t numa_id) noexcept -> analytic_operation_result_t {
    hw_iaa_aecs_analytic HW_PATH_ALIGN_STRUCTURE aecs_analytic{};
    HW_PATH_VOLATILE hw_completion_record HW_PATH_ALIGN_STRUCTURE completion_record{};
    hw_descriptor HW_PATH_ALIGN_STRUCTURE                         descriptor{};

    descriptor_builder<qpl_op_extract>(&completion_record, &aecs_analytic).operation(param_low, param_high)
                                                                           .input(input_stream)
                                                                           .output(output_stream)
                                                                           .build(&descriptor);

    return util::process_descriptor<analytic_operation_result_t, util::execution_mode_t::sync>(&descriptor,
                                                                                               &completion_record,
                                                                                               numa_id);
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

template <>
auto call_extract<execution_path_t::software>(input_stream_t &input_stream,
                                              output_stream_t<array_stream> &output_stream,
                                              uint32_t param_low,
                                              uint32_t param_high,
                                              limited_buffer_t &temporary_buffer,
                                              int32_t UNREFERENCED_PARAMETER(numa_id)) noexcept -> analytic_operation_result_t {
    aggregates_t                aggregates;
    analytic_operation_result_t operation_result;
    uint32_t                    input_bit_width = input_stream.bit_width();
    uint32_t                    status_code     = status_list::ok;

    auto aggregates_table    = dispatcher::kernels_dispatcher::get_instance().get_aggregates_table();
    auto aggregates_index    = dispatcher::get_aggregates_index(input_bit_width);
    auto aggregates_callback = (input_stream.are_aggregates_disabled()) ?
                                      &aggregates_empty_callback :
                                      aggregates_table[aggregates_index];

    if ((input_bit_width == 8u || input_bit_width == 16u || input_bit_width == 32u) &&
        input_stream.stream_format() == stream_format_t::le_format &&
        !input_stream.is_compressed()) {
        auto     extract_table  = dispatcher::kernels_dispatcher::get_instance().get_extract_table();
        uint32_t extract_index  = dispatcher::get_extract_index(input_bit_width);
        auto     extract_kernel = extract_table[extract_index];

        status_code = extract<analytic_pipeline::simple>(input_stream,
                                                         temporary_buffer,
                                                         output_stream,
                                                         extract_kernel,
                                                         aggregates_callback,
                                                         aggregates,
                                                         param_low,
                                                         param_high);
    } else {
        if (input_stream.stream_format() == stream_format_t::prle_format) {
            if (input_stream.is_compressed()) {
                status_code = extract<analytic_pipeline::inflate_prle>(input_stream,
                                                                       temporary_buffer,
                                                                       output_stream,
                                                                       aggregates_callback,
                                                                       aggregates,
                                                                       param_low,
                                                                       param_high);
            } else {
                status_code = extract<analytic_pipeline::prle>(input_stream,
                                                               temporary_buffer,
                                                               output_stream,
                                                               aggregates_callback,
                                                               aggregates,
                                                               param_low,
                                                               param_high);
            }
        } else {
            if (input_stream.is_compressed()) {
                status_code = extract<analytic_pipeline::inflate>(input_stream,
                                                                  temporary_buffer,
                                                                  output_stream,
                                                                  aggregates_callback,
                                                                  aggregates,
                                                                  param_low,
                                                                  param_high);
            } else {
                status_code = extract<analytic_pipeline::simple>(input_stream,
                                                                 temporary_buffer,
                                                                 output_stream,
                                                                 aggregates_callback,
                                                                 aggregates,
                                                                 param_low,
                                                                 param_high);
            }
        }
    }

    input_stream.calculate_checksums();

    if (1u == output_stream.bit_width()) {
        operation_result.last_bit_offset_ = ((param_high - param_low + 1u) * input_bit_width & max_bit_index);
    } else {
        operation_result.last_bit_offset_ = 0u;
    }

    operation_result.status_code_      = status_code;
    operation_result.aggregates_       = aggregates;
    operation_result.checksums_.crc32_ = input_stream.crc_checksum();
    operation_result.checksums_.xor_   = input_stream.xor_checksum();
    operation_result.output_bytes_     = output_stream.bytes_written();

    return operation_result;
}

template <>
auto call_extract<execution_path_t::auto_detect>(input_stream_t &input_stream,
                                                 output_stream_t<array_stream> &output_stream,
                                                 uint32_t param_low,
                                                 uint32_t param_high,
                                                 limited_buffer_t &temporary_buffer,
                                                 int32_t numa_id) noexcept -> analytic_operation_result_t {
    auto hw_result = call_extract<execution_path_t::hardware>(input_stream,
                                                              output_stream,
                                                              param_low,
                                                              param_high,
                                                              temporary_buffer,
                                                              numa_id);

    if (hw_result.status_code_ != status_list::ok) {
        return call_extract<execution_path_t::software>(input_stream,
                                                        output_stream,
                                                        param_low,
                                                        param_high,
                                                        temporary_buffer);
    }

    return hw_result;
}

} // namespace qpl::ml::analytics
