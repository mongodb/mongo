/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#ifndef QPL_SOURCES_MIDDLE_LAYER_ANALYTICS_HW_ANALYTIC_DEFINITIONS_H_
#define QPL_SOURCES_MIDDLE_LAYER_ANALYTICS_HW_ANALYTIC_DEFINITIONS_H_

#include "hw_definitions.h"
#include "hw_aecs_api.h"
#include "hw_descriptors_api.h"
#include "util/memory.hpp"

namespace qpl::ml::analytics {

template <qpl_operation op>
class descriptor_builder {
public:
    descriptor_builder() = delete;

    descriptor_builder(HW_PATH_VOLATILE hw_completion_record *const completion_record_ptr,
                       hw_iaa_aecs_analytic *const aecs_ptr)
            : completion_record_ptr_(completion_record_ptr),
              aecs_ptr_(aecs_ptr) {
        hw_iaa_descriptor_reset(&descriptor_);
    }

    inline auto operation(uint32_t, uint32_t) noexcept -> descriptor_builder &;

    inline auto operation(const input_stream_t &mask) noexcept -> descriptor_builder &;

    inline auto operation(const input_stream_t &set_stream,
                          uint32_t drop_low_bits,
                          uint32_t drop_high_bits) noexcept -> descriptor_builder &;

    inline auto decompression(const qpl_decomp_end_proc decompression_rule,
                              const uint32_t ignore_last_bits = 0u) noexcept -> descriptor_builder & {
        hw_iaa_descriptor_analytic_enable_decompress(&descriptor_, false, ignore_last_bits);

        hw_iaa_descriptor_set_inflate_stop_check_rule(&descriptor_,
                                                      static_cast<hw_iaa_decompress_start_stop_rule_t>(decompression_rule),
                                                      decompression_rule & qpl_check_on_nonlast_block);
        return *this;
    }

    inline auto input(input_stream_t &input_stream) noexcept -> descriptor_builder & {
        hw_iaa_descriptor_analytic_set_filter_input(&descriptor_,
                                                    input_stream.data(),
                                                    input_stream.size(),
                                                    input_stream.elements_left(),
                                                    static_cast<hw_iaa_input_format>(input_stream.stream_format()),
                                                    input_stream.bit_width());

        if (input_stream.crc_type() == input_stream_t::crc_t::iscsi) {
            hw_iaa_descriptor_set_crc_rfc3720(&descriptor_);
        }

        if (input_stream.is_compressed()) {
            auto meta = input_stream.compression_meta();

            hw_iaa_descriptor_analytic_enable_decompress(&descriptor_, false, meta.ignore_last_bits);
            hw_iaa_descriptor_set_inflate_stop_check_rule(&descriptor_,
                                                          static_cast<hw_iaa_decompress_start_stop_rule_t>(meta.end_processing_style),
                                                          false);
            hw_iaa_aecs_filter_set_drop_initial_decompressed_bytes(aecs_ptr_, input_stream.prologue_size());
        }

        return *this;
    }

    template <output_stream_type_t stream_type>
    inline auto output(const output_stream_t <stream_type> &output_stream) noexcept -> descriptor_builder & {
        auto out_format = static_cast<hw_iaa_output_format>(output_stream.output_bit_width_format());

        out_format = ((output_stream.stream_format() == stream_format_t::be_format) ?
                      static_cast<hw_iaa_output_format>(hw_iaa_output_modifier_big_endian | out_format)
                                                                                    : out_format
        );

        if constexpr (stream_type == bit_stream) {
            out_format = output_stream.is_inverted() ?
                         static_cast<hw_iaa_output_format>(hw_iaa_output_modifier_inverse | out_format)
                                                     : out_format;
        }

        hw_iaa_descriptor_analytic_set_filter_output(&descriptor_,
                                                     output_stream.data(),
                                                     output_stream.bytes_available(),
                                                     out_format);

        hw_iaa_aecs_filter_set_initial_output_index(aecs_ptr_, output_stream.initial_output_index());

        return *this;
    }

    inline auto build(hw_descriptor *const descriptor_ptr) noexcept -> void {
        hw_iaa_descriptor_set_completion_record(&descriptor_, completion_record_ptr_);
        *descriptor_ptr = descriptor_;

    }

private:
    hw_descriptor HW_PATH_ALIGN_STRUCTURE descriptor_{};
    HW_PATH_VOLATILE hw_completion_record *completion_record_ptr_ = nullptr;
    hw_iaa_aecs_analytic                  *aecs_ptr_              = nullptr;
};

template <>
inline auto descriptor_builder<qpl_op_scan_eq>::operation(const uint32_t param_low,
                                                          const uint32_t param_high) noexcept -> descriptor_builder & {
    hw_iaa_descriptor_analytic_set_scan_operation(&descriptor_, param_low, param_high, aecs_ptr_);

    return *this;
}

template <>
inline auto descriptor_builder<qpl_op_extract>::operation(const uint32_t param_low,
                                                          const uint32_t param_high) noexcept -> descriptor_builder & {
    hw_iaa_descriptor_analytic_set_extract_operation(&descriptor_, param_low, param_high, aecs_ptr_);

    return *this;
}

template <>
inline auto descriptor_builder<qpl_op_expand>::operation(const input_stream_t &mask_stream) noexcept -> descriptor_builder & {
    hw_iaa_descriptor_analytic_set_expand_operation(&descriptor_,
                                                    mask_stream.data(),
                                                    mask_stream.size(),
                                                    mask_stream.stream_format() == stream_format_t::be_format);

    return *this;
}

template <>
inline auto descriptor_builder<qpl_op_select>::operation(const input_stream_t &mask_stream) noexcept -> descriptor_builder & {
    hw_iaa_descriptor_analytic_set_select_operation(&descriptor_,
                                                    mask_stream.data(),
                                                    mask_stream.size(),
                                                    mask_stream.stream_format() == stream_format_t::be_format);

    return *this;
}

}

#endif //QPL_SOURCES_MIDDLE_LAYER_ANALYTICS_HW_ANALYTIC_DEFINITIONS_H_
