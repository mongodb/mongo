/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/*
 *  Intel® Query Processing Library (Intel® QPL)
 *  Job API (public C API)
 */

#include "common/allocation_buffer_t.hpp"
#include "common/linear_allocator.hpp"

#include "compression/deflate/deflate.hpp"
#include "compression/deflate/streams/deflate_state_builder.hpp"

#include "compression/huffman_only/huffman_only.hpp"
#include "compression/huffman_only/huffman_only_compression_state_builder.hpp"

#include "compression/stream_decorators/default_decorator.hpp"
#include "compression/stream_decorators/gzip_decorator.hpp"
#include "compression/stream_decorators/zlib_decorator.hpp"

#include "compression/huffman_table/huffman_table_utils.hpp"

#include "job.hpp"
#include "compressor.hpp"
#include "arguments_check.hpp"

#include "own_defs.h"
#include "compression_state_t.h"
#include "huffman_table.hpp"
#include "compression/huffman_table/huffman_table.hpp"

// ------ SERVICE FUNCTIONS------ //
namespace qpl{

using operation_result_t = ml::compression::compression_operation_result_t;
using own_huffman_table_t = ml::compression::compression_huffman_table;

template <>
void inline job::update<operation_result_t>(qpl_job *job_ptr, operation_result_t &result) noexcept {
    job::update_output_stream(job_ptr, result.output_bytes_, result.last_bit_offset);
    job::update_checksums(job_ptr, result.checksums_.crc32_, result.checksums_.xor_);

    if (job::is_indexing_enabled(job_ptr)) {
        job::update_index_table(job_ptr, result.indexes_written_);
    }

    if (result.status_code_ == ml::status_list::ok) {
        job::update_input_stream(job_ptr, job_ptr->available_in);
    }
}

template <qpl::ml::execution_path_t path>
uint32_t perform_compression(qpl_job *const job_ptr) noexcept {
    using namespace qpl::ml::compression;

    OWN_QPL_CHECK_STATUS(job::validate_operation<qpl_operation::qpl_op_compress>(job_ptr));

    if (job_ptr->flags & QPL_FLAG_FIRST) {
        job::reset<qpl_op_compress>(job_ptr);
    }

    qpl::ml::allocation_buffer_t state_buffer(job_ptr->data_ptr.middle_layer_buffer_ptr,
                                              job_ptr->data_ptr.hw_state_ptr);

    const qpl::ml::util::linear_allocator allocator(state_buffer);

    operation_result_t result{};

    if (job::is_huffman_only_compression(job_ptr)) { // Huffman only mode
        huffman_only_compression_state_builder<path> builder(allocator);

        builder.output(job_ptr->next_out_ptr, job_ptr->available_out)
               .be_output(job_ptr->flags & QPL_FLAG_HUFFMAN_BE)
               .collect_statistics_step(job_ptr->flags & QPL_FLAG_DYNAMIC_HUFFMAN)
               .crc_seed(job_ptr->crc)
               .verify(!(job_ptr->flags & QPL_FLAG_OMIT_VERIFY))
               .total_out(job_ptr->total_out);

        if (job_ptr->huffman_table != nullptr) {

            OWN_QPL_CHECK_STATUS(check_huffman_table_is_correct<compression_algorithm_e::huffman_only>(job_ptr->huffman_table))

            auto table_impl = use_as_huffman_table<compression_algorithm_e::huffman_only>(job_ptr->huffman_table);

            own_huffman_table_t compression_table(table_impl->get_sw_compression_huffman_table_ptr(),
                                                  table_impl->get_isal_compression_huffman_table_ptr(),
                                                  table_impl->get_hw_compression_huffman_table_ptr(),
                                                  table_impl->get_deflate_header_ptr());

            auto state = builder.compress_table(compression_table).build();

            result = compress_huffman_only<path>(job_ptr->next_in_ptr, job_ptr->available_in, state);
        } else {
            auto state = builder.build();
            result = compress_huffman_only<path>(job_ptr->next_in_ptr, job_ptr->available_in, state);
        }
    } else { // Deflate Mode
        auto builder = (job_ptr->flags & QPL_FLAG_FIRST) ?
                       deflate_state_builder<path>::create(allocator) :
                       deflate_state_builder<path>::restore(allocator);

        builder.output(job_ptr->next_out_ptr, job_ptr->available_out)
               .compression_level(static_cast<compression_level_t>(job_ptr->level))
               .crc_seed({job_ptr->crc, 1})
               .terminate(job_ptr->flags & QPL_FLAG_LAST)
               .verify(!(job_ptr->flags & QPL_FLAG_OMIT_VERIFY))
               .load_current_position(job_ptr->total_out); // Shall be deprecated

        if (job_ptr->flags & QPL_FLAG_DYNAMIC_HUFFMAN) {
            builder.collect_statistics_step(true);
        } else {
            if (job_ptr->flags & QPL_FLAG_START_NEW_BLOCK) {
                builder.start_new_block(true);
            }
            if (job_ptr->huffman_table) {
                OWN_QPL_CHECK_STATUS(check_huffman_table_is_correct<compression_algorithm_e::deflate>(job_ptr->huffman_table))
                auto table_impl = use_as_huffman_table<compression_algorithm_e::deflate>(job_ptr->huffman_table);

                auto table_ptr = reinterpret_cast<qpl_compression_huffman_table*>(table_impl->compression_huffman_table<path>());

                builder.compression_table(table_ptr);
            }
        }

        if (job_ptr->mini_block_size) {
            builder.enable_indexing(static_cast<mini_block_size_t>(job_ptr->mini_block_size),
                                    job_ptr->idx_array,
                                    job_ptr->idx_num_written,
                                    job_ptr->idx_max_size);
        }

        if (job_ptr->dictionary != nullptr &&
            job_ptr->flags & QPL_FLAG_FIRST) {
            if constexpr (qpl::ml::execution_path_t::software == path) {
                builder.dictionary(*job_ptr->dictionary);
            } else {
                return qpl::ml::status_list::not_supported_err;
            }
        }

        auto state = builder.verify(!(job_ptr->flags & QPL_FLAG_OMIT_VERIFY))
                            .build();

        if (job_ptr->flags & QPL_FLAG_CANNED_MODE) { // LZ Only
            result = deflate<path, deflate_mode_t::deflate_no_headers>(state,
                                                                       job_ptr->next_in_ptr,
                                                                       job_ptr->available_in);
        } else {
            if (job_ptr->flags & QPL_FLAG_GZIP_MODE) {
                result = gzip_decorator::wrap(deflate<path, deflate_mode_t::deflate_default>,
                                              state,
                                              job_ptr->next_in_ptr,
                                              job_ptr->available_in);
            } else if (job_ptr->flags & QPL_FLAG_ZLIB_MODE) {
                result = zlib_decorator::wrap(deflate<path, deflate_mode_t::deflate_default>,
                                              state,
                                              job_ptr->next_in_ptr,
                                              job_ptr->available_in);
            } else {
                result = default_decorator::wrap(deflate<path, deflate_mode_t::deflate_default>,
                                                 state,
                                                 job_ptr->next_in_ptr,
                                                 job_ptr->available_in);
            }
        }
    }

    job::update(job_ptr, result);

    return result.status_code_;
}

template
uint32_t perform_compression<ml::execution_path_t::hardware>(qpl_job *const job_ptr) noexcept;

template
uint32_t perform_compression<ml::execution_path_t::software>(qpl_job *const job_ptr) noexcept;
}
