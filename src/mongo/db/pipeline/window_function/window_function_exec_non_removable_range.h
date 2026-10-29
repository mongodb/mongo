// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/window_function/window_function_exec_non_removable_range_common.h"
#include "mongo/util/modules.h"

namespace mongo {

/**
 * An executor that handles left-unbounded, range-based windows.
 *
 * Uses a generic accumulator type (AccumulatorState), provided as a construction argument,
 * to determine how the window is updated, and reset; and how to get the current window value,
 * and fetch the current mem usage.
 */
class WindowFunctionExecNonRemovableRange final : public WindowFunctionExecNonRemovableRangeCommon {
public:
    WindowFunctionExecNonRemovableRange(PartitionIterator* iter,
                                        boost::intrusive_ptr<Expression> input,
                                        boost::intrusive_ptr<ExpressionFieldPath> sortExpr,
                                        boost::intrusive_ptr<AccumulatorState> function,
                                        WindowBounds bounds,
                                        SimpleMemoryUsageTracker* memTracker)
        : WindowFunctionExecNonRemovableRangeCommon(iter, input, sortExpr, bounds, memTracker),
          _function(std::move(function)) {}

    void updateWindow(const Value& input) final {
        _function->process(input, false);
    }

    void resetWindow() final {
        _function->reset();
    }

    Value getWindowValue(boost::optional<Document> current) final {
        return _function->getValue(false);
    }

    int64_t getMemUsage() final {
        return _function->getMemUsage();
    }

private:
    boost::intrusive_ptr<AccumulatorState> _function;
};
}  // namespace mongo
