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

#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/window_function/window_function_exec_non_removable_range_common.h"

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
