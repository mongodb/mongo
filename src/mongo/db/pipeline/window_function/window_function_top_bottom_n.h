// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/pipeline/accumulator_multi.h"
#include "mongo/db/pipeline/window_function/window_function.h"
#include "mongo/db/query/compiler/logical_model/sort_pattern/sort_pattern.h"
#include "mongo/util/modules.h"

namespace mongo {

/**
 * Adaptor/wrapper class makes AccumulatorTopBottomN fulfill the interface of a window function.
 * This is possible since this particular accumulator has a remove() method.
 */
template <TopBottomSense sense, bool single>
class WindowFunctionTopBottomN : public WindowFunctionState {
public:
    static std::unique_ptr<WindowFunctionState> create(ExpressionContext* const expCtx,
                                                       SortPattern sp,
                                                       long long n) {
        return std::make_unique<WindowFunctionTopBottomN<sense, single>>(expCtx, std::move(sp), n);
    }

    static AccumulationExpression parse(ExpressionContext* const expCtx,
                                        BSONElement elem,
                                        VariablesParseState vps) {
        return AccumulatorTopBottomN<sense, single>::parseTopBottomN(expCtx, elem, vps);
    }

    explicit WindowFunctionTopBottomN(ExpressionContext* const expCtx, SortPattern sp, long long n)
        : WindowFunctionState(expCtx), _acc(expCtx, std::move(sp), true) {
        _acc.startNewGroup(Value(n));
        updateMemUsage();
    }

    void add(Value value) final {
        _acc.process(value, false);
        updateMemUsage();
    }

    void remove(Value value) final {
        _acc.remove(value);
        updateMemUsage();
    }

    Value getValue(boost::optional<Value> current = boost::none) const final {
        return _acc.getValueConst(false);
    }

    void reset() final {
        _acc.reset();
        updateMemUsage();
    }

private:
    void updateMemUsage() {
        _memUsageTracker.set(sizeof(*this) + _acc.getMemUsage());
    }

    AccumulatorTopBottomN<sense, single> _acc;
};

using WindowFunctionTopN = WindowFunctionTopBottomN<TopBottomSense::kTop, false>;
using WindowFunctionBottomN = WindowFunctionTopBottomN<TopBottomSense::kBottom, false>;
using WindowFunctionTop = WindowFunctionTopBottomN<TopBottomSense::kTop, true>;
using WindowFunctionBottom = WindowFunctionTopBottomN<TopBottomSense::kBottom, true>;
};  // namespace mongo
