/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

// IWYU pragma: no_include "boost/container/detail/std_fwd.hpp"

#include "mongo/db/pipeline/window_function/window_function_exec_removable_range.h"

#include "mongo/db/pipeline/expression_context.h"
#include "mongo/util/assert_util.h"

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

using boost::optional;
using std::pair;

namespace mongo {

WindowFunctionExecRemovableRange::WindowFunctionExecRemovableRange(
    PartitionIterator* iter,
    boost::intrusive_ptr<Expression> input,
    boost::intrusive_ptr<ExpressionFieldPath> sortBy,
    std::unique_ptr<WindowFunctionState> function,
    WindowBounds bounds,
    SimpleMemoryUsageTracker* memTracker)
    : WindowFunctionExecRemovable(iter,
                                  PartitionAccessor::Policy::kEndpoints,
                                  std::move(input),
                                  std::move(function),
                                  memTracker),
      _sortBy(std::move(sortBy)),
      _bounds(std::move(bounds)) {}

namespace {
struct EndpointsChange {
    optional<pair<int, int>> added;
    optional<pair<int, int>> removed;
};

/**
 * Diffs two intervals: the result is expressed as two new intervals, for the added and removed
 * elements. The intervals are all represented as inclusive [lower, upper], so an empty interval
 * is represented as boost::none.
 *
 * For example, in 'diff([2, 10], [5, 14])' the lower bound changed from 2 to 5, so 'removed' is
 * [2, 4], and the upper bound changed from 10 to 14, so 'added' is [11, 14].
 */
EndpointsChange diffEndpoints(optional<pair<int, int>> old, optional<pair<int, int>> current) {
    EndpointsChange result;

    if (!old && !current) {
        return result;
    }
    if (!old) {
        result.added = current;
        return result;
    }
    if (!current) {
        result.removed = old;
        return result;
    }

    auto [oldLower, oldUpper] = *old;
    auto [lower, upper] = *current;
    tassert(5429407, "Endpoints should never decrease.", oldLower <= lower && oldUpper <= upper);
    if (oldLower < lower) {
        result.removed = std::pair(oldLower, lower - 1);
    }
    if (oldUpper < upper) {
        result.added = std::pair(oldUpper + 1, upper);
    }
    return result;
}
}  // namespace

void WindowFunctionExecRemovableRange::update() {
    // Calling getEndpoints here also informs the PartitionAccessor that we won't need documents
    // to the left of endpoints->first. However, we need to access those documents here in
    // update(), to remove them from the WindowFunctionState. This is ok, because the documents
    // expire later, on the next call to releaseExpired(). We can still use the documents between
    // _lastEndpoints during this update().
    auto endpoints = _iter.getEndpoints(_bounds, _lastEndpoints);
    auto [added, removed] = diffEndpoints(_lastEndpoints, endpoints);

    if (added) {
        auto [lower, upper] = *added;
        for (auto i = lower; i <= upper; ++i) {
            addValue(_input->evaluate(*_iter[i], &_input->getExpressionContext()->variables));
        }
    }
    if (removed) {
        auto [lower, upper] = *removed;
        for (auto i = lower; i <= upper; ++i) {
            removeValue();
        }
    }

    // Update _lastEndpoints.
    if (endpoints) {
        auto [lower, upper] = *endpoints;
        // On the next call to update(), we will have advanced by 1 document.
        // The document we call '0' now, will be called '-1' on that next update().
        _lastEndpoints = std::pair(lower - 1, upper - 1);
    } else {
        _lastEndpoints = boost::none;
    }
}

}  // namespace mongo
