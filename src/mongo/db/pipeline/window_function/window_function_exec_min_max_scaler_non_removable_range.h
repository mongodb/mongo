// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/expression/evaluate.h"
#include "mongo/db/pipeline/window_function/window_function_exec_non_removable_range_common.h"
#include "mongo/db/pipeline/window_function/window_function_min_max_scaler_util.h"
#include "mongo/util/modules.h"

namespace mongo {

/**
 * An executor specifically for the implementation of $minMaxScaler
 * for range based WindowBounds that do not remove documents from the window (left unbounded).
 */
class WindowFunctionExecMinMaxScalerNonRemovableRange final
    : public WindowFunctionExecNonRemovableRangeCommon {
public:
    WindowFunctionExecMinMaxScalerNonRemovableRange(
        PartitionIterator* iter,
        boost::intrusive_ptr<Expression> input,
        boost::intrusive_ptr<ExpressionFieldPath> sortExpr,
        WindowBounds bounds,
        SimpleMemoryUsageTracker* memTracker,
        std::pair<Value, Value> sMinAndsMax)
        : WindowFunctionExecNonRemovableRangeCommon(iter, input, sortExpr, bounds, memTracker),
          _sMinAndsMax(sMinAndsMax.first, sMinAndsMax.second) {}

    void updateWindow(const Value& input) final {
        uassert(10487004,
                str::stream()
                    << "'input' argument to $minMaxScaler must evaluate to a numeric type, got: "
                    << typeName(input.getType()),
                input.numeric());
        _windowMinAndMax.update(input);
    }

    void resetWindow() final {
        _windowMinAndMax.reset();
    }

    Value getWindowValue(boost::optional<Document> current) final {
        tassert(10487005,
                "$minMaxScaler window function must be provided with the value of the current "
                "document",
                current.has_value());

        const Value& inputValue =
            _input->evaluate(*current, &_input->getExpressionContext()->variables);
        // This is a tassert, not a uassert, because the same value should always already be
        // present in the current window, where it would have been caught as a non-numeric.
        tassert(10487006,
                str::stream()
                    << "'input' argument to $minMaxScaler must evaluate to a numeric type, got: "
                    << typeName(inputValue.getType()),
                inputValue.numeric());

        return min_max_scaler::computeResult(inputValue, _windowMinAndMax, _sMinAndsMax);
    }

    int64_t getMemUsage() final {
        // The only memory used, regardless of window bound size, is the memory used by the two
        // MinAndMax structs (one for the arguments, and one for the current values). Initialization
        // state also does not affect memory usage, because MinAndMax is fully default constructed.
        // Therefore, the memory usage of the MinMaxScaler non-removable range function is constant.
        return kWindowFunctionExecMinMaxScalerNonRemovableRangeMemoryUsageBytes;
    }

private:
    // Declare memory usage as a static constexpr since it is a constant. The compiler may be able
    // to make small optimizations for more performant code. However, it is likely that saving
    // repetitive multiplications here will not impact the performance of $minMaxScaler in a
    // noticeable way.
    static constexpr int64_t kWindowFunctionExecMinMaxScalerNonRemovableRangeMemoryUsageBytes =
        sizeof(min_max_scaler::MinAndMax) * 2;

    // Output domain Value is bounded between sMin and sMax (inclusive).
    // These are provided as arguments to $minMaxScaler, and do not change.
    const min_max_scaler::MinAndMax _sMinAndsMax;

    // The min and max Values of the window seen so far.
    min_max_scaler::MinAndMax _windowMinAndMax;
};

}  // namespace mongo
