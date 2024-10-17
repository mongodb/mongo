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

#include "mongo/db/pipeline/accumulator_multi.h"
#include "mongo/db/pipeline/window_function/window_function.h"

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
