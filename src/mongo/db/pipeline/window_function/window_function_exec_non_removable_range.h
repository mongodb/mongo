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

#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/window_function/partition_iterator.h"
#include "mongo/db/pipeline/window_function/window_function_exec_non_removable.h"

namespace mongo {

/**
 * An executor that handles left-unbounded, range-based windows.
 */
class WindowFunctionExecNonRemovableRange final : public WindowFunctionExec {
public:
    WindowFunctionExecNonRemovableRange(PartitionIterator* iter,
                                        boost::intrusive_ptr<Expression> input,
                                        boost::intrusive_ptr<ExpressionFieldPath> sortExpr,
                                        boost::intrusive_ptr<AccumulatorState> function,
                                        WindowBounds bounds,
                                        MemoryUsageTracker::PerFunctionMemoryTracker* memTracker)
        : WindowFunctionExec(PartitionAccessor(iter, PartitionAccessor::Policy::kRightEndpoint),
                             memTracker),
          _input(std::move(input)),
          _sortExpr(std::move(sortExpr)),
          _function(std::move(function)),
          _bounds(bounds) {}

    Value getNext() final {
        update();
        return _function->getValue(false);
    }

    void reset() final {
        _function->reset();
        _lastEndpoints = boost::none;
    }

private:
    void update() {
        auto endpoints = _iter.getEndpoints(_bounds, _lastEndpoints);
        // There are 4 different transitions we can make:
        if (_lastEndpoints) {
            if (endpoints) {
                // Transition from nonempty to nonempty: add new values based on how the window
                // changed.
                for (int i = _lastEndpoints->second + 1; i <= endpoints->second; ++i) {
                    addValueAt(i);
                }
            } else {
                // Transition from nonempty to empty: discard the accumulator state.
                _function->reset();
                _memTracker->set(_function->getMemUsage());
            }
        } else {
            if (endpoints) {
                // Transition from empty to nonempty: add the new values.
                for (int i = endpoints->first; i <= endpoints->second; ++i) {
                    addValueAt(i);
                }
            } else {
                // Transition from empty to empty: nothing to do!
            }
        }

        if (endpoints) {
            // Shift endpoints by 1 because we will have advanced by 1 document on the next call
            // to update().
            _lastEndpoints = std::pair(endpoints->first - 1, endpoints->second - 1);
        } else {
            _lastEndpoints = boost::none;
        }
    }
    void addValueAt(int offset) {
        auto doc = _iter[offset];
        tassert(5429411, "endpoints must fall in the partition", doc);
        Value v = _input->evaluate(*doc, &_input->getExpressionContext()->variables);
        _function->process(v, false);
        _memTracker->set(_function->getMemUsage());
    }

    boost::intrusive_ptr<Expression> _input;
    boost::intrusive_ptr<ExpressionFieldPath> _sortExpr;
    boost::intrusive_ptr<AccumulatorState> _function;
    WindowBounds _bounds;

    boost::optional<std::pair<int, int>> _lastEndpoints;
};
}  // namespace mongo
