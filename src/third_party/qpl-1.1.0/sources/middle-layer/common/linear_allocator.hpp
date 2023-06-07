/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#include <iterator>
#include <memory>

#include "allocation_buffer_t.hpp"

#ifndef QPL_SOURCES_MIDDLE_LAYER_COMPRESSION_CONTAINERS_BUFFER_HPP_
#define QPL_SOURCES_MIDDLE_LAYER_COMPRESSION_CONTAINERS_BUFFER_HPP_

namespace qpl::ml::util {

enum class memory_block_t {
    not_aligned,
    aligned_64u,
};

class linear_allocator {
public:
    linear_allocator(allocation_buffer_t &buffer) : buffer_(buffer) {
    };

    template <class U>
    constexpr linear_allocator(const linear_allocator &allocator) noexcept
            :buffer_(allocator.buffer_) {
    }

    template <class T, memory_block_t align = memory_block_t::not_aligned>
    [[nodiscard]] inline auto allocate(size_t n = 1u) const noexcept -> T * {
        uint8_t *ptr     = buffer_.data();
        void    *new_ptr = ptr;

        auto byte_size = sizeof(T) * n;

        if constexpr (align == memory_block_t::aligned_64u) {

            auto capacity = buffer_.capacity();

            if (std::align(64u, byte_size, new_ptr, capacity)) {
                byte_size += std::distance(ptr, reinterpret_cast<uint8_t *>(new_ptr));
            }
            else {
                // if it is not possible to align memory,
                // set output to nullptr and not shift buffer
                new_ptr   = nullptr;
                byte_size = 0;
            }
        }

        buffer_.shift_data(byte_size);

        return reinterpret_cast<T *>(new_ptr);
    }

private:
    allocation_buffer_t &buffer_;
};

template <class T, class U>
bool operator==(const linear_allocator &, const linear_allocator &) {
    return false;
}

template <class T, class U>
bool operator!=(const linear_allocator &, const linear_allocator &) {
    return true;
}

}

#endif //QPL_SOURCES_MIDDLE_LAYER_COMPRESSION_CONTAINERS_BUFFER_HPP_
