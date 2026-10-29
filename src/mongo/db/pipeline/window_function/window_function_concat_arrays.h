// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/window_function/window_function.h"
#include "mongo/db/query/query_knob_descriptors_execution.h"
#include "mongo/util/modules.h"

namespace mongo {

class WindowFunctionConcatArrays final : public WindowFunctionState {
public:
    static constexpr auto kName = "$concatArrays"sv;
    static inline const Value kDefaultEmptyArray = Value{std::vector<Value>()};

    static std::unique_ptr<WindowFunctionState> create(ExpressionContext* const expCtx) {
        return std::make_unique<WindowFunctionConcatArrays>(expCtx);
    }

    explicit WindowFunctionConcatArrays(ExpressionContext* const expCtx)
        : WindowFunctionState(expCtx, MemoryUsageLimit{query_knobs::kMaxConcatArraysBytes}) {
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
        _memUsageTracker.assertWithinMemoryLimit(_expCtx->getOperationContext(), kName);
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
