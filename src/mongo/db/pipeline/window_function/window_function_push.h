// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/document_value/value_comparator.h"
#include "mongo/db/memory_tracking/memory_usage_limit.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/memory_token_container_util.h"
#include "mongo/db/pipeline/window_function/window_function.h"
#include "mongo/db/query/query_knob_descriptors_execution.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <deque>
#include <list>
#include <memory>
#include <utility>
#include <vector>

namespace mongo {

class WindowFunctionPush final : public WindowFunctionState {
public:
    static constexpr auto kName = "$push"sv;

    using ValueListConstIterator = std::list<Value>::const_iterator;

    static inline const Value kDefault = Value{std::vector<Value>()};

    static std::unique_ptr<WindowFunctionState> create(ExpressionContext* const expCtx) {
        return std::make_unique<WindowFunctionPush>(expCtx);
    }

    explicit WindowFunctionPush(ExpressionContext* const expCtx)
        : WindowFunctionState(expCtx, MemoryUsageLimit{query_knobs::kMaxPushBytes}) {
        _memUsageTracker.set(sizeof(*this));
    }

    void add(Value value) override {
        if (value.missing()) {
            return;
        }
        _values.emplace_back(SimpleMemoryUsageToken{value.getApproximateSize(), &_memUsageTracker},
                             std::move(value));
        _memUsageTracker.assertWithinMemoryLimit(_expCtx->getOperationContext(), kName);
    }

    /**
     * This should only remove the first/lowest element in the window.
     */
    void remove(Value value) override {
        if (value.missing()) {
            return;
        }

        tassert(5423801, "Can't remove from an empty WindowFunctionPush", _values.size() != 0);
        auto valToRemove = _values.front().value();
        tassert(
            5414202,
            "Attempted to remove an element other than the first element from WindowFunctionPush",
            _expCtx->getValueComparator().evaluate(valToRemove == value));
        _values.pop_front();
    }

    void reset() override {
        _values.clear();
        _memUsageTracker.set(sizeof(*this));
    }

    Value getValue(boost::optional<Value> current = boost::none) const override {
        if (_values.empty()) {
            return kDefault;
        }
        return convertToValueFromMemoryTokenWithValue(
            _values.begin(), _values.end(), _values.size());
    }

private:
    std::deque<SimpleMemoryUsageTokenWith<Value>> _values;
};

}  // namespace mongo
