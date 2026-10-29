// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/memory_tracking/memory_usage_tracker.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/window_function/partition_iterator.h"
#include "mongo/db/pipeline/window_function/window_bounds.h"
#include "mongo/db/pipeline/window_function/window_function_exec.h"
#include "mongo/util/modules.h"

#include <functional>
#include <utility>
#include <variant>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
/**
 * A (virtual) executor that specifically handles document-based window types which only
 * updates values, and does not remove old ones.
 *
 * Only the upper bound is needed as the lower bound is always considered to be unbounded.
 *
 * Concrete instances must implement the virtual interfaces that inform this class how to
 * update the window, get the current window value, reset the window, and get the current mem usage.
 */
class WindowFunctionExecNonRemovableCommon : public WindowFunctionExec {
public:
    WindowFunctionExecNonRemovableCommon(PartitionIterator* iter,
                                         boost::intrusive_ptr<Expression> input,
                                         WindowBounds::Bound<int> upperDocumentBound,
                                         SimpleMemoryUsageTracker* memTracker)
        /**
         * Constructs a non-removable window function executor with the given input expression to be
         * evaluated and passed to the corresponding WindowFunc for each document in the window.
         *
         * The 'upperDocumentBound' parameter is the user-supplied upper bound for the window, which
         * may be "current", "unbounded" or an integer. A negative integer will look behind the
         * current document and a positive integer will look ahead.
         */
        : WindowFunctionExec(PartitionAccessor(iter, PartitionAccessor::Policy::kDefaultSequential),
                             memTracker),
          _input(std::move(input)),
          _upperDocumentBound(upperDocumentBound) {};

    Value getNext(boost::optional<Document> current = boost::none) final {
        if (!_initialized) {
            initialize();
        } else if (!holds_alternative<WindowBounds::Unbounded>(_upperDocumentBound)) {
            // Right-unbounded windows will accumulate all values on the first pass during
            // initialization.
            auto upperIndex = [&]() {
                if (holds_alternative<WindowBounds::Current>(_upperDocumentBound))
                    return 0;
                else
                    return get<int>(_upperDocumentBound);
            }();

            if (auto doc = (this->_iter)[upperIndex]) {
                updateWindow(_input->evaluate(*doc, &_input->getExpressionContext()->variables));
                _memTracker->set(getMemUsage());
            } else {
                // Upper bound is out of range, but may be because it's off of the end of the
                // partition. For instance, for bounds [unbounded, -1] we won't be able to
                // access the upper bound until the second call to getNext().
            }
        }

        return getWindowValue(current);
    }

    void reset() final {
        _initialized = false;
        resetWindow();
        _memTracker->set(0);
    }

protected:
    virtual void updateWindow(const Value& input) = 0;
    virtual void resetWindow() = 0;
    virtual Value getWindowValue(boost::optional<Document> current) = 0;
    virtual int64_t getMemUsage() = 0;

    boost::intrusive_ptr<Expression> _input;
    WindowBounds::Bound<int> _upperDocumentBound;

private:
    // In one of two states: either the initial window has not been populated or we are sliding and
    // accumulating a single value per iteration.
    bool _initialized = false;

    void initialize() {
        auto needMore = [&](int index) {
            return visit(OverloadedVisitor{
                             [&](const WindowBounds::Unbounded&) { return true; },
                             [&](const WindowBounds::Current&) { return index == 0; },
                             [&](const int& n) { return index <= n; },
                         },
                         _upperDocumentBound);
        };

        _initialized = true;
        for (int i = 0; needMore(i); i++) {
            if (auto doc = (this->_iter)[i]) {
                updateWindow(_input->evaluate(*doc, &_input->getExpressionContext()->variables));
                _memTracker->set(getMemUsage());
            } else {
                // Already reached the end of partition for the first value to compute.
                break;
            }
        }
    }
};

}  // namespace mongo
