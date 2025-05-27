/**
 *    Copyright (C) 2022-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/db/pipeline/field_path.h"

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
