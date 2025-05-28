/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/exec/expression/evaluate.h"
#include "mongo/db/pipeline/window_function/window_function_exec_non_removable_common.h"
#include "mongo/db/pipeline/window_function/window_function_min_max_scaler_util.h"

namespace mongo {

/**
 * An executor specifically for the implementation of $minMaxScaler
 * for document based WindowBounds that do not remove documents from the window (left unbounded).
 */
class WindowFunctionExecMinMaxScalerNonRemovable final
    : public WindowFunctionExecNonRemovableCommon {
public:
    WindowFunctionExecMinMaxScalerNonRemovable(PartitionIterator* iter,
                                               boost::intrusive_ptr<Expression> input,
                                               WindowBounds::Bound<int> upperDocumentBound,
                                               SimpleMemoryUsageTracker* memTracker,
                                               std::pair<Value, Value> sMinAndsMax)
        : WindowFunctionExecNonRemovableCommon(iter, input, upperDocumentBound, memTracker),
          _sMinAndsMax(sMinAndsMax.first, sMinAndsMax.second) {};

    void updateWindow(const Value& input) final {
        uassert(10487001,
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
        tassert(10487002,
                "$minMaxScaler window function must be provided with the value of the current "
                "document",
                current.has_value());

        const Value& inputValue =
            _input->evaluate(*current, &_input->getExpressionContext()->variables);
        // This is a tassert, not a uassert, because the same value should always already be
        // present in the current window, where it would have been caught as a non-numeric.
        tassert(10487003,
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
        // Therefore, the memory usage of the MinMaxScaler non-removable function is constant.
        return kWindowFunctionExecMinMaxScalerNonRemovableMemoryUsageBytes;
    }

private:
    // Declare memory usage as a static constexpr since it is a constant. The compiler may be able
    // to make small optimizations for more performant code. However, it is likely that saving
    // repetitive multiplications here will not impact the performance of $minMaxScaler in a
    // noticeable way.
    static constexpr int64_t kWindowFunctionExecMinMaxScalerNonRemovableMemoryUsageBytes =
        sizeof(min_max_scaler::MinAndMax) * 2;

    // Output domain Value is bounded between sMin and sMax (inclusive).
    // These are provided as arguments to $minMaxScaler, and do not change.
    const min_max_scaler::MinAndMax _sMinAndsMax;

    // The min and max Values of the window seen so far.
    min_max_scaler::MinAndMax _windowMinAndMax;
};

}  // namespace mongo
