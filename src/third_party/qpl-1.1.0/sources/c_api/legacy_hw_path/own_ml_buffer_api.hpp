/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#ifndef QPL_SOURCES_HW_PATH_SOURCES_OWN_ML_QPL_BUFFER_API_H_
#define QPL_SOURCES_HW_PATH_SOURCES_OWN_ML_QPL_BUFFER_API_H_

#include "util/memory.hpp"

#ifdef __cplusplus
extern "C" {
#endif

static inline uint8_t *own_qpl_buffer_get_data(qpl_buffer *const buffer_ptr) {
    return buffer_ptr->data;
}

static inline uint32_t own_qpl_buffer_get_size(qpl_buffer *const buffer_ptr) {
    return buffer_ptr->actual_bytes;
}

static inline uint32_t own_qpl_buffer_get_capacity(qpl_buffer *const UNREFERENCED_PARAMETER(buffer_ptr)) {
    return QPL_INTERNAL_BUFFER_SIZE;
}

static inline void own_qpl_buffer_fill(qpl_buffer *const buffer_ptr,
                                       uint8_t *const source_ptr,
                                       const uint32_t source_size) {
    qpl::ml::util::copy(source_ptr,
                        source_ptr + source_size,
                        buffer_ptr->data + buffer_ptr->actual_bytes);
    buffer_ptr->actual_bytes += source_size;
}

static inline bool own_qpl_buffer_is_empty(const qpl_buffer *const buffer_ptr) {
    return 0u == buffer_ptr->actual_bytes;
}

static inline bool own_qpl_buffer_touch(const qpl_buffer *const buffer_ptr,
                                        const uint32_t source_size) {
    return (source_size + buffer_ptr->actual_bytes) < QPL_INTERNAL_BUFFER_SIZE;
}

#ifdef __cplusplus
}
#endif

#endif //QPL_SOURCES_HW_PATH_SOURCES_OWN_ML_QPL_BUFFER_API_H_
