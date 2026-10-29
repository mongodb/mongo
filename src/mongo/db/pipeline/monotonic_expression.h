// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/pipeline/field_path.h"
#include "mongo/util/modules.h"

#include <numeric>

namespace mongo::monotonic {

enum class State { NonMonotonic, Constant, Increasing, Decreasing };

/**
 * Given monotonic states of function f(x), returns monotonic state of -f(x). If function is
 * constant or non monotonic, it will remain the same. If it is increasing, it will become
 * decreasing, and vice versa.
 */
State opposite(State state);

/**
 * Given monotonic states of functions f(x) and g(x), returns monotonic state of f(x)+g(x). Plus
 * operator can be replaced with any operation that preserves monotonic behavior.
 *
 * If any argument is non monotonic, then the whole function is non monotonic.
 * If one of the arguments is a constant, then the whole function has the same monotonic state as
 * the other argument. If all arguments have the the same monotonic state, then the whole function
 * has the same monotonic state. Otherwise, the result is NonMonotonic.
 */
State combine(State lhs, State rhs);

template <typename ExpressionsContainer>
State combineExpressions(const FieldPath& sortedFieldPath, const ExpressionsContainer& container) {
    return std::accumulate(container.begin(),
                           container.end(),
                           State::Constant,
                           [&](State state, const auto& expression) {
                               if (expression == nullptr) {
                                   return state;
                               }
                               return combine(state,
                                              expression->getMonotonicState(sortedFieldPath));
                           });
}

}  // namespace mongo::monotonic
