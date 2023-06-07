/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#include "analytics_state_t.h"
#include "filter_operations.hpp"
#include "arguments_check.hpp"
#include "analytics/extract.hpp"

namespace qpl {

uint32_t perform_extract(qpl_job *job_ptr, uint8_t *buffer_ptr, uint32_t buffer_size) {
    using namespace ml;

    OWN_QPL_CHECK_STATUS(job::validate_operation<qpl_op_extract>(job_ptr))

    if (job_ptr->param_low > job_ptr->param_high) {
        job::update_input_stream(job_ptr, 0u);

        return status_list::ok;
    }

    const auto input_stream_format  = analytics::get_stream_format(job_ptr->parser);
    const auto out_bit_width_format = static_cast<analytics::output_bit_width_format_t>(job_ptr->out_bit_width);
    const auto output_stream_format = (job_ptr->flags & QPL_FLAG_OUT_BE) ? analytics::stream_format_t::be_format
                                                                         : analytics::stream_format_t::le_format;
    const auto crc_type             = job_ptr->flags & QPL_FLAG_CRC32C ? analytics::input_stream_t::crc_t::iscsi
                                                                       : analytics::input_stream_t::crc_t::gzip;

    auto *src_begin = const_cast<uint8_t *>(job_ptr->next_in_ptr);
    auto *src_end   = const_cast<uint8_t *>(job_ptr->next_in_ptr + job_ptr->available_in);
    auto *dst_begin = const_cast<uint8_t *>(job_ptr->next_out_ptr);
    auto *dst_end   = const_cast<uint8_t *>(job_ptr->next_out_ptr + job_ptr->available_out);

    auto *analytics_state_ptr     = reinterpret_cast<own_analytics_state_t *>(job_ptr->data_ptr.analytics_state_ptr);
    auto *decompress_buffer_begin = analytics_state_ptr->inflate_buf_ptr;
    auto *decompress_buffer_end   = decompress_buffer_begin + analytics_state_ptr->inflate_buf_size;

    allocation_buffer_t state_buffer(job_ptr->data_ptr.middle_layer_buffer_ptr, job_ptr->data_ptr.hw_state_ptr);

    analytics::analytic_operation_result_t extract_result{};

    switch (job_ptr->data_ptr.path) {
        case qpl_path_hardware: {
            auto input_stream = analytics::input_stream_t::builder(src_begin, src_end)
                    .element_count(job_ptr->num_input_elements)
                    .omit_checksums(job_ptr->flags & QPL_FLAG_OMIT_CHECKSUMS)
                    .omit_aggregates(job_ptr->flags & QPL_FLAG_OMIT_AGGREGATES)
                    .ignore_bytes(job_ptr->drop_initial_bytes)
                    .crc_type(crc_type)
                    .compressed(job_ptr->flags & QPL_FLAG_DECOMPRESS_ENABLE,
                                static_cast<qpl_decomp_end_proc>(job_ptr->decomp_end_processing),
                                job_ptr->ignore_end_bits)
                    .decompress_buffer<execution_path_t::hardware>(decompress_buffer_begin, decompress_buffer_end)
                    .stream_format(input_stream_format, job_ptr->src1_bit_width)
                    .build<execution_path_t::hardware>(state_buffer);

            auto output_stream = analytics::output_stream_t<analytics::array_stream>::builder(dst_begin, dst_end)
                    .stream_format(output_stream_format)
                    .bit_format(out_bit_width_format, input_stream.bit_width())
                    .nominal(input_stream.bit_width() == bit_bits_size)
                    .initial_output_index(job_ptr->initial_output_index)
                    .build<execution_path_t::hardware>();

            auto bad_arg_status = validate_input_stream(input_stream);

            if (bad_arg_status != status_list::ok) {
                return bad_arg_status;
            }

            limited_buffer_t temporary_buffer(buffer_ptr, buffer_ptr + buffer_size, input_stream.bit_width());

            extract_result = analytics::call_extract<execution_path_t::hardware>(input_stream,
                                                                                 output_stream,
                                                                                 job_ptr->param_low,
                                                                                 job_ptr->param_high,
                                                                                 temporary_buffer,
                                                                                 job_ptr->numa_id);
            break;
        }
        case qpl_path_auto: {
            auto input_stream = analytics::input_stream_t::builder(src_begin, src_end)
                    .element_count(job_ptr->num_input_elements)
                    .omit_checksums(job_ptr->flags & QPL_FLAG_OMIT_CHECKSUMS)
                    .omit_aggregates(job_ptr->flags & QPL_FLAG_OMIT_AGGREGATES)
                    .ignore_bytes(job_ptr->drop_initial_bytes)
                    .crc_type(crc_type)
                    .compressed(job_ptr->flags & QPL_FLAG_DECOMPRESS_ENABLE,
                                static_cast<qpl_decomp_end_proc>(job_ptr->decomp_end_processing),
                                job_ptr->ignore_end_bits)
                    .decompress_buffer<execution_path_t::auto_detect>(decompress_buffer_begin, decompress_buffer_end)
                    .stream_format(input_stream_format, job_ptr->src1_bit_width)
                    .build<execution_path_t::auto_detect>(state_buffer);

            auto output_stream = analytics::output_stream_t<analytics::array_stream>::builder(dst_begin, dst_end)
                    .stream_format(output_stream_format)
                    .bit_format(out_bit_width_format, input_stream.bit_width())
                    .nominal(input_stream.bit_width() == bit_bits_size)
                    .initial_output_index(job_ptr->initial_output_index)
                    .build<execution_path_t::auto_detect>();

            auto bad_arg_status = validate_input_stream(input_stream);

            if (bad_arg_status != status_list::ok) {
                return bad_arg_status;
            }

            limited_buffer_t temporary_buffer(buffer_ptr, buffer_ptr + buffer_size, input_stream.bit_width());

            extract_result = analytics::call_extract<execution_path_t::auto_detect>(input_stream,
                                                                                    output_stream,
                                                                                    job_ptr->param_low,
                                                                                    job_ptr->param_high,
                                                                                    temporary_buffer,
                                                                                    job_ptr->numa_id);
            break;
        }
        case qpl_path_software: {
            auto input_stream = analytics::input_stream_t::builder(src_begin, src_end)
                    .element_count(job_ptr->num_input_elements)
                    .omit_checksums(job_ptr->flags & QPL_FLAG_OMIT_CHECKSUMS)
                    .omit_aggregates(job_ptr->flags & QPL_FLAG_OMIT_AGGREGATES)
                    .ignore_bytes(job_ptr->drop_initial_bytes)
                    .crc_type(crc_type)
                    .compressed(job_ptr->flags & QPL_FLAG_DECOMPRESS_ENABLE,
                                static_cast<qpl_decomp_end_proc>(job_ptr->decomp_end_processing),
                                job_ptr->ignore_end_bits)
                    .decompress_buffer<execution_path_t::software>(decompress_buffer_begin, decompress_buffer_end)
                    .stream_format(input_stream_format, job_ptr->src1_bit_width)
                    .build<execution_path_t::software>(state_buffer);

            auto output_stream = analytics::output_stream_t<analytics::array_stream>::builder(dst_begin, dst_end)
                    .stream_format(output_stream_format)
                    .bit_format(out_bit_width_format, input_stream.bit_width())
                    .nominal(input_stream.bit_width() == bit_bits_size)
                    .initial_output_index(job_ptr->initial_output_index)
                    .build<execution_path_t::software>();

            auto bad_arg_status = validate_input_stream(input_stream);

            if (bad_arg_status != status_list::ok) {
                return bad_arg_status;
            }

            limited_buffer_t temporary_buffer(buffer_ptr, buffer_ptr + buffer_size, input_stream.bit_width());

            extract_result = analytics::call_extract<execution_path_t::software>(input_stream,
                                                                                 output_stream,
                                                                                 job_ptr->param_low,
                                                                                 job_ptr->param_high,
                                                                                 temporary_buffer);
            break;
        }
    }

    job_ptr->total_out = extract_result.output_bytes_;

    if (extract_result.status_code_ == 0) {
        update_job(job_ptr, extract_result);
    }

    return extract_result.status_code_;
}

} // namespace qpl
