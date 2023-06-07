/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/*
 *  Intel® Query Processing Library (Intel® QPL)
 *  Job API (public C API)
 */

#include "own_defs.h"
#include "compression_state_t.h"
#include "common/defs.hpp"
#include "compression/huffman_table/inflate_huffman_table.hpp"
#include "compression/inflate/inflate.hpp"
#include "compression/inflate/inflate_state.hpp"
#include "compression/huffman_only/huffman_only.hpp"
#include "compression/stream_decorators/gzip_decorator.hpp"
#include "compression/stream_decorators/zlib_decorator.hpp"
#include "compression/stream_decorators/default_decorator.hpp"
#include "job.hpp"
#include "arguments_check.hpp"
#include "huffman_table.hpp"

namespace qpl {

using operation_result_t = ml::compression::decompression_operation_result_t;
using huffman_table_t = ml::compression::decompression_huffman_table;

template <>
void inline job::update<operation_result_t>(qpl_job *job_ptr, operation_result_t &result) noexcept {
    job::update_input_stream(job_ptr, result.completed_bytes_);
    job::update_output_stream(job_ptr, result.output_bytes_, 0u);
    job::update_checksums(job_ptr, result.checksums_.crc32_, result.checksums_.xor_);
}

void inline set_representation_flags(qpl_decompression_huffman_table *qpl_decompression_table_ptr,
                                     huffman_table_t &ml_decompression_table) {
    if (is_sw_representation_used(qpl_decompression_table_ptr)) {
        ml_decompression_table.enable_sw_decompression_table();
    }

    if (is_hw_representation_used(qpl_decompression_table_ptr)) {
        ml_decompression_table.enable_hw_decompression_table();
    }

    if (is_deflate_representation_used(qpl_decompression_table_ptr)) {
        ml_decompression_table.enable_deflate_header();
    }
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstack-usage=4096"
#endif

// @todo workaround: there is bug in HW path for Huffman Only + BE
// this code should be removed after fix in HW
uint32_t inline workaround_huffman_only_be_hw(qpl_job* const job_ptr, qpl::ml::compression::endianness_t* endianness)
{
    uint32_t flag_wrkrnd = 0;
    if (job_ptr->flags & QPL_FLAG_HUFFMAN_BE) {
        if (job_ptr->available_in & 1) {
            if (0 == *(job_ptr->next_in_ptr + job_ptr->available_in - 1)) {
                job_ptr->available_in--;
            }
        }
        else {
            if (0 != job_ptr->available_in) {
                if (0 == *(job_ptr->next_in_ptr + job_ptr->available_in - 2)) {
                    if (0 == *(job_ptr->next_in_ptr + job_ptr->available_in - 1)) {
                        job_ptr->available_in--;
                    }
                    else {
                        uint16_t* start_wrkrnd_ptr = (uint16_t*)job_ptr->next_in_ptr;
                        uint32_t half_available_in = job_ptr->available_in >> 1;
                        for (uint32_t count_words = 0; count_words < half_available_in; count_words++) {
                            uint16_t data16_in = start_wrkrnd_ptr[count_words];
                            uint16_t data16_out = 0;
                            uint16_t run1_h, run1_l;
                            for (run1_h = 0x8000, run1_l = 1; run1_h; run1_h >>= 1, run1_l += run1_l) {
                                if (data16_in & run1_l) {
                                    data16_out |= run1_h;
                                }
                            }
                            start_wrkrnd_ptr[count_words] = data16_out;
                        }
                        flag_wrkrnd = 1;
                        job_ptr->available_in--;
                        job_ptr->flags &= (~QPL_FLAG_HUFFMAN_BE);
                        *endianness = qpl::ml::compression::little_endian;
                    }
                }
            }
        }
    }
    return flag_wrkrnd;
}
//@todo the end of code for removing

template <qpl::ml::execution_path_t path>
uint32_t perform_decompress(qpl_job *const job_ptr) noexcept {
    using namespace qpl::ml::compression;
    using namespace qpl::ml;

    if (job_ptr->flags & QPL_FLAG_FIRST) {
        job::reset<qpl_op_decompress>(job_ptr);
    }

    decompression_operation_result_t result{};

    OWN_QPL_CHECK_STATUS(qpl::job::validate_operation<qpl_op_decompress>(job_ptr));

    qpl::ml::allocation_buffer_t state_buffer(job_ptr->data_ptr.middle_layer_buffer_ptr,
                                              job_ptr->data_ptr.hw_state_ptr);

    const qpl::ml::util::linear_allocator allocator(state_buffer);

    if (job_ptr->flags & QPL_FLAG_NO_HDRS) {
        OWN_QPL_CHECK_STATUS(check_huffman_table_is_correct<compression_algorithm_e::huffman_only>(job_ptr->huffman_table))
        auto table_impl = use_as_huffman_table<compression_algorithm_e::huffman_only>(job_ptr->huffman_table);

        auto d_table_ptr = reinterpret_cast<qpl_decompression_huffman_table*>(table_impl->decompression_huffman_table<path>());

        // Initialize decompression table
        decompression_huffman_table decompression_table(table_impl->get_sw_decompression_table_buffer(),
                                                        table_impl->get_hw_decompression_table_buffer(),
                                                        table_impl->get_deflate_header_buffer(),
                                                        table_impl->get_lookup_table_buffer_ptr());

        set_representation_flags(d_table_ptr, decompression_table);

        huffman_only_decompression_state<path> state(allocator);

        auto endianness = (job_ptr->flags & QPL_FLAG_HUFFMAN_BE) ? big_endian : little_endian;

        // @todo workaround: there is bug in HW path for Huffman Only + BE
        // this code should be removed after fix in HW
        uint32_t flag_wrkrnd = 0;
        // @todo the end of code for removing

        if constexpr (path == qpl::ml::execution_path_t::software) {
            state.input(job_ptr->next_in_ptr, job_ptr->next_in_ptr + job_ptr->available_in)
                    .output(job_ptr->next_out_ptr, job_ptr->next_out_ptr + job_ptr->available_out)
                    .crc_seed(job_ptr->crc)
                    .endianness(endianness);
            state.last_bits_offset(static_cast<uint8_t>(qpl::ml::byte_bits_size - job_ptr->ignore_end_bits));
        } else {
            // @todo  workaround: there is bug in HW path for Huffman Only + BE
            // this code should be removed after fix in HW
            flag_wrkrnd = workaround_huffman_only_be_hw(job_ptr, &endianness);
            // @todo the end of code for removing
            state.input(job_ptr->next_in_ptr, job_ptr->next_in_ptr + job_ptr->available_in)
                    .output(job_ptr->next_out_ptr, job_ptr->next_out_ptr + job_ptr->available_out)
                    .crc_seed(job_ptr->crc)
                    .endianness(endianness);
            state.ignore_end_bits = job_ptr->ignore_end_bits;
        }
        result = decompress_huffman_only<path>(state, decompression_table);
        // @todo workaround: there is bug in HW path for Huffman Only + BE
        // this code should be removed after fix in HW
        if (flag_wrkrnd) {
           job_ptr->flags |= QPL_FLAG_HUFFMAN_BE;
        }
        // @todo the end of code for removing
    } else {
        // Prepare decompression state
        auto state = (job_ptr->flags & QPL_FLAG_FIRST) ?
                     qpl::ml::compression::inflate_state<path>::template create<true>(allocator) :
                     qpl::ml::compression::inflate_state<path>::restore(allocator);

        if ( (job_ptr->flags & QPL_FLAG_RND_ACCESS) && !(job_ptr->flags & QPL_FLAG_CANNED_MODE)){ // Random Access
            state.input(job_ptr->next_in_ptr, job_ptr->next_in_ptr + job_ptr->available_in)
             .output(job_ptr->next_out_ptr, job_ptr->next_out_ptr + job_ptr->available_out)
             .crc_seed(job_ptr->crc)
             .input_access({static_cast<bool>( !(job_ptr->flags & QPL_FLAG_FIRST)),
                            job_ptr->ignore_start_bits,
                            job_ptr->ignore_end_bits});

        } else {
            state.input(job_ptr->next_in_ptr, job_ptr->next_in_ptr + job_ptr->available_in)
                .output(job_ptr->next_out_ptr, job_ptr->next_out_ptr + job_ptr->available_out)
                .crc_seed(job_ptr->crc)
                .input_access({static_cast<bool>((job_ptr->flags & QPL_FLAG_RND_ACCESS) || (job_ptr->flags & QPL_FLAG_CANNED_MODE)),
                                job_ptr->ignore_start_bits,
                                job_ptr->ignore_end_bits});
        }

        if (job::is_dictionary(job_ptr)) {
                if constexpr (qpl::ml::execution_path_t::software == path) {
                    state.dictionary(*job_ptr->dictionary);
                } else {
                    if (!job::is_canned_mode_decompression(job_ptr)) {
                        state.dictionary(*job_ptr->dictionary);
                    } else {
                        return qpl::ml::status_list::not_supported_err;
                    }
                }
        }

        if (job_ptr->flags & QPL_FLAG_CANNED_MODE) { // Canned decompression
            OWN_QPL_CHECK_STATUS(check_huffman_table_is_correct<compression_algorithm_e::deflate>(job_ptr->huffman_table))
            auto table_impl = use_as_huffman_table<compression_algorithm_e::deflate>(job_ptr->huffman_table);

            // Initialize decompression table
            decompression_huffman_table decompression_table(table_impl->get_sw_decompression_table_buffer(),
                                                            table_impl->get_hw_decompression_table_buffer(),
                                                            table_impl->get_deflate_header_buffer(),
                                                            table_impl->get_lookup_table_buffer_ptr());

            auto d_table_ptr = reinterpret_cast<qpl_decompression_huffman_table*>(table_impl->decompression_huffman_table<path>());

            set_representation_flags(d_table_ptr, decompression_table);

            if (decompression_table.is_deflate_header_used()) {
                result = default_decorator::unwrap(inflate<path, inflate_mode_t::inflate_body>,
                                                   state.decompress_table(decompression_table),
                                                   stop_and_check_any_eob);
            } else {
                result.status_code_ = qpl::ml::status_list::status_invalid_params;
            }
        } else if (!(job_ptr->flags & QPL_FLAG_RND_ACCESS)){ // Default inflating
            // Perform decompression in inflate standard
            auto end_processing_properties = static_cast<end_processing_condition_t>(job_ptr->decomp_end_processing);

            if (job_ptr->flags & QPL_FLAG_DECOMP_FLUSH_ALWAYS) {
                state.flush_out();
            }

            if (job_ptr->flags & QPL_FLAG_LAST) {
                state.terminate();
            }

            if (job_ptr->flags & QPL_FLAG_GZIP_MODE) {
                result = gzip_decorator::unwrap(inflate<path, inflate_mode_t::inflate_default>,
                                                state,
                                                end_processing_properties);
            } else if (job_ptr->flags & QPL_FLAG_ZLIB_MODE) {
                result = zlib_decorator::unwrap(inflate<path, inflate_mode_t::inflate_default>,
                                                state,
                                                end_processing_properties);
            } else {
                result = default_decorator::unwrap(inflate<path, inflate_mode_t::inflate_default>,
                                                   state,
                                                   end_processing_properties);
            }
        } else { // Random Access
            if (job_ptr->flags & QPL_FLAG_FIRST) {
                result = inflate<path, inflate_mode_t::inflate_header>(state, stop_and_check_any_eob);
            } else {
                result = default_decorator::unwrap(inflate<path, inflate_mode_t::inflate_body>,
                                                   state,
                                                   stop_and_check_any_eob);
            }
        }
    }

    if (result.status_code_ == 0) {
        job::update(job_ptr, result);
    }
    if (result.status_code_ == QPL_STS_INTL_OUTPUT_OVERFLOW) {
        return QPL_STS_MORE_OUTPUT_NEEDED;
    }

    return result.status_code_;
}

template
uint32_t perform_decompress<qpl::ml::execution_path_t::hardware>(qpl_job *const job_ptr) noexcept;

template
uint32_t perform_decompress<qpl::ml::execution_path_t::software>(qpl_job *const job_ptr) noexcept;

}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
