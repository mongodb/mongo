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

template <AccumulatorMinMax::Sense sense>
class WindowFunctionMinMaxCommon : public WindowFunctionState {
public:
    void add(Value value) final {
        // Ignore nullish values.
        if (value.nullish())
            return;
        _memUsageBytes += value.getApproximateSize();
        _values.insert(std::move(value));
    }

    void remove(Value value) final {
        // Ignore nullish values.
        if (value.nullish())
            return;
        // std::multiset::insert is guaranteed to put the element after any equal elements
        // already in the container. So find() / erase() will remove the oldest equal element,
        // which is what we want, to satisfy "remove() undoes add() when called in FIFO order".
        auto iter = _values.find(value);
        tassert(5371400, "Can't remove from an empty WindowFunctionMinMax", iter != _values.end());
        _memUsageBytes -= iter->getApproximateSize();
        _values.erase(iter);
    }

    void reset() final {
        _values.clear();
        _memUsageBytes = sizeof(*this);
    }

protected:
    // Constructor hidden so that only instances of the derived types can be created.
    explicit WindowFunctionMinMaxCommon(ExpressionContext* const expCtx)
        : WindowFunctionState(expCtx),
          _values(_expCtx->getValueComparator().makeOrderedValueMultiset()) {}

    // Holds all the values in the window, in order, with constant-time access to both ends.
    ValueMultiset _values;
};

template <AccumulatorMinMax::Sense sense>
class WindowFunctionMinMax : public WindowFunctionMinMaxCommon<sense> {
public:
    using WindowFunctionMinMaxCommon<sense>::_values;
    using WindowFunctionMinMaxCommon<sense>::_memUsageBytes;

    static inline const Value kDefault = Value{BSONNULL};

    static std::unique_ptr<WindowFunctionState> create(ExpressionContext* const expCtx) {
        return std::make_unique<WindowFunctionMinMax<sense>>(expCtx);
    }

    explicit WindowFunctionMinMax(ExpressionContext* const expCtx)
        : WindowFunctionMinMaxCommon<sense>(expCtx) {
        _memUsageBytes = sizeof(*this);
    }

    Value getValue() const final {
        if (_values.empty())
            return kDefault;
        if constexpr (sense == AccumulatorMinMax::Sense::kMin) {
            return *_values.begin();
        } else {
            return *_values.rbegin();
        }
        MONGO_UNREACHABLE_TASSERT(5371401);
    }
};

template <AccumulatorMinMax::Sense sense>
class WindowFunctionMinMaxN : public WindowFunctionMinMaxCommon<sense> {
public:
    using WindowFunctionMinMaxCommon<sense>::_values;
    using WindowFunctionMinMaxCommon<sense>::_memUsageBytes;

    static std::unique_ptr<WindowFunctionState> create(ExpressionContext* const expCtx,
                                                       long long n) {
        return std::make_unique<WindowFunctionMinMaxN<sense>>(expCtx, n);
    }
    explicit WindowFunctionMinMaxN(ExpressionContext* const expCtx, long long n)
        : WindowFunctionMinMaxCommon<sense>(expCtx), _n(n) {
        _memUsageBytes = sizeof(*this);
    }

    Value getValue() const final {
        if (_values.empty()) {
            return Value(std::vector<Value>());
        }

        auto processVal = [&](auto begin, auto end, size_t size) -> Value {
            auto n = static_cast<size_t>(_n);

            // If 'n' is greater than the size of the current window, then return all the values.
            if (n >= size) {
                return Value(std::vector(begin, end));
            } else {
                std::vector<Value> result;
                result.reserve(n);
                auto it = begin;
                for (size_t i = 0; i < n; ++i, ++it) {
                    result.push_back(*it);
                }
                return Value(std::move(result));
            }
        };

        auto size = _values.size();
        if constexpr (sense == AccumulatorMinMax::Sense::kMin) {
            return processVal(_values.begin(), _values.end(), size);
        } else {
            return processVal(_values.rbegin(), _values.rend(), size);
        }
    }


private:
    long long _n;
};
using WindowFunctionMin = WindowFunctionMinMax<AccumulatorMinMax::Sense::kMin>;
using WindowFunctionMax = WindowFunctionMinMax<AccumulatorMinMax::Sense::kMax>;
using WindowFunctionMinN = WindowFunctionMinMaxN<AccumulatorMinMax::Sense::kMin>;
using WindowFunctionMaxN = WindowFunctionMinMaxN<AccumulatorMinMax::Sense::kMax>;

}  // namespace mongo
