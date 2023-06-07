/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/*
 *  Intel® Query Processing Library (Intel® QPL)
 *  Middle Layer API (private C++ API)
 */

#ifndef QPL_ZLIB_DECORATOR_HPP_
#define QPL_ZLIB_DECORATOR_HPP_

#include "compression/inflate/isal_kernels_wrappers.hpp"
#include "compression/compression_defs.hpp"
#include "compression/verification/verification_defs.hpp"
#include "common/defs.hpp"
#include "compression/deflate/streams/compression_stream.hpp"
#include "util/checksum.hpp"

namespace qpl::ml::compression {

class zlib_decorator {
public:
    template <class F, class state_t, class ...arguments>
    static auto unwrap(F function, state_t &state, arguments... args) noexcept -> decompression_operation_result_t;

    template <class F, class state_t, class ...arguments>
    static auto wrap(F function, state_t &state, arguments... args) noexcept -> compression_operation_result_t;

    struct zlib_header {
        uint8_t compression_info;
        uint8_t flags;
        bool dictionary_flag;
        uint32_t dictionary_id;
        uint8_t compression_level;
        uint32_t byte_size;
    };

    static auto read_header(const uint8_t *stream_ptr, uint32_t stream_size, zlib_header &header) noexcept -> qpl_ml_status;
};

}

#endif //QPL_ZLIB_DECORATOR_HPP_
