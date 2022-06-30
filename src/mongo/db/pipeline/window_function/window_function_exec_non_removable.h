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
#include "mongo/db/pipeline/window_function/window_bounds.h"
#include "mongo/db/pipeline/window_function/window_function_exec.h"

namespace mongo {

/**
 * An executor that specifically handles document-based window types which accumulate values with no
 * need to remove old ones.
 *
 * Only the upper bound is needed as the lower bound is always considered to be unbounded.
 */
class WindowFunctionExecNonRemovable final : public WindowFunctionExec {
public:
    /**
     * Constructs a non-removable window function executor with the given input expression to be
     * evaluated and passed to the corresponding WindowFunc for each document in the window.
     *
     * The 'upperDocumentBound' parameter is the user-supplied upper bound for the window, which may
     * be "current", "unbounded" or an integer. A negative integer will look behind the current
     * document and a positive integer will look ahead.
     */
    WindowFunctionExecNonRemovable(PartitionIterator* iter,
                                   boost::intrusive_ptr<Expression> input,
                                   boost::intrusive_ptr<AccumulatorState> function,
                                   WindowBounds::Bound<int> upperDocumentBound,
                                   MemoryUsageTracker::PerFunctionMemoryTracker* memTracker)
        : WindowFunctionExec(PartitionAccessor(iter, PartitionAccessor::Policy::kDefaultSequential),
                             memTracker),
          _input(std::move(input)),
          _function(std::move(function)),
          _upperDocumentBound(upperDocumentBound){};

    Value getNext() final {
        if (!_initialized) {
            initialize();
        } else if (!stdx::holds_alternative<WindowBounds::Unbounded>(_upperDocumentBound)) {
            // Right-unbounded windows will accumulate all values on the first pass during
            // initialization.
            auto upperIndex = [&]() {
                if (stdx::holds_alternative<WindowBounds::Current>(_upperDocumentBound))
                    return 0;
                else
                    return stdx::get<int>(_upperDocumentBound);
            }();

            if (auto doc = (this->_iter)[upperIndex]) {
                _function->process(
                    _input->evaluate(*doc, &_input->getExpressionContext()->variables), false);
                _memTracker->set(_function->getMemUsage());
            } else {
                // Upper bound is out of range, but may be because it's off of the end of the
                // partition. For instance, for bounds [unbounded, -1] we won't be able to
                // access the upper bound until the second call to getNext().
            }
        }

        return _function->getValue(false);
    }

    void reset() final {
        _initialized = false;
        _function->reset();
        _memTracker->set(0);
    }

private:
    boost::intrusive_ptr<Expression> _input;
    boost::intrusive_ptr<AccumulatorState> _function;
    WindowBounds::Bound<int> _upperDocumentBound;

    // In one of two states: either the initial window has not been populated or we are sliding and
    // accumulating a single value per iteration.
    bool _initialized = false;

    void initialize() {
        auto needMore = [&](int index) {
            return stdx::visit(
                OverloadedVisitor{
                    [&](const WindowBounds::Unbounded&) { return true; },
                    [&](const WindowBounds::Current&) { return index == 0; },
                    [&](const int& n) { return index <= n; },
                },
                _upperDocumentBound);
        };

        _initialized = true;
        for (int i = 0; needMore(i); i++) {
            if (auto doc = (this->_iter)[i]) {
                _function->process(
                    _input->evaluate(*doc, &_input->getExpressionContext()->variables), false);
                _memTracker->set(_function->getMemUsage());
            } else {
                // Already reached the end of partition for the first value to compute.
                break;
            }
        }
    }
};

}  // namespace mongo
