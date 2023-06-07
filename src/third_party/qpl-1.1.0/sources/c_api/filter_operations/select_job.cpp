/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#include "analytics_state_t.h"
#include "filter_operations.hpp"
#include "arguments_check.hpp"
#include "analytics/select.hpp"

namespace qpl {

uint32_t perform_select(qpl_job *job_ptr,
                        uint8_t *unpack_buffer_ptr,
                        uint32_t unpack_buffer_size,
                        uint8_t *output_buffer_ptr,
                        uint32_t output_buffer_size,
                        uint8_t *mask_buffer_ptr,
                        uint32_t mask_buffer_size) {
    using namespace ml;
    using namespace ml::analytics;

    OWN_QPL_CHECK_STATUS(job::validate_operation<qpl_op_select>(job_ptr))

    const auto input_stream_format  = get_stream_format(job_ptr->parser);
    const auto out_bit_width_format = static_cast<analytics::output_bit_width_format_t>(job_ptr->out_bit_width);
    const auto mask_stream_format   = job_ptr->flags & QPL_FLAG_SRC2_BE ? stream_format_t::be_format
                                                                        : stream_format_t::le_format;
    const auto output_stream_format = (job_ptr->flags & QPL_FLAG_OUT_BE) ? stream_format_t::be_format
                                                                         : stream_format_t::le_format;
    const auto crc_type             = job_ptr->flags & QPL_FLAG_CRC32C ? analytics::input_stream_t::crc_t::iscsi
                                                                       : analytics::input_stream_t::crc_t::gzip;

    auto *src_begin  = const_cast<uint8_t *>(job_ptr->next_in_ptr);
    auto *src_end    = const_cast<uint8_t *>(job_ptr->next_in_ptr + job_ptr->available_in);
    auto *dst_begin  = const_cast<uint8_t *>(job_ptr->next_out_ptr);
    auto *dst_end    = const_cast<uint8_t *>(job_ptr->next_out_ptr + job_ptr->available_out);
    auto *mask_begin = const_cast<uint8_t *>(job_ptr->next_src2_ptr);
    auto *mask_end   = const_cast<uint8_t *>(job_ptr->next_src2_ptr + job_ptr->available_src2);

    auto *analytics_state_ptr     = reinterpret_cast<own_analytics_state_t *>( job_ptr->data_ptr.analytics_state_ptr);
    auto *decompress_buffer_begin = analytics_state_ptr->inflate_buf_ptr;
    auto *decompress_buffer_end   = decompress_buffer_begin + analytics_state_ptr->inflate_buf_size;

    allocation_buffer_t state_buffer(job_ptr->data_ptr.middle_layer_buffer_ptr, job_ptr->data_ptr.hw_state_ptr);

    analytic_operation_result_t result{};

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

            auto mask_stream = analytics::input_stream_t::builder(mask_begin, mask_end)
                    .element_count(job_ptr->num_input_elements)
                    .stream_format(mask_stream_format, job_ptr->src2_bit_width)
                    .build<execution_path_t::hardware>();

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

            // Configure buffers
            limited_buffer_t unpack_buffer(unpack_buffer_ptr, unpack_buffer_ptr + unpack_buffer_size, input_stream.bit_width());
            limited_buffer_t set_buffer(mask_buffer_ptr, mask_buffer_ptr + mask_buffer_size, byte_bits_size);
            limited_buffer_t output_buffer(output_buffer_ptr, output_buffer_ptr + output_buffer_size, 1u);

            result = call_select<execution_path_t::hardware>(input_stream,
                                                             mask_stream,
                                                             output_stream,
                                                             unpack_buffer,
                                                             set_buffer,
                                                             output_buffer,
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

            auto mask_stream = analytics::input_stream_t::builder(mask_begin, mask_end)
                    .element_count(job_ptr->num_input_elements)
                    .stream_format(mask_stream_format, job_ptr->src2_bit_width)
                    .build<execution_path_t::auto_detect>();

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

            // Configure buffers
            limited_buffer_t unpack_buffer(unpack_buffer_ptr, unpack_buffer_ptr + unpack_buffer_size, input_stream.bit_width());
            limited_buffer_t set_buffer(mask_buffer_ptr, mask_buffer_ptr + mask_buffer_size, byte_bits_size);
            limited_buffer_t output_buffer(output_buffer_ptr, output_buffer_ptr + output_buffer_size, 1u);

            result = call_select<execution_path_t::auto_detect>(input_stream,
                                                                mask_stream,
                                                                output_stream,
                                                                unpack_buffer,
                                                                set_buffer,
                                                                output_buffer,
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

            auto mask_stream = analytics::input_stream_t::builder(mask_begin, mask_end)
                    .element_count(job_ptr->num_input_elements)
                    .stream_format(mask_stream_format, job_ptr->src2_bit_width)
                    .build<execution_path_t::software>();

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

            // Configure buffers
            limited_buffer_t unpack_buffer(unpack_buffer_ptr, unpack_buffer_ptr + unpack_buffer_size, input_stream.bit_width());
            limited_buffer_t set_buffer(mask_buffer_ptr, mask_buffer_ptr + mask_buffer_size, byte_bits_size);
            limited_buffer_t output_buffer(output_buffer_ptr, output_buffer_ptr + output_buffer_size, 1u);

            result = call_select<execution_path_t::software>(input_stream,
                                                             mask_stream,
                                                             output_stream,
                                                             unpack_buffer,
                                                             set_buffer,
                                                             output_buffer);
        }
    }

    job_ptr->total_out = result.output_bytes_;

    if (result.status_code_ == 0) {
        update_job(job_ptr, result);
    }

    return result.status_code_;
}

} // namespace qpl
