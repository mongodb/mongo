/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#include "select.hpp"
#include "descriptor_builder.hpp"
#include "util/descriptor_processing.hpp"

namespace qpl::ml::analytics {

template <analytic_pipeline pipeline_t>
static inline auto select(input_stream_t &input_stream,
                          input_stream_t &mask_stream,
                          output_stream_t<array_stream> &output_stream,
                          limited_buffer_t &unpack_buffer,
                          limited_buffer_t &set_buffer,
                          limited_buffer_t &output_buffer,
                          dispatcher::aggregates_function_ptr_t aggregates_callback,
                          aggregates_t &aggregates) noexcept -> uint32_t {
    auto        table       = dispatcher::kernels_dispatcher::get_instance().get_select_table();
    const auto  index       = dispatcher::get_select_index(input_stream.bit_width());
    const auto  select_impl = table[index];

    uint32_t source_elements = 0;
    uint32_t mask_elements   = 0;
    uint8_t  *source_ptr     = nullptr;
    uint8_t  *mask_ptr       = nullptr;

    auto drop_initial_bytes_status = input_stream.skip_prologue(unpack_buffer);
    if (QPL_STS_OK != drop_initial_bytes_status) {
        return drop_initial_bytes_status;
    }

    // Main action
    while (!input_stream.is_processed()) {
        if (mask_elements == 0) {
            auto unpack_result = mask_stream.unpack<analytic_pipeline::simple>(set_buffer);

            if (status_list::ok != unpack_result.status) {
                return unpack_result.status;
            }

            mask_elements = unpack_result.unpacked_elements;
            mask_ptr      = set_buffer.data();
        }

        if (source_elements == 0) {
            auto unpack_result = input_stream.unpack<pipeline_t>(unpack_buffer);

            if (status_list::ok != unpack_result.status) {
                return unpack_result.status;
            }

            source_elements = unpack_result.unpacked_elements;
            source_ptr      = unpack_buffer.data();
        }

        const auto elements_to_process = std::min(source_elements, mask_elements);
        const auto processed_elements  = select_impl(source_ptr,
                                                     mask_ptr,
                                                     output_buffer.data(),
                                                     elements_to_process);

        mask_ptr += elements_to_process;
        source_ptr += util::bit_to_byte(elements_to_process * input_stream.bit_width());

        mask_elements -= elements_to_process;
        source_elements -= elements_to_process;

        if (processed_elements != 0) {
            auto pack_status = output_stream.perform_pack(output_buffer.data(), processed_elements);
            if (status_list::ok != pack_status) {
                return pack_status;
            }
            aggregates_callback(output_buffer.data(),
                                processed_elements,
                                &aggregates.min_value_,
                                &aggregates.max_value_,
                                &aggregates.sum_,
                                &aggregates.index_);
        }
    }

    return status_list::ok;
}

template <>
auto call_select<execution_path_t::software>(input_stream_t &input_stream,
                                             input_stream_t &mask_stream,
                                             output_stream_t<array_stream> &output_stream,
                                             limited_buffer_t &unpack_buffer,
                                             limited_buffer_t &set_buffer,
                                             limited_buffer_t &output_buffer,
                                             int32_t UNREFERENCED_PARAMETER(numa_id)) noexcept -> analytic_operation_result_t {
    // Get required aggregates kernel
    auto aggregates_table    = dispatcher::kernels_dispatcher::get_instance().get_aggregates_table();
    auto aggregates_index    = dispatcher::get_aggregates_index(1u);
    auto aggregates_callback = (input_stream.are_aggregates_disabled()) ?
                                &aggregates_empty_callback :
                                aggregates_table[aggregates_index];

    aggregates_t aggregates{};

    uint32_t status_code = status_list::ok;

    if (input_stream.stream_format() == stream_format_t::prle_format) {
        if (input_stream.is_compressed()) {
            status_code = select<analytic_pipeline::inflate_prle>(input_stream,
                                                                  mask_stream,
                                                                  output_stream,
                                                                  unpack_buffer,
                                                                  set_buffer,
                                                                  output_buffer,
                                                                  aggregates_callback,
                                                                  aggregates);
        } else {
            status_code = select<analytic_pipeline::prle>(input_stream,
                                                          mask_stream,
                                                          output_stream,
                                                          unpack_buffer,
                                                          set_buffer,
                                                          output_buffer,
                                                          aggregates_callback,
                                                          aggregates);
        }
    } else {
        if (input_stream.is_compressed()) {
            status_code = select<analytic_pipeline::inflate>(input_stream,
                                                             mask_stream,
                                                             output_stream,
                                                             unpack_buffer,
                                                             set_buffer,
                                                             output_buffer,
                                                             aggregates_callback,
                                                             aggregates);
        } else {
            status_code = select<analytic_pipeline::simple>(input_stream,
                                                            mask_stream,
                                                            output_stream,
                                                            unpack_buffer,
                                                            set_buffer,
                                                            output_buffer,
                                                            aggregates_callback,
                                                            aggregates);
        }
    }
    input_stream.calculate_checksums();

    analytic_operation_result_t operation_result{};

    // Store operations result
    operation_result.status_code_      = status_code;
    operation_result.aggregates_       = aggregates;
    operation_result.checksums_.crc32_ = input_stream.crc_checksum();
    operation_result.checksums_.xor_   = input_stream.xor_checksum();
    operation_result.output_bytes_     = output_stream.bytes_written();

    operation_result.last_bit_offset_ = (1u == output_stream.bit_width())
                                        ? input_stream.elements_left() & max_bit_index
                                        : 0u;

    return operation_result;
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstack-usage=4096"
#endif

template <>
auto call_select<execution_path_t::hardware>(input_stream_t &input_stream,
                                             input_stream_t &mask_stream,
                                             output_stream_t<array_stream> &output_stream,
                                             limited_buffer_t &UNREFERENCED_PARAMETER(unpack_buffer),
                                             limited_buffer_t &UNREFERENCED_PARAMETER(set_buffer),
                                             limited_buffer_t &UNREFERENCED_PARAMETER(output_buffer),
                                             int32_t numa_id) noexcept -> analytic_operation_result_t {
    hw_iaa_aecs_analytic HW_PATH_ALIGN_STRUCTURE aecs_analytic{};
    HW_PATH_VOLATILE hw_completion_record HW_PATH_ALIGN_STRUCTURE completion_record{};
    hw_descriptor HW_PATH_ALIGN_STRUCTURE                         descriptor{};

    descriptor_builder<qpl_op_select>(&completion_record, &aecs_analytic).operation(mask_stream)
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
auto call_select<execution_path_t::auto_detect>(input_stream_t &input_stream,
                                                input_stream_t &mask_stream,
                                                output_stream_t<array_stream> &output_stream,
                                                limited_buffer_t &unpack_buffer,
                                                limited_buffer_t &set_buffer,
                                                limited_buffer_t &output_buffer,
                                                int32_t numa_id) noexcept -> analytic_operation_result_t {
    auto hw_result = call_select<execution_path_t::hardware>(input_stream,
                                                             mask_stream,
                                                             output_stream,
                                                             unpack_buffer,
                                                             set_buffer,
                                                             output_buffer,
                                                             numa_id);

    if (hw_result.status_code_ != status_list::ok) {
        return call_select<execution_path_t::software>(input_stream,
                                                       mask_stream,
                                                       output_stream,
                                                       unpack_buffer,
                                                       set_buffer,
                                                       output_buffer);
    }

    return hw_result;
}

}
