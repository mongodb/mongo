/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#include "buffer.hpp"

namespace qpl::ml {

[[nodiscard]] auto buffer_t::size() const noexcept -> uint32_t {
    return static_cast<uint32_t>(std::distance(begin_, end_));
}

[[nodiscard]] auto buffer_t::data() const noexcept -> uint8_t * {
    return begin_;
}

[[nodiscard]] auto buffer_t::begin() const noexcept -> uint8_t * {
    return begin_;
}

[[nodiscard]] auto buffer_t::end() const noexcept -> uint8_t * {
    return end_;
}

} // namespace qpl::ml::analytics
