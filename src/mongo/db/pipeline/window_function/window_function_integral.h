// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonmisc.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/window_function/window_function.h"
#include "mongo/db/pipeline/window_function/window_function_sum.h"
#include "mongo/util/modules.h"

#include <deque>
#include <memory>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

class WindowFunctionIntegral final : public WindowFunctionState {
public:
    static inline const Value kDefault = Value{BSONNULL};

    static std::unique_ptr<WindowFunctionState> create(
        ExpressionContext* const expCtx, boost::optional<long long> unitMillis = boost::none) {
        return std::make_unique<WindowFunctionIntegral>(expCtx, unitMillis);
    }

    explicit WindowFunctionIntegral(ExpressionContext* const expCtx,
                                    boost::optional<long long> unitMillis = boost::none,
                                    bool isNonremovable = false)
        : WindowFunctionState(expCtx), _integral(expCtx), _unitMillis(unitMillis) {
        _memUsageTracker.set(sizeof(*this));
    }

    void add(Value value) override;

    /**
     * This should only remove the first/lowest element in the window.
     */
    void remove(Value value) override;

    void reset() override {
        _values.clear();
        _nanCount = 0;
        _integral.reset();
        // AccumulatorIntegral's reset() depends on the fact that WindowFunctionIntegral's reset()
        // will set '_memUsageTracker' to sizeof(*this). If you want to reset '_memUsageTracker' to
        // other value, please update AccumulatorIntegral's reset() as well.
        _memUsageTracker.set(sizeof(*this));
    }

    Value getValue(boost::optional<Value> current = boost::none) const override;

private:
    /**
     * Returns the integral of two adjacent points calculated based on the Trapezoidal Rule:
     * https://en.wikipedia.org/wiki/Trapezoidal_rule
     *
     * NaN value input will return 0 instead of NaN meaning no impact on integral.
     */
    Value integralOfTwoPointsByTrapezoidalRule(const Value& preValue, const Value& newValue);

    void assertValueType(const Value& value);

    WindowFunctionSum _integral;
    std::deque<SimpleMemoryUsageTokenWith<Value>> _values;
    boost::optional<long long> _unitMillis;
    int _nanCount = 0;
    bool isNonremovable = false;
};

}  // namespace mongo
