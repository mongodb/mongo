// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/pipeline/accumulator_multi.h"
#include "mongo/db/pipeline/window_function/window_function.h"
#include "mongo/util/modules.h"

namespace mongo {
using FirstLastSense = AccumulatorFirstLastN::Sense;

template <FirstLastSense S>
class WindowFunctionFirstLastN : public WindowFunctionState {
public:
    static std::unique_ptr<WindowFunctionState> create(ExpressionContext* const expCtx,
                                                       long long n) {
        return std::make_unique<WindowFunctionFirstLastN<S>>(expCtx, n);
    }

    static AccumulationExpression parse(ExpressionContext* const expCtx,
                                        BSONElement elem,
                                        VariablesParseState vps) {
        return AccumulatorFirstLastN::parseFirstLastN<S>(expCtx, elem, vps);
    }

    static const char* getName() {
        if constexpr (S == FirstLastSense::kFirst) {
            return AccumulatorFirstN::getName();
        } else {
            return AccumulatorLastN::getName();
        }
    }

    WindowFunctionFirstLastN(ExpressionContext* const expCtx, long long n)
        : WindowFunctionState(expCtx), _n(n) {
        _memUsageTracker.set(sizeof(*this));
    }

    void add(Value value) final {
        auto valToInsert = value.missing() ? Value(BSONNULL) : std::move(value);
        _values.emplace_back(
            SimpleMemoryUsageToken{valToInsert.getApproximateSize(), &_memUsageTracker},
            std::move(valToInsert));
    }

    void remove(Value value) final {
        auto valToRemove = value.missing() ? Value(BSONNULL) : value;
        tassert(5788400, "Can't remove from an empty WindowFunctionFirstLastN", !_values.empty());
        auto iter = _values.begin();
        tassert(5788402,
                str::stream() << "Attempted to remove an element other than the first element from "
                                 "window function "
                              << getName(),
                _expCtx->getValueComparator().compare(iter->value(), valToRemove) == 0);
        _values.erase(iter);
    }

    Value getValue(boost::optional<Value> current = boost::none) const final {
        if (_values.empty()) {
            return Value(std::vector<Value>{});
        }
        auto n = static_cast<size_t>(_n);

        if (n >= _values.size()) {
            return convertToValueFromMemoryTokenWithValue(
                _values.begin(), _values.end(), _values.size());
        }

        if constexpr (S == FirstLastSense::kFirst) {
            return convertToValueFromMemoryTokenWithValue(_values.begin(), _values.begin() + n, n);
        } else {
            return convertToValueFromMemoryTokenWithValue(_values.end() - n, _values.end(), n);
        }
    }

    void reset() final {
        _values.clear();
        _memUsageTracker.set(sizeof(*this));
    }

private:
    std::vector<SimpleMemoryUsageTokenWith<Value>> _values;
    long long _n;
};
using WindowFunctionFirstN = WindowFunctionFirstLastN<FirstLastSense::kFirst>;
using WindowFunctionLastN = WindowFunctionFirstLastN<FirstLastSense::kLast>;
};  // namespace mongo
