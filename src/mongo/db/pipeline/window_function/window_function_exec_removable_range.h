// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/memory_tracking/memory_usage_tracker.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/window_function/partition_iterator.h"
#include "mongo/db/pipeline/window_function/window_bounds.h"
#include "mongo/db/pipeline/window_function/window_function.h"
#include "mongo/db/pipeline/window_function/window_function_exec.h"
#include "mongo/util/modules.h"

#include <memory>
#include <utility>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

/**
 * An executor that handles left-bounded, range-based windows.
 *
 * Left-bounded windows require a window-function state that supports removing elements.
 */
class WindowFunctionExecRemovableRange final : public WindowFunctionExecRemovable {
public:
    /**
     * Constructs a removable window function executor with the given input expression to be
     * evaluated and passed to the corresponding WindowFunc for each document in the window.
     *
     * For example, in
     *     {$setWindowFields: {
     *         sortBy: {ts: 1},
     *         output: {
     *             v: {$max: "$x", window: {range: [-5, 'unbounded']}}
     *         }
     *     }}
     *
     * 'input' is "$x"
     * 'sortBy' is "$ts" (translated from the sort spec, {ts: 1})
     * 'function' is a WindowFunctionMax
     * 'bounds' is WindowBounds::RangeBased{Value{-5}, WindowBounds::Unbounded{}}
     */
    WindowFunctionExecRemovableRange(PartitionIterator* iter,
                                     boost::intrusive_ptr<Expression> input,
                                     boost::intrusive_ptr<ExpressionFieldPath> sortBy,
                                     std::unique_ptr<WindowFunctionState> function,
                                     WindowBounds bounds,
                                     SimpleMemoryUsageTracker* memTracker);

private:
    void doReset() final {
        _lastEndpoints = boost::none;
    }

    void update() final;

    boost::intrusive_ptr<ExpressionFieldPath> _sortBy;
    WindowBounds _bounds;
    boost::optional<std::pair<int, int>> _lastEndpoints;
};
}  // namespace mongo
