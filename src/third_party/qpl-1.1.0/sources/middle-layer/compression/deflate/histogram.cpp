/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#include "compression/deflate/histogram.hpp"
#include "util/descriptor_processing.hpp"
#include "util/memory.hpp"

#include "deflate_hash_table.h"
#include "dispatcher/dispatcher.hpp"
#include "../../../c_api/compression_operations/bit_writer.h"
#include "../../../c_api/compression_operations/own_deflate_job.h"

namespace qpl::ml::compression {

namespace details {
void histogram_join_another(qpl_histogram &first_histogram_ptr,
                            qpl_histogram &second_histogram_ptr) {
    const uint32_t histogram_notes = sizeof(qpl_histogram) / sizeof(uint32_t);

    auto *first_ptr  = reinterpret_cast<uint32_t *>(&first_histogram_ptr);
    auto *second_ptr = reinterpret_cast<uint32_t *>(&second_histogram_ptr);

    for (uint32_t i = 0; i < histogram_notes; i++) {
        first_ptr[i] += second_ptr[i];
    }
}

static inline void isal_histogram_set_statistics(isal_histogram *isal_histogram_ptr,
                                                 const uint32_t *literal_length_histogram_ptr,
                                                 const uint32_t *offsets_histogram_ptr) {
    for (uint32_t i = 0u; i < QPLC_DEFLATE_LL_TABLE_SIZE; i++) {
        isal_histogram_ptr->lit_len_histogram[i] = literal_length_histogram_ptr[i];
    }

    for (uint32_t i = 0; i < QPLC_DEFLATE_D_TABLE_SIZE; i++) {
        isal_histogram_ptr->dist_histogram[i] = offsets_histogram_ptr[i];
    }
}

static inline void isal_histogram_get_statistics(const isal_histogram *isal_histogram_ptr,
                                                 uint32_t *literal_length_histogram_ptr,
                                                 uint32_t *offsets_histogram_ptr) {
    for (uint32_t i = 0u; i < QPLC_DEFLATE_LL_TABLE_SIZE; i++) {
        literal_length_histogram_ptr[i] = (uint32_t) isal_histogram_ptr->lit_len_histogram[i];
    }

    for (uint32_t i = 0u; i < QPLC_DEFLATE_D_TABLE_SIZE; i++) {
        offsets_histogram_ptr[i] = (uint32_t) isal_histogram_ptr->dist_histogram[i];
    }
}

static inline void remove_empty_places_in_histogram(qpl_histogram &histogram) {
    for (unsigned int &literal_length: histogram.literal_lengths) {
        if (literal_length == 0) {
            literal_length = 1;
        }
    }

    for (unsigned int &distance: histogram.distances) {
        if (distance == 0) {
            distance = 1;
        }
    }
}

}

template <>
auto update_histogram<execution_path_t::hardware>(const uint8_t *begin,
                                                  const uint8_t *end,
                                                  deflate_histogram &histogram,
                                                  deflate_level UNREFERENCED_PARAMETER(level)) noexcept -> qpl_ml_status {
    hw_descriptor HW_PATH_ALIGN_STRUCTURE                         descriptor;
    HW_PATH_VOLATILE hw_completion_record HW_PATH_ALIGN_STRUCTURE completion_record;
    qpl_histogram                                                 hw_histogram;

    util::set_zeros(descriptor.data, HW_PATH_DESCRIPTOR_SIZE);
    util::set_zeros(&hw_histogram, sizeof(qpl_histogram));

    hw_iaa_descriptor_init_statistic_collector(&descriptor,
                                               begin,
                                               static_cast<uint32_t>(std::distance(begin, end)),
                                               reinterpret_cast<hw_iaa_histogram *>(&hw_histogram));

    hw_iaa_descriptor_set_completion_record(&descriptor, &completion_record);
    completion_record.status = 0u;

    auto status = util::process_descriptor<qpl_ml_status,
                                           util::execution_mode_t::sync>(&descriptor, &completion_record);

    if (status_list::ok == status) {
        details::histogram_join_another(histogram, hw_histogram);
    }

    details::remove_empty_places_in_histogram(histogram);

    return status;
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstack-usage=4096"
#endif

template <>
auto update_histogram<execution_path_t::software>(const uint8_t *begin,
                                                  const uint8_t *end,
                                                  deflate_histogram &histogram,
                                                  deflate_level level) noexcept -> qpl_ml_status {
    using namespace qpl::ml;

    static const auto &histogram_reset = ((qplc_deflate_histogram_reset_ptr)
            (dispatcher::kernels_dispatcher::get_instance().get_deflate_table()[1]));

    if (qpl_default_level == level) {
        isal_histogram isal_histogram_v = {{0u}, {0u}, {0u}};
        details::isal_histogram_set_statistics(&isal_histogram_v,
                                               histogram.literal_lengths,
                                               histogram.distances);

        // Update ISA-L isal_histogram and create huffman table from it
        isal_update_histogram(const_cast<uint8_t *>(begin),
                              static_cast<int>(std::distance(begin, end)),
                              &isal_histogram_v);

        // Store result
        details::isal_histogram_get_statistics(&isal_histogram_v,
                                               histogram.literal_lengths,
                                               histogram.distances);
    } else {
        own_deflate_job deflateJob = {};

        deflate_histogram_t deflate_histogram_ptr = {};

        uint8_t temporary_buffer[1u];

        uint32_t hash_table[OWN_HIGH_HASH_TABLE_SIZE];
        uint32_t hash_history_table[OWN_HIGH_HASH_TABLE_SIZE];
        deflateJob.histogram_ptr = &deflate_histogram_ptr;

        deflateJob.histogram_ptr->table.hash_table_ptr = hash_table;
        deflateJob.histogram_ptr->table.hash_story_ptr = hash_history_table;

        histogram_reset(&deflate_histogram_ptr);

        deflate_histogram_set_statistics(&deflate_histogram_ptr,
                                         histogram.literal_lengths,
                                         histogram.distances);

        own_initialize_deflate_job(&deflateJob,
                                   begin,
                                   static_cast<uint32_t>(std::distance(begin, end)),
                                   temporary_buffer,
                                   1u,
                                   initial_status,
                                   qpl_gathering_mode);

        own_update_deflate_histogram_high_level(&deflateJob);

        deflate_histogram_get_statistics(&deflate_histogram_ptr,
                                         histogram.literal_lengths,
                                         histogram.distances);
    }

    details::remove_empty_places_in_histogram(histogram);

    return QPL_STS_OK;
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

}
