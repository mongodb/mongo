// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/document_value/value_comparator.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/memory_usage_container_util.h"
#include "mongo/db/pipeline/window_function/window_function.h"
#include "mongo/db/query/query_knob_descriptors_execution.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <map>
#include <memory>
#include <utility>
#include <vector>

namespace mongo {

class WindowFunctionAddToSet final : public WindowFunctionState {
public:
    static constexpr auto kName = "$addToSet"sv;

    static inline const Value kDefault = Value{std::vector<Value>()};

    static std::unique_ptr<WindowFunctionState> create(ExpressionContext* const expCtx) {
        return std::make_unique<WindowFunctionAddToSet>(expCtx);
    }

    explicit WindowFunctionAddToSet(ExpressionContext* const expCtx)
        : WindowFunctionState(expCtx, MemoryUsageLimit{query_knobs::kMaxAddToSetBytes}),
          _values(ValueWithMemoryUsageComparator(&_expCtx->getValueComparator())) {
        _memUsage = sizeof(*this);
    }

    void add(Value value) override {
        int64_t size = static_cast<int64_t>(value.getApproximateSize());
        _values.emplace(ValueWithMemoryUsage<Value>{std::move(value), size});
        _memUsage += size;
        checkMemUsage(kName);
    }

    /**
     * This should only remove the first/lowest element in the window.
     */
    void remove(Value value) override {
        auto iter = _values.find(std::move(value));
        tassert(
            5423800, "Can't remove from an empty WindowFunctionAddToSet", iter != _values.end());
        _memUsage -= iter->memoryUsage;
        _values.erase(iter);
    }

    void reset() override {
        _values.clear();
        _memUsage = sizeof(*this);
    }

    Value getValue(boost::optional<Value> current = boost::none) const override {
        std::vector<Value> output;
        if (_values.empty())
            return kDefault;
        for (auto it = _values.begin(); it != _values.end(); it = _values.upper_bound(*it)) {
            output.push_back(it->value);
        }

        return Value(std::move(output));
    }

private:
    std::multiset<ValueWithMemoryUsage<Value>, ValueWithMemoryUsageComparator> _values;
};

}  // namespace mongo
