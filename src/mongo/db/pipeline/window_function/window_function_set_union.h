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

#include "mongo/db/pipeline/memory_token_container_util.h"
#include "mongo/db/pipeline/window_function/window_function.h"

namespace mongo {

class WindowFunctionSetUnion final : public WindowFunctionState {
public:
    static inline const Value kDefault = Value{std::vector<Value>()};

    static std::unique_ptr<WindowFunctionState> create(ExpressionContext* expCtx) {
        return std::make_unique<WindowFunctionSetUnion>(expCtx);
    }

    explicit WindowFunctionSetUnion(ExpressionContext* const expCtx)
        : WindowFunctionState(expCtx, internalQueryMaxSetUnionBytes.load()),
          _values(MemoryTokenValueComparator(&_expCtx->getValueComparator())) {
        _memUsageTracker.set(sizeof(*this));
    }

    void add(Value value) override {
        if (value.missing()) {
            return;
        }

        uassert(ErrorCodes::TypeMismatch,
                str::stream() << "$setUnion requires array inputs, but input "
                              << redact(value.toString()) << " is of type "
                              << typeName(value.getType()),
                value.isArray());

        for (const auto& val : value.getArray()) {
            _values.emplace(SimpleMemoryUsageToken{val.getApproximateSize(), &_memUsageTracker},
                            val);
            uassert(ErrorCodes::ExceededMemoryLimit,
                    str::stream() << "$setUnion used too much memory and spilling to disk will not "
                                     "reduce memory usage. Used: "
                                  << _memUsageTracker.inUseTrackedMemoryBytes()
                                  << "bytes. Memory limit: "
                                  << _memUsageTracker.maxAllowedMemoryUsageBytes() << " bytes",
                    _memUsageTracker.withinMemoryLimit());
        }
    }

    void remove(Value value) override {
        if (value.missing()) {
            return;
        }

        tassert(1628403, "Can only remove an array from WindowFunctionSetUnion", value.isArray());

        auto numValuesToRemove = value.getArrayLength();

        tassert(1628404,
                "Can't remove more values than the number contained in WindowFunctionSetUnion",
                _values.size() >= numValuesToRemove);

        for (const auto& valToRemove : value.getArray()) {
            auto iter = _values.find(valToRemove);
            tassert(1628405,
                    "Can't remove a value that is not contained in the WindowFunctionSetUnion",
                    iter != _values.end());
            _values.erase(iter);
        }
    }

    void reset() override {
        _values.clear();
        _memUsageTracker.set(sizeof(*this));
    }

    Value getValue(boost::optional<Value> current = boost::none) const override {
        std::vector<Value> output;
        if (_values.empty()) {
            return kDefault;
        }
        for (auto it = _values.begin(); it != _values.end(); it = _values.upper_bound(*it)) {
            output.push_back(it->value());
        }

        return Value(std::move(output));
    }

private:
    std::multiset<SimpleMemoryUsageTokenWith<Value>, MemoryTokenValueComparator> _values;
};

}  // namespace mongo
