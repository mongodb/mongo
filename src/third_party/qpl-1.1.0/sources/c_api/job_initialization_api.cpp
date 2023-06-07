/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/*
 *  Intel® Query Processing Library (Intel® QPL)
 *  Job API (public C API)
 */

#include <algorithm> // std::max

#include "qpl/qpl.h"
#include "util/memory.hpp"
#include "util/hw_status_converting.hpp"
#include "compression/verification/verification_state.hpp"

// Legacy
#include "own_defs.h"
#include "compression_operations/compression_state_t.h"
#include "filter_operations/analytics_state_t.h"
#include "legacy_hw_path/async_hw_api.h"
#include "legacy_hw_path/hardware_state.h"
#include "compression_operations/own_deflate_job.h" // @todo check if could be removed

// get_buffer_size functions for middle-layer buffer allocation
#include "compression/deflate/streams/sw_deflate_state.hpp"
#include "compression/deflate/streams/hw_deflate_state.hpp"
#include "compression/inflate/inflate_state.hpp"
#include "compression/huffman_only/huffman_only_compression_state.hpp"
#include "compression/huffman_only/huffman_only_decompression_state.hpp"
#include "compression/verification/verification_state.hpp"

#ifdef __cplusplus
extern "C" {
#endif

QPL_INLINE uint32_t own_get_job_size_compress  (qpl_path_t qpl_path);
QPL_INLINE uint32_t own_get_job_size_decompress(qpl_path_t qpl_path);
QPL_INLINE uint32_t own_get_job_size_analytics (qpl_path_t qpl_path);
uint32_t            own_get_job_size_middle_layer_buffer(qpl_path_t qpl_path);

QPL_INLINE void own_init_compress  (qpl_job *qpl_job_ptr);
QPL_INLINE void own_init_decompress(qpl_job *qpl_job_ptr);
QPL_INLINE void own_init_analytics (qpl_job *qpl_job_ptr);

QPL_FUN(qpl_status, qpl_get_job_size, (qpl_path_t qpl_path, uint32_t *job_size_ptr)) {
    QPL_BAD_PTR_RET(job_size_ptr);
    QPL_BADARG_RET (qpl_path_auto > qpl_path || qpl_path_software < qpl_path, QPL_STS_PATH_ERR);

    // qpl_job_ptr can have any alignment,
    // therefore need to add additional bytes to be able to align pointers
    *job_size_ptr  = QPL_ALIGNED_SIZE(sizeof(qpl_job), QPL_DEFAULT_ALIGNMENT) + QPL_DEFAULT_ALIGNMENT;

    // add storage required for internal stuctures
    *job_size_ptr += QPL_ALIGNED_SIZE(own_get_job_size_compress(qpl_path), QPL_DEFAULT_ALIGNMENT);
    *job_size_ptr += QPL_ALIGNED_SIZE(own_get_job_size_decompress(qpl_path), QPL_DEFAULT_ALIGNMENT);
    *job_size_ptr += QPL_ALIGNED_SIZE(own_get_job_size_analytics(qpl_path), QPL_DEFAULT_ALIGNMENT);
    *job_size_ptr += QPL_ALIGNED_SIZE(own_get_job_size_middle_layer_buffer(qpl_path), QPL_DEFAULT_ALIGNMENT);

    if (qpl_path_hardware == qpl_path || qpl_path_auto == qpl_path) {
        *job_size_ptr += QPL_ALIGNED_SIZE(hw_get_job_size(), QPL_DEFAULT_ALIGNMENT);
    }

    return QPL_STS_OK;
}

QPL_FUN(qpl_status, qpl_init_job, (qpl_path_t qpl_path, qpl_job *qpl_job_ptr)) {
    using namespace qpl::ml;

    QPL_BADARG_RET (qpl_path_auto > qpl_path || qpl_path_software < qpl_path, QPL_STS_PATH_ERR);
    QPL_BAD_PTR_RET(qpl_job_ptr);

    uint32_t       status                   = QPL_STS_OK;
    const uint32_t job_size                 = QPL_ALIGNED_SIZE(sizeof(qpl_job), QPL_DEFAULT_ALIGNMENT);
    const uint32_t comp_size                = QPL_ALIGNED_SIZE(own_get_job_size_compress(qpl_path), QPL_DEFAULT_ALIGNMENT);
    const uint32_t decomp_size              = QPL_ALIGNED_SIZE(own_get_job_size_decompress(qpl_path), QPL_DEFAULT_ALIGNMENT);
    const uint32_t analytics_size           = QPL_ALIGNED_SIZE(own_get_job_size_analytics(qpl_path), QPL_DEFAULT_ALIGNMENT);
    const uint32_t middle_layer_buffer_size = QPL_ALIGNED_SIZE(own_get_job_size_middle_layer_buffer(qpl_path), QPL_DEFAULT_ALIGNMENT);

    util::set_zeros((uint8_t *) qpl_job_ptr, job_size);

    // qpl_job_ptr can have any alignment when allocated,
    // therefore need to manually calculate and align pointers to the auxiliary buffers
    qpl_job_ptr->data_ptr.compress_state_ptr =
            (uint8_t *) QPL_ALIGNED_PTR(((uint8_t *) qpl_job_ptr), QPL_DEFAULT_ALIGNMENT) + job_size;
    qpl_job_ptr->data_ptr.decompress_state_ptr    = qpl_job_ptr->data_ptr.compress_state_ptr + comp_size;
    qpl_job_ptr->data_ptr.analytics_state_ptr     = qpl_job_ptr->data_ptr.decompress_state_ptr + decomp_size;
    qpl_job_ptr->data_ptr.middle_layer_buffer_ptr = qpl_job_ptr->data_ptr.analytics_state_ptr + analytics_size;
    qpl_job_ptr->data_ptr.hw_state_ptr            = qpl_job_ptr->data_ptr.middle_layer_buffer_ptr + middle_layer_buffer_size;
    qpl_job_ptr->data_ptr.path                    = qpl_path;

#ifdef linux
    if (qpl_path_hardware == qpl_path || qpl_path_auto == qpl_path) {
        qpl_job_ptr->numa_id = -1;

        auto *const hw_state_ptr = (qpl_hw_state *) (qpl_job_ptr->data_ptr.hw_state_ptr);
        uint32_t hw_size = QPL_ALIGNED_SIZE(hw_get_job_size(), QPL_DEFAULT_ALIGNMENT);

        util::set_zeros((uint8_t *) hw_state_ptr, hw_size);

        if (qpl_path_hardware == qpl_job_ptr->data_ptr.path) {
            status = hw_accelerator_get_context(&hw_state_ptr->accel_context);

            if (HW_ACCELERATOR_STATUS_OK != status) {
                qpl_job_ptr->data_ptr.path = qpl_path_software;
                status = util::convert_hw_accelerator_status_to_qpl_status(status);
            }
        }
    }
#else
    if (qpl_path_software != qpl_path) {
        qpl_job_ptr->data_ptr.path = qpl_path_software;
    }
#endif

    // set internal structures to zero
    util::set_zeros((uint8_t *) qpl_job_ptr->data_ptr.compress_state_ptr, comp_size);
    util::set_zeros((uint8_t *) qpl_job_ptr->data_ptr.decompress_state_ptr, decomp_size);
    util::set_zeros((uint8_t *) qpl_job_ptr->data_ptr.analytics_state_ptr, analytics_size);
    util::set_zeros((uint8_t *) qpl_job_ptr->data_ptr.middle_layer_buffer_ptr, middle_layer_buffer_size);

    // initialize internal structures
    // note: ml is just a raw buffer, so no need
    own_init_compress(qpl_job_ptr);
    own_init_decompress(qpl_job_ptr);
    own_init_analytics(qpl_job_ptr);

    return static_cast<qpl_status>(status);
}

QPL_FUN(qpl_status, qpl_fini_job, (qpl_job *qpl_job_ptr)) {
    QPL_BAD_PTR_RET(qpl_job_ptr);
    uint32_t status = QPL_STS_OK;

    if (qpl_path_software != qpl_job_ptr->data_ptr.path) {
        status = hw_accelerator_finalize(&((qpl_hw_state *) qpl_job_ptr->data_ptr.hw_state_ptr)->accel_context);
    }

    return static_cast<qpl_status>(status);
}

/**
 * @brief Returns size of the legacy decompression state
 *
 * @note Currently not in use
 */
QPL_INLINE uint32_t own_get_job_size_decompress(qpl_path_t UNREFERENCED_PARAMETER(qpl_path)) {
    return 0;
}

/**
 * @brief Returns size of the legacy compression state
 *
 * @note Currently only stores middle_layer_compression_style
 */
QPL_INLINE uint32_t own_get_job_size_compress(qpl_path_t UNREFERENCED_PARAMETER(qpl_path)) {
    return QPL_ALIGNED_SIZE(sizeof(own_compression_state_t), QPL_DEFAULT_ALIGNMENT);
}

/**
 * @brief Returns size of the analytics_buffer
 *
 * @note Holds allocations required for performin various analytics operations.
 */
QPL_INLINE uint32_t own_get_job_size_analytics(qpl_path_t UNREFERENCED_PARAMETER(qpl_path)) {
    uint32_t size = 0u;

    size += QPL_ALIGNED_SIZE(sizeof(own_analytics_state_t), QPL_DEFAULT_ALIGNMENT);
    size += QPL_ALIGNED_SIZE(OWN_INFLATE_BUF_SIZE, QPL_DEFAULT_ALIGNMENT);
    size += QPL_ALIGNED_SIZE(OWN_UNPACK_BUF_SIZE, QPL_DEFAULT_ALIGNMENT);
    size += QPL_ALIGNED_SIZE(OWN_SET_BUF_SIZE, QPL_DEFAULT_ALIGNMENT);
    size += QPL_ALIGNED_SIZE(OWN_SRC2_BUF_SIZE, QPL_DEFAULT_ALIGNMENT);

    return size;
}

/**
 * @brief Returns size of the middle_layer_buffer
 *
 * @note The purpose of middle_layer_buffer is to hold all the states that are constructed in middle-layer,
 * e.g., deflate_state stores internal structures needed for deflate compression,
 * inflate_state store internal structures needed for decompression with defaltes, etc.
 * Job structure currently is supposed to be used for either deflate or huffman only mode,
 * and not both at the same time, so it is not necessary to allocate memory required for both states,
 * hence the std::max usage below.
 */
uint32_t own_get_job_size_middle_layer_buffer(qpl_path_t UNREFERENCED_PARAMETER(qpl_path)) {
    using namespace qpl::ml;
    uint32_t size = 0u;

    if (qpl_path_software == qpl_path || qpl_path_auto == qpl_path) {
        uint32_t deflate_size      = 0;
        uint32_t huffman_only_size = 0;

        deflate_size += compression::deflate_state<execution_path_t::software>::get_buffer_size();
        deflate_size += compression::verify_state<execution_path_t::software>::get_buffer_size();
        deflate_size += compression::inflate_state<execution_path_t::software>::get_buffer_size();

        huffman_only_size += compression::huffman_only_state<execution_path_t::software>::get_buffer_size();
        huffman_only_size += compression::huffman_only_decompression_state<execution_path_t::software>::get_buffer_size();

        size += std::max(deflate_size, huffman_only_size);
    }

    if (qpl_path_hardware == qpl_path || qpl_path_auto == qpl_path) {
        uint32_t deflate_size      = 0;
        uint32_t huffman_only_size = 0;

        deflate_size += compression::deflate_state<execution_path_t::hardware>::get_buffer_size();
        deflate_size += compression::verify_state<execution_path_t::hardware>::get_buffer_size();
        deflate_size += compression::inflate_state<execution_path_t::hardware>::get_buffer_size();

        huffman_only_size += compression::huffman_only_state<execution_path_t::hardware>::get_buffer_size();
        huffman_only_size += compression::huffman_only_decompression_state<execution_path_t::hardware>::get_buffer_size();

        size += std::max(deflate_size, huffman_only_size);
    }

    return size;
}

QPL_INLINE void own_init_decompress(qpl_job* UNREFERENCED_PARAMETER(qpl_job_ptr)) {
    return;
}

QPL_INLINE void own_init_compress(qpl_job *qpl_job_ptr) {
    auto *data_ptr = (own_compression_state_t *) qpl_job_ptr->data_ptr.compress_state_ptr;

    data_ptr->middle_layer_compression_style = 0;
}

QPL_INLINE void own_init_analytics(qpl_job *qpl_job_ptr) {
    auto *analytics_state_ptr = (own_analytics_state_t *) qpl_job_ptr->data_ptr.analytics_state_ptr;
    analytics_state_ptr->inflate_buf_size = QPL_ALIGNED_SIZE(OWN_INFLATE_BUF_SIZE, QPL_DEFAULT_ALIGNMENT);
    analytics_state_ptr->unpack_buf_size  = QPL_ALIGNED_SIZE(OWN_UNPACK_BUF_SIZE, QPL_DEFAULT_ALIGNMENT);
    analytics_state_ptr->set_buf_size     = QPL_ALIGNED_SIZE(OWN_SET_BUF_SIZE, QPL_DEFAULT_ALIGNMENT);
    analytics_state_ptr->src2_buf_size    = QPL_ALIGNED_SIZE(OWN_SRC2_BUF_SIZE, QPL_DEFAULT_ALIGNMENT);
    analytics_state_ptr->inflate_buf_ptr =
            (uint8_t *) analytics_state_ptr + QPL_ALIGNED_SIZE(sizeof(own_analytics_state_t), QPL_DEFAULT_ALIGNMENT);
    analytics_state_ptr->unpack_buf_ptr  =
            (uint8_t *) analytics_state_ptr->inflate_buf_ptr + analytics_state_ptr->inflate_buf_size;
    analytics_state_ptr->set_buf_ptr     =
            (uint8_t *) analytics_state_ptr->unpack_buf_ptr + analytics_state_ptr->unpack_buf_size;
    analytics_state_ptr->src2_buf_ptr    =
            (uint8_t *) analytics_state_ptr->set_buf_ptr + analytics_state_ptr->set_buf_size;
}


#ifdef __cplusplus
}
#endif
