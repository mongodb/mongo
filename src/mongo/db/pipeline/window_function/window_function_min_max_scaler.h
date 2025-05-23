/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/window_function/window_function.h"
#include "mongo/db/pipeline/window_function/window_function_min_max.h"
#include "mongo/db/pipeline/window_function/window_function_min_max_scaler_util.h"
#include "mongo/util/assert_util.h"

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
