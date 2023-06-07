/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#include "limited_buffer.hpp"

namespace qpl::ml {

[[nodiscard]] auto limited_buffer_t::max_elements_count() const noexcept -> uint32_t {
    return max_elements_in_buffer_;
}

[[nodiscard]] auto limited_buffer_t::data() const noexcept -> uint8_t * {
    return buffer_t::data() + byte_shift_;
}

} // namespace qpl::ml
