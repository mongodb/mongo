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
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/window_function/window_function_sum.h"

namespace mongo {

class WindowFunctionAvg final : public RemovableSum {
public:
    explicit WindowFunctionAvg(ExpressionContext* const expCtx) : RemovableSum(expCtx), _count(0) {
        // Note that RemovableSum manages the memory usage tracker directly for calls to add/remove.
        // Here we only add the members that this class holds.
        _memUsageBytes += sizeof(long long);
    }

    static std::unique_ptr<WindowFunctionState> create(ExpressionContext* const expCtx) {
        return std::make_unique<WindowFunctionAvg>(expCtx);
    }

    static Value getDefault() {
        return Value(BSONNULL);
    }
    void add(Value value) final {
        if (!value.numeric())
            return;
        RemovableSum::add(std::move(value));
        _count++;
    }

    void remove(Value value) final {
        if (!value.numeric())
            return;
        RemovableSum::remove(std::move(value));
        _count--;
    }

    Value getValue() const final {
        if (_count == 0) {
            return getDefault();
        }
        Value sum = RemovableSum::getValue();
        switch (sum.getType()) {
            case NumberInt:
            case NumberLong:
                return Value(sum.coerceToDouble() / static_cast<double>(_count));
            case NumberDouble: {
                double internalSum = sum.getDouble();
                if (std::isnan(internalSum) || std::isinf(internalSum)) {
                    return sum;
                }
                return Value(internalSum / static_cast<double>(_count));
            }
            case NumberDecimal: {
                Decimal128 internalSum = sum.getDecimal();
                if (internalSum.isNaN() || internalSum.isInfinite()) {
                    return sum;
                }
                return Value(internalSum.divide(Decimal128(_count)));
            }
            default:
                MONGO_UNREACHABLE_TASSERT(5371301);
        }
    }

    void reset() {
        RemovableSum::reset();
        _count = 0;
        _memUsageBytes += sizeof(long long);
    }

private:
    long long _count;
};

}  // namespace mongo
