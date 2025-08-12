/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/window_function/window_function.h"

namespace mongo {

class WindowFunctionConcatArrays final : public WindowFunctionState {
public:
    static inline const Value kDefaultEmptyArray = Value{std::vector<Value>()};

    static std::unique_ptr<WindowFunctionState> create(ExpressionContext* const expCtx) {
        return std::make_unique<WindowFunctionConcatArrays>(expCtx);
    }

    explicit WindowFunctionConcatArrays(ExpressionContext* const expCtx)
        : WindowFunctionState(expCtx, internalQueryMaxConcatArraysBytes.load()) {
        _memUsageTracker.set(sizeof(*this));
    }

    void add(Value value) override {
        if (value.missing()) {
            return;
        }

        uassert(ErrorCodes::TypeMismatch,
                str::stream() << "$concatArrays requires array inputs, but input "
                              << redact(value.toString()) << " is of type "
                              << typeName(value.getType()),
                value.isArray());

        _count += value.getArrayLength();
        _values.emplace_back(SimpleMemoryUsageToken{value.getApproximateSize(), &_memUsageTracker},
                             std::move(value));
        uassert(ErrorCodes::ExceededMemoryLimit,
                str::stream() << "$concatArrays used too much memory and spilling to disk will not "
                                 "reduce memory usage. Used: "
                              << _memUsageTracker.inUseTrackedMemoryBytes()
                              << " bytes. Memory limit: "
                              << _memUsageTracker.maxAllowedMemoryUsageBytes() << " bytes",
                _memUsageTracker.withinMemoryLimit());
    }

    void remove(Value value) override {
        if (value.missing()) {
            return;
        }

        tassert(
            1628400, "Can only remove an array from WindowFunctionConcatArrays", value.isArray());

        tassert(
            1628401, "Can't remove from an empty WindowFunctionConcatArrays", _values.size() > 0);

        // Assert that we are removing the element at the front of the WindowFunctionConcatArrays
        // (i.e. that removals are occurring in FIFO order). This check is expensive on workloads
        // with a lot of removals, an becomes even more expensive with arbitrarily long arrays.
        dassert(_expCtx->getValueComparator().getEqualTo()(_values.front().value(), value));

        _count -= value.getArrayLength();
        _values.pop_front();
    }

    void reset() override {
        _values.clear();
        _memUsageTracker.set(sizeof(*this));
        _count = 0;
    }

    Value getValue(boost::optional<Value> current = boost::none) const override {
        if (_values.empty()) {
            return kDefaultEmptyArray;
        }

        std::vector<Value> result;
        result.reserve(_count);

        for (const auto& array : _values) {
            for (const auto& val : array.value().getArray()) {
                result.emplace_back(val);
            }
        }

        return Value{std::move(result)};
    }

private:
    // Every Value in _values will be an array. This will be unrolled in getValue() to produce a
    // single array that is the concatenation of all the arrays in _values.
    std::deque<SimpleMemoryUsageTokenWith<Value>> _values;

    // The number of Values that will be present in the array produced by getValue().
    long long _count = 0;
};

}  // namespace mongo
