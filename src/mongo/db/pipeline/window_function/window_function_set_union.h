// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/pipeline/memory_usage_container_util.h"
#include "mongo/db/pipeline/window_function/window_function.h"
#include "mongo/db/query/query_knob_descriptors_execution.h"
#include "mongo/util/modules.h"

namespace mongo {

class WindowFunctionSetUnion final : public WindowFunctionState {
public:
    static constexpr auto kName = "$setUnion"sv;
    static inline const Value kDefault = Value{std::vector<Value>()};

    static std::unique_ptr<WindowFunctionState> create(ExpressionContext* expCtx) {
        return std::make_unique<WindowFunctionSetUnion>(expCtx);
    }

    explicit WindowFunctionSetUnion(ExpressionContext* const expCtx)
        : WindowFunctionState(expCtx, MemoryUsageLimit{query_knobs::kMaxSetUnionBytes}),
          _values(ValueWithMemoryUsageComparator(&_expCtx->getValueComparator())) {
        _memUsage = sizeof(*this);
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
            int64_t size = static_cast<int64_t>(val.getApproximateSize());
            _values.emplace(ValueWithMemoryUsage<Value>{val, size});
            _memUsage += size;
            checkMemUsage(kName);
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
            _memUsage -= iter->memoryUsage;
            _values.erase(iter);
        }
    }

    void reset() override {
        _values.clear();
        _memUsage = sizeof(*this);
    }

    Value getValue(boost::optional<Value> current = boost::none) const override {
        std::vector<Value> output;
        if (_values.empty()) {
            return kDefault;
        }
        for (auto it = _values.begin(); it != _values.end(); it = _values.upper_bound(*it)) {
            output.push_back(it->value);
        }

        return Value(std::move(output));
    }

private:
    std::multiset<ValueWithMemoryUsage<Value>, ValueWithMemoryUsageComparator> _values;
};

}  // namespace mongo
