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

#include "mongo/bson/bson_depth.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/curop_failpoint_helpers.h"
#include "mongo/db/exec/expression/evaluate.h"
#include "mongo/util/elapsed_tracker.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {

MONGO_FAIL_POINT_DEFINE(mapReduceFilterPauseBeforeLoop);

namespace exec::expression {

namespace {

void mapReduceFilterWaitBeforeLoop(OperationContext* opCtx) {
    CurOpFailpointHelpers::waitWhileFailPointEnabled(
        &mapReduceFilterPauseBeforeLoop, opCtx, "mapReduceFilterPauseBeforeLoop", []() {
            LOGV2(9006800, "waiting due to 'mapReduceFilterPauseBeforeLoop' failpoint");
        });
}

std::function<void()> getExpressionInterruptChecker(OperationContext* opCtx) {
    if (opCtx) {
        ElapsedTracker et(&opCtx->fastClockSource(),
                          internalQueryExpressionInterruptIterations.load(),
                          Milliseconds{internalQueryExpressionInterruptPeriodMS.load()});
        return [=]() mutable {
            if (MONGO_unlikely(et.intervalHasElapsed())) {
                opCtx->checkForInterrupt();
            }
        };
    } else {
        return []() {
        };
    }
}

}  // namespace


Value evaluate(const ExpressionMap& expr, const Document& root, Variables* variables) {
    // guaranteed at parse time that this isn't using our _varId
    Value inputVal = expr.getInput()->evaluate(root, variables);
    if (inputVal.nullish()) {
        return Value(BSONNULL);
    }

    uassert(16883,
            str::stream() << "input to $map must be an array not " << typeName(inputVal.getType()),
            inputVal.isArray());

    const std::vector<Value>& input = inputVal.getArray();

    if (input.empty()) {
        return inputVal;
    }

    auto checkForInterrupt =
        getExpressionInterruptChecker(expr.getExpressionContext()->getOperationContext());
    mapReduceFilterWaitBeforeLoop(expr.getExpressionContext()->getOperationContext());

    size_t memUsed = 0;
    std::vector<Value> output;
    output.reserve(input.size());
    const size_t memLimit = internalQueryMaxMapFilterReduceBytes.load();
    for (size_t i = 0; i < input.size(); i++) {
        checkForInterrupt();
        variables->setValue(expr.getVarId(), input[i]);

        Value toInsert = expr.getEach()->evaluate(root, variables);
        if (toInsert.missing()) {
            toInsert = Value(BSONNULL);  // can't insert missing values into array
        }

        output.push_back(toInsert);
        memUsed += toInsert.getApproximateSize();
        if (MONGO_unlikely(memUsed > memLimit)) {
            uasserted(ErrorCodes::ExceededMemoryLimit,
                      "$map would use too much memory and cannot spill");
        }
    }

    return Value(std::move(output));
}

Value evaluate(const ExpressionReduce& expr, const Document& root, Variables* variables) {
    Value inputVal = expr.getInput()->evaluate(root, variables);

    if (inputVal.nullish()) {
        return Value(BSONNULL);
    }

    uassert(40080,
            str::stream() << "$reduce requires that 'input' be an array, found: "
                          << inputVal.toString(),
            inputVal.isArray());

    auto checkForInterrupt =
        getExpressionInterruptChecker(expr.getExpressionContext()->getOperationContext());
    mapReduceFilterWaitBeforeLoop(expr.getExpressionContext()->getOperationContext());

    size_t memLimit = internalQueryMaxMapFilterReduceBytes.load();
    Value accumulatedValue = expr.getInitial()->evaluate(root, variables);

    size_t itr = 0;
    int32_t prevDepth = -1;
    size_t interval = expr.getAccumulatedValueDepthCheckInterval();
    for (auto&& elem : inputVal.getArray()) {
        checkForInterrupt();

        variables->setValue(expr.getThisVar(), elem);
        variables->setValue(expr.getValueVar(), accumulatedValue);

        accumulatedValue = expr.getIn()->evaluate(root, variables);
        if ((interval > 0) && (itr % interval) == 0 &&
            (accumulatedValue.isObject() || accumulatedValue.isArray())) {
            int32_t depth =
                accumulatedValue.depth(2 * BSONDepth::getMaxAllowableDepth() /*maxDepth*/);
            if (MONGO_unlikely(depth == -1)) {
                uasserted(ErrorCodes::Overflow,
                          "$reduce accumulated value exceeded max allowable BSON depth");
            }
            // Exponential backoff if depth has not increased.
            if (depth == prevDepth) {
                tassert(10236400,
                        "unexpected control flow in $reduce object/array depth verification",
                        prevDepth != -1);
                interval *= 2;
            }
            prevDepth = depth;
        }
        if (MONGO_unlikely(accumulatedValue.getApproximateSize() > memLimit)) {
            uasserted(ErrorCodes::ExceededMemoryLimit,
                      "$reduce would use too much memory and cannot spill");
        }
        itr++;
    }

    return accumulatedValue;
}

Value evaluate(const ExpressionFilter& expr, const Document& root, Variables* variables) {
    // We are guaranteed at parse time that this isn't using our _varId.
    Value inputVal = expr.getInput()->evaluate(root, variables);

    if (inputVal.nullish()) {
        return Value(BSONNULL);
    }

    uassert(28651,
            str::stream() << "input to $filter must be an array not "
                          << typeName(inputVal.getType()),
            inputVal.isArray());

    const std::vector<Value>& input = inputVal.getArray();

    if (input.empty()) {
        return inputVal;
    }

    // This counter ensures we don't return more array elements than our limit arg has specified.
    // For example, given the query, {$project: {b: {$filter: {input: '$a', as: 'x', cond: {$gt:
    // ['$$x', 1]}, limit: {$literal: 3}}}}} remainingLimitCounter would be 3 and we would return up
    // to the first 3 elements matching our condition, per doc.
    auto approximateOutputSize = input.size();
    boost::optional<int> remainingLimitCounter;
    if (expr.hasLimit()) {
        auto limitValue = (expr.getChildren()[*expr.getLimit()])->evaluate(root, variables);
        // If the $filter query contains limit: null, we interpret the query as being "limit-less"
        // and therefore return all matching elements per doc.
        if (!limitValue.nullish()) {
            uassert(
                327391,
                str::stream() << "$filter: limit must be represented as a 32-bit integral value: "
                              << limitValue.toString(),
                limitValue.integral());
            int coercedLimitValue = limitValue.coerceToInt();
            uassert(327392,
                    str::stream() << "$filter: limit must be greater than 0: "
                                  << limitValue.toString(),
                    coercedLimitValue > 0);
            remainingLimitCounter = coercedLimitValue;
            approximateOutputSize =
                std::min(approximateOutputSize, static_cast<size_t>(coercedLimitValue));
        }
    }

    auto checkForInterrupt =
        getExpressionInterruptChecker(expr.getExpressionContext()->getOperationContext());
    mapReduceFilterWaitBeforeLoop(expr.getExpressionContext()->getOperationContext());

    std::vector<Value> output;
    output.reserve(approximateOutputSize);
    for (const auto& elem : input) {
        checkForInterrupt();
        variables->setValue(expr.getVariableId(), elem);

        if (expr.getCond()->evaluate(root, variables).coerceToBool()) {
            output.push_back(elem);
            if (remainingLimitCounter && --*remainingLimitCounter == 0) {
                return Value(std::move(output));
            }
        }
    }

    return Value(std::move(output));
}

}  // namespace exec::expression

}  // namespace mongo
