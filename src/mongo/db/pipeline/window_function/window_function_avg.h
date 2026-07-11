// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/window_function/window_function.h"
#include "mongo/db/pipeline/window_function/window_function_sum.h"
#include "mongo/platform/decimal128.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <cmath>
#include <memory>
#include <utility>

namespace mongo {

class WindowFunctionAvg final : public RemovableSum {
public:
    explicit WindowFunctionAvg(ExpressionContext* const expCtx) : RemovableSum(expCtx), _count(0) {
        // Note that RemovableSum manages the memory usage tracker directly for calls to add/remove.
        // Here we only add the members that this class holds.
        _memUsageTracker.add(sizeof(long long));
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

    Value getValue(boost::optional<Value> current = boost::none) const final {
        if (_count == 0) {
            return getDefault();
        }
        Value sum = RemovableSum::getValue(current);
        switch (sum.getType()) {
            case BSONType::numberInt:
            case BSONType::numberLong:
                return Value(sum.coerceToDouble() / static_cast<double>(_count));
            case BSONType::numberDouble: {
                double internalSum = sum.getDouble();
                if (std::isnan(internalSum) || std::isinf(internalSum)) {
                    return sum;
                }
                return Value(internalSum / static_cast<double>(_count));
            }
            case BSONType::numberDecimal: {
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

    void reset() override {
        RemovableSum::reset();
        _count = 0;
        _memUsageTracker.add(sizeof(long long));
    }

private:
    long long _count;
};

}  // namespace mongo
