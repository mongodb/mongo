/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/*
 *  Intel® Query Processing Library (Intel® QPL)
 *  Middle Layer API (public C++ API)
 */

#ifndef QPL_SOURCES_MIDDLE_LAYER_COMMON_ALLOCATION_BUFFER_T_HPP_
#define QPL_SOURCES_MIDDLE_LAYER_COMMON_ALLOCATION_BUFFER_T_HPP_

#include "util/util.hpp"
#include "buffer.hpp"

namespace qpl::ml {

class allocation_buffer_t : public buffer_t {
public:
    template <class iterator_t>
    explicit allocation_buffer_t(iterator_t begin, iterator_t end)
            : buffer_t(begin, end),
              current_(&*begin) {
        // No actions required
    }

    [[nodiscard]] inline auto data() const noexcept -> uint8_t * override {
        return current_;
    }

    [[nodiscard]] inline auto capacity() const noexcept -> size_t {
        return std::distance(current_, buffer_t::end());
    }

    inline auto shift_data(size_t shift_size) noexcept -> void {
        current_ += shift_size;
    }

    static auto empty() -> allocation_buffer_t {
        return {};
    }

private:
    allocation_buffer_t() noexcept = default;

    uint8_t *current_ = nullptr;
};

}

#endif //QPL_SOURCES_MIDDLE_LAYER_COMMON_ALLOCATION_BUFFER_T_HPP_
