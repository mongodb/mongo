/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/*
 *  Intel® Query Processing Library (Intel® QPL)
 *  Middle Layer API (private C++ API)
 */

#ifndef QPL_SOURCES_MIDDLE_LAYER_COMPRESSION_VERIFICATION_VERIFICATION_STATE_BUILDER_HPP_
#define QPL_SOURCES_MIDDLE_LAYER_COMPRESSION_VERIFICATION_VERIFICATION_STATE_BUILDER_HPP_

#include "common/defs.hpp"
#include "compression/verification/verification_state.hpp"

namespace qpl::ml::compression {

template <execution_path_t path>
class verification_state_builder;

template <>
class verification_state_builder<execution_path_t::software> {
protected:
    using common_type = verification_state_builder<execution_path_t::software>;
    using state_type = verify_state<execution_path_t::software>;

public:
    static auto create(const qpl::ml::util::linear_allocator &allocator) -> common_type {
        auto builder = verification_state_builder<execution_path_t::software>(allocator);
        builder.state_.first(true);
        builder.state_.reset();
        builder.state_.crc_seed(0);
        builder.state_.reset_state();
        builder.state_.reset_miniblock_state();

        return builder;
    }

    static auto restore(const qpl::ml::util::linear_allocator &allocator) -> common_type {
        auto builder = verification_state_builder<execution_path_t::software>(allocator);
        builder.state_.first(false);
        builder.state_.reset();

        return builder;
    }

    inline auto build() noexcept -> state_type {

        return state_;
    }

private:
    state_type state_;

    verification_state_builder(const qpl::ml::util::linear_allocator &allocator) : state_(allocator) {
        // No actions required
    };
};

}

#endif //QPL_SOURCES_MIDDLE_LAYER_COMPRESSION_VERIFICATION_VERIFICATION_STATE_BUILDER_HPP_
