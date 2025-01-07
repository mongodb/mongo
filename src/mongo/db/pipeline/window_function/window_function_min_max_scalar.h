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
#include "mongo/util/assert_util.h"

namespace mongo {

class WindowFunctionMinMaxScalar : public WindowFunctionMinMaxCommon {
public:
    using WindowFunctionMinMaxCommon::_memUsageTracker;
    using WindowFunctionMinMaxCommon::_values;

    static std::unique_ptr<WindowFunctionState> create(ExpressionContext* const expCtx,
                                                       std::pair<Value, Value> sMinAndsMax = {
                                                           Value(0), Value(1)}) {
        return std::make_unique<WindowFunctionMinMaxScalar>(expCtx, sMinAndsMax);
    }

    explicit WindowFunctionMinMaxScalar(ExpressionContext* const expCtx,
                                        std::pair<Value, Value> sMinAndsMax = {Value(0), Value(1)})
        : WindowFunctionMinMaxCommon(expCtx), _sMinAndsMax(sMinAndsMax) {
        _memUsageTracker.set(sizeof(*this));
    }

    // Stateless computation of the output value of $minMaxScalar, provided all needed inputs
    // Output = (((currentValue - windowMin) / (windowMax - windowMin)) * (sMax - sMin)) + sMin
    static Value computeValue(const Value& currentValue,
                              const Value& windowMin,
                              const Value& windowMax,
                              const Value& sMin,
                              const Value& sMax);

    Value getValue(boost::optional<Value> current) const final {
        // Value of the current document is needed to compute the $minMaxScalar.
        tassert(9459901,
                "$minMaxScalar window function must be provided with the value of the current "
                "document",
                current.has_value());
        tassert(9459902,
                "There must always be documents in the current window for $minMaxScalar",
                !_values.empty());

        return computeValue(*current,
                            // First value in ordered set is the min of the window.
                            _values.begin()->value(),
                            // Last value in the ordered set is the max of the window.
                            _values.rbegin()->value(),
                            _sMinAndsMax.first,
                            _sMinAndsMax.second);
    }

private:
    // Output domain Value is bounded between sMin and sMax (inclusive).
    // First value is min, second value is max.
    std::pair<Value, Value> _sMinAndsMax;
};

};  // namespace mongo
