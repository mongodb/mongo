// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/accumulation_statement.h"
#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/accumulator_multi.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/pipeline/window_function/window_function.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

namespace mongo {

class WindowFunctionMinMaxCommon : public WindowFunctionState {
public:
    void add(Value value) final {
        // Ignore nullish values.
        if (value.nullish()) {
            return;
        }
        _values.emplace(SimpleMemoryUsageToken{value.getApproximateSize(), &_memUsageTracker},
                        std::move(value));
    }

    void remove(Value value) final {
        // Ignore nullish values.
        if (value.nullish()) {
            return;
        }
        // std::multiset::insert is guaranteed to put the element after any equal elements
        // already in the container. So find() / erase() will remove the oldest equal element,
        // which is what we want, to satisfy "remove() undoes add() when called in FIFO order".
        auto iter = _values.find(value);
        tassert(5371400, "Can't remove from an empty WindowFunctionMinMax", iter != _values.end());
        _values.erase(iter);
    }

    void reset() final {
        _values.clear();
        _memUsageTracker.set(sizeof(*this));
    }

protected:
    // Constructor hidden so that only instances of the derived types can be created.
    explicit WindowFunctionMinMaxCommon(ExpressionContext* const expCtx)
        : WindowFunctionState(expCtx),
          _values(MemoryTokenValueComparator(&_expCtx->getValueComparator())) {}

    // Holds all the values in the window, in order, with constant-time access to both ends.
    std::multiset<SimpleMemoryUsageTokenWith<Value>, MemoryTokenValueComparator> _values;
};

template <AccumulatorMinMax::Sense sense>
class WindowFunctionMinMax : public WindowFunctionMinMaxCommon {
public:
    using WindowFunctionMinMaxCommon::_memUsageTracker;
    using WindowFunctionMinMaxCommon::_values;

    static inline const Value kDefault = Value{BSONNULL};

    static std::unique_ptr<WindowFunctionState> create(ExpressionContext* const expCtx) {
        return std::make_unique<WindowFunctionMinMax<sense>>(expCtx);
    }

    explicit WindowFunctionMinMax(ExpressionContext* const expCtx)
        : WindowFunctionMinMaxCommon(expCtx) {
        _memUsageTracker.set(sizeof(*this));
    }

    Value getValue(boost::optional<Value> current = boost::none) const final {
        if (_values.empty())
            return kDefault;
        if constexpr (sense == AccumulatorMinMax::Sense::kMin) {
            return _values.begin()->value();
        } else {
            return _values.rbegin()->value();
        }
        MONGO_UNREACHABLE_TASSERT(5371401);
    }
};

template <AccumulatorMinMax::Sense sense>
class WindowFunctionMinMaxN : public WindowFunctionMinMaxCommon {
public:
    using WindowFunctionMinMaxCommon::_memUsageTracker;
    using WindowFunctionMinMaxCommon::_values;

    static std::unique_ptr<WindowFunctionState> create(ExpressionContext* const expCtx,
                                                       long long n) {
        return std::make_unique<WindowFunctionMinMaxN<sense>>(expCtx, n);
    }

    static AccumulationExpression parse(ExpressionContext* const expCtx,
                                        BSONElement elem,
                                        VariablesParseState vps) {
        return AccumulatorMinMaxN::parseMinMaxN<sense>(expCtx, elem, vps);
    }

    static const char* getName() {
        if constexpr (sense == AccumulatorMinMax::Sense::kMin) {
            return AccumulatorMinN::getName();
        } else {
            return AccumulatorMaxN::getName();
        }
    }

    explicit WindowFunctionMinMaxN(ExpressionContext* const expCtx, long long n)
        : WindowFunctionMinMaxCommon(expCtx), _n(n) {
        _memUsageTracker.set(sizeof(*this));
    }

    Value getValue(boost::optional<Value> current = boost::none) const final {
        if (_values.empty()) {
            return Value(std::vector<Value>());
        }

        auto processVal = [&](auto begin, auto end, size_t size) -> Value {
            auto n = std::min(static_cast<size_t>(_n), size);
            std::vector<Value> result;
            result.reserve(n);
            auto it = begin;
            for (size_t i = 0; i < n; ++i, ++it) {
                result.push_back(it->value());
            }
            return Value(std::move(result));
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
