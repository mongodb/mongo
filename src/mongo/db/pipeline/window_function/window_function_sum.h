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
#include "mongo/db/pipeline/window_function/window_function.h"

namespace mongo {

class RemovableSum : public WindowFunctionState {
protected:
    explicit RemovableSum(ExpressionContext* const expCtx)
        : WindowFunctionState(expCtx),
          _sumAcc(AccumulatorSum::create(expCtx)),
          _posInfiniteValueCount(0),
          _negInfiniteValueCount(0),
          _nanCount(0),
          _doubleCount(0),
          _decimalCount(0) {
        _memUsageBytes = sizeof(*this) + _sumAcc->getMemUsage();
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

    Value getValue() const override;

    void reset() {
        _sumAcc->reset();
        _posInfiniteValueCount = 0;
        _negInfiniteValueCount = 0;
        _nanCount = 0;
        _doubleCount = 0;
        _decimalCount = 0;
        _memUsageBytes = sizeof(*this) + _sumAcc->getMemUsage();
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
            case NumberInt:
                accountForIntegral(value.getInt(), quantity);
                break;
            case NumberLong:
                accountForIntegral(value.getLong(), quantity);
                break;
            case NumberDouble:
                _doubleCount += quantity;
                accountForDouble(value.getDouble(), quantity);
                break;
            case NumberDecimal:
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
