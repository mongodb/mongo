// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/window_function/window_function.h"
#include "mongo/platform/decimal128.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/modules.h"

#include <cmath>
#include <limits>
#include <memory>
#include <utility>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

class RemovableSum : public WindowFunctionState {
protected:
    explicit RemovableSum(ExpressionContext* const expCtx)
        : WindowFunctionState(expCtx),
          _sumAcc(make_intrusive<AccumulatorSum>(expCtx)),
          _posInfiniteValueCount(0),
          _negInfiniteValueCount(0),
          _nanCount(0),
          _doubleCount(0),
          _decimalCount(0) {
        _memUsageTracker.set(sizeof(*this) + _sumAcc->getMemUsage());
    }

public:
    static Value getDefault() {
        return Value{0};
    }

    void add(Value value) override {
        update(std::move(value), 1);
    }

    void remove(Value value) override {
        update(std::move(value), -1);
    }

    Value getValue(boost::optional<Value> current = boost::none) const override;

    void reset() override {
        _sumAcc->reset();
        _posInfiniteValueCount = 0;
        _negInfiniteValueCount = 0;
        _nanCount = 0;
        _doubleCount = 0;
        _decimalCount = 0;
        _memUsageTracker.set(sizeof(*this) + _sumAcc->getMemUsage());
    }

private:
    boost::intrusive_ptr<AccumulatorState> _sumAcc;
    int _posInfiniteValueCount;
    int _negInfiniteValueCount;
    int _nanCount;
    long long _doubleCount;
    long long _decimalCount;

    template <class T>
    void accountForIntegral(T value, int quantity) {
        if (value == std::numeric_limits<T>::min() && quantity == -1) {
            // Avoid overflow by processing in two parts.
            _sumAcc->process(Value(std::numeric_limits<T>::max()), false);
            _sumAcc->process(Value(1), false);
        } else {
            _sumAcc->process(Value(value * quantity), false);
        }
    }

    void accountForDouble(double value, int quantity) {
        // quantity should be 1 if adding value, -1 if removing value
        if (std::isnan(value)) {
            _nanCount += quantity;
        } else if (value == std::numeric_limits<double>::infinity()) {
            _posInfiniteValueCount += quantity;
        } else if (value == -std::numeric_limits<double>::infinity()) {
            _negInfiniteValueCount += quantity;
        } else {
            _sumAcc->process(Value(value * quantity), false);
        }
    }

    void accountForDecimal(Decimal128 value, int quantity) {
        // quantity should be 1 if adding value, -1 if removing value
        if (value.isNaN()) {
            _nanCount += quantity;
        } else if (value.isInfinite() && !value.isNegative()) {
            _posInfiniteValueCount += quantity;
        } else if (value.isInfinite() && value.isNegative()) {
            _negInfiniteValueCount += quantity;
        } else {
            if (quantity == -1) {
                value = value.negate();
            }
            _sumAcc->process(Value(value), false);
        }
    }

    void update(Value value, int quantity) {
        // quantity should be 1 if adding value, -1 if removing value
        if (!value.numeric())
            return;
        switch (value.getType()) {
            case BSONType::numberInt:
                accountForIntegral(value.getInt(), quantity);
                break;
            case BSONType::numberLong:
                accountForIntegral(value.getLong(), quantity);
                break;
            case BSONType::numberDouble:
                _doubleCount += quantity;
                accountForDouble(value.getDouble(), quantity);
                break;
            case BSONType::numberDecimal:
                _decimalCount += quantity;
                accountForDecimal(value.getDecimal(), quantity);
                break;
            default:
                MONGO_UNREACHABLE_TASSERT(5371300);
        }
    }
};

class WindowFunctionSum final : public RemovableSum {
public:
    explicit WindowFunctionSum(ExpressionContext* const expCtx) : RemovableSum(expCtx) {}

    static std::unique_ptr<WindowFunctionState> create(ExpressionContext* const expCtx) {
        return std::make_unique<WindowFunctionSum>(expCtx);
    }
};

}  // namespace mongo
