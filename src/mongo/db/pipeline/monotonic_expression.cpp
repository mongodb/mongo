// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/monotonic_expression.h"

#include "mongo/util/assert_util.h"

namespace mongo::monotonic {

State opposite(State state) {
    switch (state) {
        case State::NonMonotonic:
        case State::Constant:
            return state;
        case State::Increasing:
            return State::Decreasing;
        case State::Decreasing:
            return State::Increasing;
    };
    MONGO_UNREACHABLE;
}

State combine(State lhs, State rhs) {
    if (lhs == State::NonMonotonic || rhs == State::NonMonotonic) {
        return State::NonMonotonic;
    }
    if (lhs == rhs || lhs == State::Constant) {
        return rhs;
    }
    if (rhs == State::Constant) {
        return lhs;
    }
    return State::NonMonotonic;
}

}  // namespace mongo::monotonic
