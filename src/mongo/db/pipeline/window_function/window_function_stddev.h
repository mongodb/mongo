// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/window_function/window_function.h"
#include "mongo/util/modules.h"

namespace mongo {

class WindowFunctionStdDev : public WindowFunctionState {
protected:
    explicit WindowFunctionStdDev(ExpressionContext* const expCtx, bool isSamp)
        : WindowFunctionState(expCtx),
          _sum(make_intrusive<AccumulatorSum>(expCtx)),
          _m2(make_intrusive<AccumulatorSum>(expCtx)),
          _isSamp(isSamp),
          _count(0),
          _nonfiniteValueCount(0) {
        _memUsageTracker.set(sizeof(*this));
    }

public:
    static Value getDefault() {
        return Value(BSONNULL);
    }

    void add(Value value) override {
        update(std::move(value), +1);
    }

    void remove(Value value) override {
        update(std::move(value), -1);
    }

    Value getValue(boost::optional<Value> current = boost::none) const final {
        if (_nonfiniteValueCount > 0)
            return Value(BSONNULL);
        const long long adjustedCount = _isSamp ? _count - 1 : _count;
        if (adjustedCount <= 0)
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

    void reset() override {
        _m2->reset();
        _sum->reset();
        _memUsageTracker.set(sizeof(*this));
        _count = 0;
        _nonfiniteValueCount = 0;
    }

private:
    void update(Value value, int quantity) {
        // quantity should be 1 if adding value, -1 if removing value
        if (!value.numeric())
            return;
        if ((value.getType() == BSONType::numberDouble && !std::isfinite(value.getDouble())) ||
            (value.getType() == BSONType::numberDecimal && !value.getDecimal().isFinite())) {
            _nonfiniteValueCount += quantity;
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
        _memUsageTracker.set(sizeof(*this) + _sum->getMemUsage() + _m2->getMemUsage());
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
