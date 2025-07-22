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
#include "mongo/db/query/compiler/logical_model/sort_pattern/sort_pattern.h"

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
