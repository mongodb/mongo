/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/*
 *  Intel® Query Processing Library (Intel® QPL)
 *  Middle Layer API (private C++ API)
 */

#ifndef QPL_GZIP_DECORATOR_HPP_
#define QPL_GZIP_DECORATOR_HPP_

#include "compression/inflate/isal_kernels_wrappers.hpp"
#include "compression/compression_defs.hpp"
#include "compression/verification/verification_defs.hpp"
#include "common/defs.hpp"
#include "compression/deflate/streams/compression_stream.hpp"

namespace qpl::ml::compression {

constexpr uint32_t OWN_GZIP_HEADER_LENGTH  = 10u;

extern std::array<uint8_t, OWN_GZIP_HEADER_LENGTH> default_gzip_header;

class gzip_decorator {
public:
    template <class F, class state_t, class ...arguments>
    static auto unwrap(F function, state_t &state, arguments... args) noexcept -> decompression_operation_result_t;

    template <class F, class state_t, class ...arguments>
    static auto wrap(F function, state_t &state, arguments... args) noexcept -> compression_operation_result_t;

    struct gzip_header {
        uint8_t ID1;
        uint8_t ID2;
        uint8_t compression_method;
        uint8_t flags;
        // uint8_t *comment;
        // uint8_t extra_flags;
        uint32_t modification_time;
        uint8_t os;
        uint16_t crc16;
        uint32_t byte_size;
    };

    struct gzip_trailer {
        uint32_t crc32;
        uint32_t input_size;
    };

    static auto read_header(const uint8_t *destination_ptr, uint32_t stream_size, gzip_header &header) noexcept -> qpl_ml_status;

    static inline void write_header_unsafe(const uint8_t *destination_ptr, 
                                           uint32_t UNREFERENCED_PARAMETER(size)) noexcept {
        *(uint64_t *) (destination_ptr)      = *(uint64_t *) (&default_gzip_header[0]);
        *(uint16_t *) (destination_ptr + 8u) = *(uint16_t *) (&default_gzip_header[8]);
    }

    static inline auto write_header(const uint8_t *destination_ptr, uint32_t size, gzip_header &header) noexcept;

    static inline auto read_trailer(const uint8_t *destination_ptr, uint32_t size, gzip_trailer &trailer) noexcept -> int32_t;

    static inline void write_trailer_unsafe(const uint8_t *destination_ptr, 
                                            uint32_t UNREFERENCED_PARAMETER(size), 
                                            gzip_trailer &trailer) noexcept {
        *(uint32_t *) (destination_ptr)      = trailer.crc32;
        *(uint32_t *) (destination_ptr + 4u) = trailer.input_size;
    }
};

}

#endif //QPL_GZIP_DECORATOR_HPP_
