/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#ifndef QPL_MIDDLE_LAYER_COMPRESSION_IMPLEMENTATIONS_COMPRESSION_IMPLEMENTATION_HPP
#define QPL_MIDDLE_LAYER_COMPRESSION_IMPLEMENTATIONS_COMPRESSION_IMPLEMENTATION_HPP

#include <cstdint>
#include <array>

#include "common/defs.hpp"
#include "compression/compression_defs.hpp"

namespace qpl::ml::compression {

template <class stream_t>
class implementation {
public:
    using handler_t = auto (*)(stream_t &, compression_state_t &) noexcept -> qpl_ml_status;

    implementation() = delete;

    constexpr explicit
    implementation(std::initializer_list<std::pair<compression_state_t, handler_t>> handlers) {
        for (const auto &it : handlers) {
            handlers_[static_cast<uint32_t>(it.first)] = it.second;
        }
    }

    constexpr implementation(const implementation &other) noexcept = default;

    constexpr implementation(implementation &&other) noexcept = default;

    constexpr auto operator=(const implementation &other) noexcept -> implementation & = default;

    constexpr auto operator=(implementation &&other) noexcept -> implementation & = default;

    auto execute(stream_t &stream, compression_state_t &state) const noexcept -> qpl_ml_status {
        return handlers_[static_cast<uint32_t>(state)](stream, state);
    }

protected:
    std::array<handler_t, static_cast<int>(compression_state_t::count)> handlers_{};
};

} // namespace qpl::ml::compression

#endif // QPL_MIDDLE_LAYER_COMPRESSION_IMPLEMENTATIONS_COMPRESSION_IMPLEMENTATION_HPP
