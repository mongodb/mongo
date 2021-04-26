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
#include "mongo/db/pipeline/window_function/window_function.h"

namespace mongo {

class WindowFunctionStdDev : public WindowFunctionState {
protected:
    explicit WindowFunctionStdDev(ExpressionContext* const expCtx, bool isSamp)
        : WindowFunctionState(expCtx),
          _sum(AccumulatorSum::create(expCtx)),
          _m2(AccumulatorSum::create(expCtx)),
          _isSamp(isSamp),
          _count(0),
          _nonfiniteValueCount(0) {
        _memUsageBytes = sizeof(*this);
    }

public:
    static Value getDefault() {
        return Value(BSONNULL);
    }

    void add(Value value) {
        update(std::move(value), +1);
    }

    void remove(Value value) {
        update(std::move(value), -1);
    }

    Value getValue() const final {
        if (_nonfiniteValueCount > 0)
            return Value(BSONNULL);
        const long long adjustedCount = _isSamp ? _count - 1 : _count;
        if (adjustedCount == 0)
            return getDefault();
        double squaredDifferences = _m2->getValue(false).coerceToDouble();
        if (squaredDifferences < 0 || (!_isSamp && _count == 1)) {
            // _m2 is the sum of squared differences from the mean, so it should always be
            // nonnegative. It may take on a small negative value due to floating point error, which
            // breaks the sqrt calculation. In this case, the closest valid value for _m2 is 0, so
            // we reset _m2 and return 0 for the standard deviation.
            // If we're doing a population std dev of one element, it is also correct to return 0.
            _m2->reset();
            return Value{0};
        }
        return Value(sqrt(_m2->getValue(false).coerceToDouble() / adjustedCount));
    }

    void reset() {
        _m2->reset();
        _sum->reset();
        _memUsageBytes = sizeof(*this);
        _count = 0;
        _nonfiniteValueCount = 0;
    }

private:
    void update(Value value, int quantity) {
        // quantity should be 1 if adding value, -1 if removing value
        if (!value.numeric())
            return;
        if ((value.getType() == NumberDouble && !std::isfinite(value.getDouble())) ||
            (value.getType() == NumberDecimal && !value.getDecimal().isFinite())) {
            _nonfiniteValueCount += quantity;
            _count += quantity;
            return;
        }

        if (_count == 0) {  // Assuming we are adding value if _count == 0.
            _count++;
            _sum->process(value, false);
            return;
        } else if (_count + quantity == 0) {  // Empty the window.
            reset();
            return;
        }
        double x = _count * value.coerceToDouble() - _sum->getValue(false).coerceToDouble();
        _count += quantity;
        _sum->process(Value{value.coerceToDouble() * quantity}, false);
        _m2->process(Value{x * x * quantity / (_count * (_count - quantity))}, false);
        _memUsageBytes = sizeof(*this) + _sum->getMemUsage() + _m2->getMemUsage();
    }

    // Std dev cannot make use of RemovableSum because of its specific handling of non-finite
    // values. Adding a NaN or +/-inf makes the result NaN. Additionally, the consistent and
    // exclusive use of doubles in std dev calculations makes the type handling in RemovableSum
    // unnecessary.
    boost::intrusive_ptr<AccumulatorState> _sum;
    boost::intrusive_ptr<AccumulatorState> _m2;
    bool _isSamp;
    long long _count;
    int _nonfiniteValueCount;
};

class WindowFunctionStdDevPop final : public WindowFunctionStdDev {
public:
    static std::unique_ptr<WindowFunctionState> create(ExpressionContext* const expCtx) {
        return std::make_unique<WindowFunctionStdDevPop>(expCtx);
    }
    explicit WindowFunctionStdDevPop(ExpressionContext* const expCtx)
        : WindowFunctionStdDev(expCtx, false) {}
};

class WindowFunctionStdDevSamp final : public WindowFunctionStdDev {
public:
    static std::unique_ptr<WindowFunctionState> create(ExpressionContext* const expCtx) {
        return std::make_unique<WindowFunctionStdDevSamp>(expCtx);
    }
    explicit WindowFunctionStdDevSamp(ExpressionContext* const expCtx)
        : WindowFunctionStdDev(expCtx, true) {}
};
}  // namespace mongo
