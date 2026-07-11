// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/window_function/window_function.h"
#include "mongo/db/pipeline/window_function/window_function_min_max.h"
#include "mongo/db/pipeline/window_function/window_function_min_max_scaler_util.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

namespace mongo {

/**
 * Implementation of a WindowFunctionState for computing the value of $minMaxScaler
 * for removable windows (for both document and range based bounds).
 * Builds off of WindowFunctionMinMaxCommon, which tracks the min and max in the window.
 */
class WindowFunctionMinMaxScaler : public WindowFunctionMinMaxCommon {
public:
    using WindowFunctionMinMaxCommon::_memUsageTracker;
    using WindowFunctionMinMaxCommon::_values;

    static std::unique_ptr<WindowFunctionState> create(ExpressionContext* const expCtx,
                                                       std::pair<Value, Value> sMinAndsMax = {
                                                           Value(0), Value(1)}) {
        return std::make_unique<WindowFunctionMinMaxScaler>(expCtx, sMinAndsMax);
    }

    explicit WindowFunctionMinMaxScaler(ExpressionContext* const expCtx,
                                        std::pair<Value, Value> sMinAndsMax = {Value(0), Value(1)})
        : WindowFunctionMinMaxCommon(expCtx), _sMinAndsMax(sMinAndsMax.first, sMinAndsMax.second) {
        _memUsageTracker.set(sizeof(*this));
    }

    Value getValue(boost::optional<Value> current) const final {
        // Value of the current document is needed to compute the $minMaxScaler.
        tassert(9459901,
                "$minMaxScaler window function must be provided with the value of the current "
                "document",
                current.has_value());
        uassert(10487000,
                str::stream()
                    << "'input' argument to $minMaxScaler must evaluate to a numeric type, got: "
                    << typeName(current->getType()),
                current->numeric());
        tassert(9459902,
                "There must always be documents in the current window for $minMaxScaler",
                !_values.empty());

        // First value in ordered set is the min of the window.
        // Last value in the ordered set is the max of the window.
        const min_max_scaler::MinAndMax windowMinAndMax(_values.begin()->value(),
                                                        _values.rbegin()->value());

        return min_max_scaler::computeResult(*current, windowMinAndMax, _sMinAndsMax);
    }

private:
    // Output domain Value is bounded between sMin and sMax (inclusive).
    // These are provided as arguments to $minMaxScaler, and do not change.
    const min_max_scaler::MinAndMax _sMinAndsMax;
};

};  // namespace mongo
