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
