// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/window_function/partition_iterator.h"
#include "mongo/db/pipeline/window_function/window_function_exec.h"
#include "mongo/util/modules.h"

namespace mongo {

/**
 * A (virtual) executor that handles left-unbounded, range-based windows.
 *
 * Concrete instances must implement the virtual interfaces that inform this class how to
 * update the window, get the current window value, reset the window, and get the current mem usage.
 */
class WindowFunctionExecNonRemovableRangeCommon : public WindowFunctionExec {
public:
    WindowFunctionExecNonRemovableRangeCommon(PartitionIterator* iter,
                                              boost::intrusive_ptr<Expression> input,
                                              boost::intrusive_ptr<ExpressionFieldPath> sortExpr,
                                              WindowBounds bounds,
                                              SimpleMemoryUsageTracker* memTracker)
        : WindowFunctionExec(PartitionAccessor(iter, PartitionAccessor::Policy::kRightEndpoint),
                             memTracker),
          _input(std::move(input)),
          _sortExpr(std::move(sortExpr)),
          _bounds(bounds) {}

    Value getNext(boost::optional<Document> current = boost::none) final {
        update();
        return getWindowValue(current);
    }

    void reset() final {
        resetWindow();
        _memTracker->set(0);
        _lastEndpoints = boost::none;
    }

protected:
    virtual void updateWindow(const Value& input) = 0;
    virtual void resetWindow() = 0;
    virtual Value getWindowValue(boost::optional<Document> current) = 0;
    virtual int64_t getMemUsage() = 0;

    boost::intrusive_ptr<Expression> _input;
    boost::intrusive_ptr<ExpressionFieldPath> _sortExpr;
    WindowBounds _bounds;

private:
    boost::optional<std::pair<int, int>> _lastEndpoints;

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
                resetWindow();
                _memTracker->set(getMemUsage());
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
        updateWindow(_input->evaluate(*doc, &_input->getExpressionContext()->variables));
        _memTracker->set(getMemUsage());
    }
};
}  // namespace mongo
