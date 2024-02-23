/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/query/sbe_stage_builder_abt_helpers.h"

#include <algorithm>
#include <boost/optional/optional.hpp>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <numeric>
#include <string_view>

#include <absl/container/node_hash_map.h>

#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/sbe/abt/abt_lower.h"
#include "mongo/db/exec/sbe/expressions/runtime_environment.h"
#include "mongo/db/query/bson_typemask.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/optimizer/algebra/polyvalue.h"
#include "mongo/db/query/optimizer/reference_tracker.h"
#include "mongo/db/query/optimizer/rewrites/path_lower.h"
#include "mongo/db/query/optimizer/utils/utils.h"
#include "mongo/db/query/sbe_stage_builder.h"
#include "mongo/db/query/sbe_stage_builder_abt_holder_impl.h"
#include "mongo/db/query/sbe_stage_builder_const_eval.h"
#include "mongo/db/query/sbe_stage_builder_helpers.h"
#include "mongo/db/query/sbe_stage_builder_type_checker.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

namespace mongo::stage_builder {
std::unique_ptr<sbe::EExpression> makeBalancedBooleanOpTreeImpl(
    sbe::EPrimBinary::Op logicOp,
    std::vector<std::unique_ptr<sbe::EExpression>>& leaves,
    size_t from,
    size_t until) {
    invariant(from < until);
    if (from + 1 == until) {
        return std::move(leaves[from]);
    } else {
        size_t mid = (from + until) / 2;
        auto lhs = makeBalancedBooleanOpTreeImpl(logicOp, leaves, from, mid);
        auto rhs = makeBalancedBooleanOpTreeImpl(logicOp, leaves, mid, until);
        return makeBinaryOp(logicOp, std::move(lhs), std::move(rhs));
    }
}

std::unique_ptr<sbe::EExpression> makeBalancedBooleanOpTree(
    sbe::EPrimBinary::Op logicOp, std::vector<std::unique_ptr<sbe::EExpression>> leaves) {
    return makeBalancedBooleanOpTreeImpl(logicOp, leaves, 0, leaves.size());
}

SbExpr makeBalancedBooleanOpTree(sbe::EPrimBinary::Op logicOp,
                                 std::vector<SbExpr> leaves,
                                 StageBuilderState& state) {
    if (std::all_of(leaves.begin(), leaves.end(), [](auto&& e) { return e.canExtractABT(); })) {
        std::vector<optimizer::ABT> abtExprs;
        abtExprs.reserve(leaves.size());
        for (auto&& e : leaves) {
            abtExprs.push_back(abt::unwrap(e.extractABT()));
        }
        return abt::wrap(makeBalancedBooleanOpTree(logicOp == sbe::EPrimBinary::logicAnd
                                                       ? optimizer::Operations::And
                                                       : optimizer::Operations::Or,
                                                   std::move(abtExprs)));
    }

    std::vector<std::unique_ptr<sbe::EExpression>> exprs;
    exprs.reserve(leaves.size());
    for (auto&& e : leaves) {
        exprs.emplace_back(e.extractExpr(state));
    }
    return SbExpr{makeBalancedBooleanOpTree(logicOp, std::move(exprs))};
}

optimizer::ABT makeFillEmptyFalse(optimizer::ABT e) {
    return optimizer::make<optimizer::BinaryOp>(
        optimizer::Operations::FillEmpty, std::move(e), optimizer::Constant::boolean(false));
}

optimizer::ABT makeFillEmptyTrue(optimizer::ABT e) {
    return optimizer::make<optimizer::BinaryOp>(
        optimizer::Operations::FillEmpty, std::move(e), optimizer::Constant::boolean(true));
}

optimizer::ABT makeFillEmptyNull(optimizer::ABT e) {
    return optimizer::make<optimizer::BinaryOp>(
        optimizer::Operations::FillEmpty, std::move(e), optimizer::Constant::null());
}

optimizer::ABT makeFillEmptyUndefined(optimizer::ABT e) {
    return optimizer::make<optimizer::BinaryOp>(
        optimizer::Operations::FillEmpty,
        std::move(e),
        makeABTConstant(sbe::value::TypeTags::bsonUndefined, 0));
}

optimizer::ABT makeNot(optimizer::ABT e) {
    return makeUnaryOp(optimizer::Operations::Not, std::move(e));
}

optimizer::ABT makeUnaryOp(optimizer::Operations unaryOp, optimizer::ABT operand) {
    return optimizer::make<optimizer::UnaryOp>(unaryOp, std::move(operand));
}

optimizer::ABT makeBinaryOp(optimizer::Operations binaryOp,
                            optimizer::ABT lhs,
                            optimizer::ABT rhs) {
    return optimizer::make<optimizer::BinaryOp>(binaryOp, std::move(lhs), std::move(rhs));
}

optimizer::ABT generateABTNullOrMissing(optimizer::ABT var) {
    return makeFillEmptyTrue(
        makeABTFunction("typeMatch"_sd,
                        std::move(var),
                        optimizer::Constant::int32(getBSONTypeMask(BSONType::jstNULL))));
}

optimizer::ABT generateABTNullOrMissing(optimizer::ProjectionName var) {
    return generateABTNullOrMissing(makeVariable(std::move(var)));
}

optimizer::ABT generateABTNullMissingOrUndefined(optimizer::ABT var) {
    return makeFillEmptyTrue(
        makeABTFunction("typeMatch"_sd,
                        std::move(var),
                        optimizer::Constant::int32(getBSONTypeMask(BSONType::jstNULL) |
                                                   getBSONTypeMask(BSONType::Undefined))));
}

optimizer::ABT generateABTNullMissingOrUndefined(optimizer::ProjectionName var) {
    return generateABTNullMissingOrUndefined(makeVariable(std::move(var)));
}

optimizer::ABT generateABTNonStringCheck(optimizer::ABT var) {
    return makeNot(makeABTFunction("isString"_sd, std::move(var)));
}

optimizer::ABT generateABTNonStringCheck(optimizer::ProjectionName var) {
    return generateABTNonStringCheck(makeVariable(std::move(var)));
}

optimizer::ABT generateABTNonTimestampCheck(optimizer::ProjectionName var) {
    return makeNot(makeABTFunction("isTimestamp"_sd, makeVariable(std::move(var))));
}

optimizer::ABT generateABTNegativeCheck(optimizer::ProjectionName var) {
    return optimizer::make<optimizer::BinaryOp>(
        optimizer::Operations::And,
        makeNot(makeABTFunction("isNaN"_sd, makeVariable(var))),
        optimizer::make<optimizer::BinaryOp>(
            optimizer::Operations::Lt, makeVariable(var), optimizer::Constant::int32(0)));
}

optimizer::ABT generateABTNonPositiveCheck(optimizer::ProjectionName var) {
    return optimizer::make<optimizer::BinaryOp>(
        optimizer::Operations::Lte, makeVariable(std::move(var)), optimizer::Constant::int32(0));
}

optimizer::ABT generateABTPositiveCheck(optimizer::ABT var) {
    return optimizer::make<optimizer::BinaryOp>(
        optimizer::Operations::Gt, std::move(var), optimizer::Constant::int32(0));
}

optimizer::ABT generateABTNonNumericCheck(optimizer::ProjectionName var) {
    return makeNot(makeABTFunction("isNumber"_sd, makeVariable(std::move(var))));
}

optimizer::ABT generateABTLongLongMinCheck(optimizer::ProjectionName var) {
    return optimizer::make<optimizer::BinaryOp>(
        optimizer::Operations::And,
        makeABTFunction("typeMatch"_sd,
                        makeVariable(var),
                        optimizer::Constant::int32(getBSONTypeMask(BSONType::NumberLong))),
        optimizer::make<optimizer::BinaryOp>(
            optimizer::Operations::Eq,
            makeVariable(var),
            optimizer::Constant::int64(std::numeric_limits<int64_t>::min())));
}

optimizer::ABT generateABTNonArrayCheck(optimizer::ProjectionName var) {
    return makeNot(makeABTFunction("isArray"_sd, makeVariable(std::move(var))));
}

optimizer::ABT generateABTNonObjectCheck(optimizer::ProjectionName var) {
    return makeNot(makeABTFunction("isObject"_sd, makeVariable(std::move(var))));
}

optimizer::ABT generateABTNullishOrNotRepresentableInt32Check(optimizer::ProjectionName var) {
    return optimizer::make<optimizer::BinaryOp>(
        optimizer::Operations::Or,
        generateABTNullMissingOrUndefined(var),
        makeNot(makeABTFunction("exists"_sd,
                                makeABTFunction("convert"_sd,
                                                makeVariable(var),
                                                optimizer::Constant::int32(static_cast<int32_t>(
                                                    sbe::value::TypeTags::NumberInt32))))));
}

optimizer::ABT generateInvalidRoundPlaceArgCheck(const optimizer::ProjectionName& var) {
    return makeBalancedBooleanOpTree(
        optimizer::Operations::Or,
        {
            // We can perform our numerical test with trunc. trunc will return nothing if we pass a
            // non-number to it. We return true if the comparison returns nothing, or if
            // var != trunc(var), indicating this is not a whole number.
            makeFillEmptyTrue(
                optimizer::make<optimizer::BinaryOp>(optimizer::Operations::Neq,
                                                     makeVariable(var),
                                                     makeABTFunction("trunc", makeVariable(var)))),
            optimizer::make<optimizer::BinaryOp>(
                optimizer::Operations::Lt, makeVariable(var), optimizer::Constant::int32(-20)),
            optimizer::make<optimizer::BinaryOp>(
                optimizer::Operations::Gt, makeVariable(var), optimizer::Constant::int32(100)),
        });
}

optimizer::ABT generateABTNaNCheck(optimizer::ProjectionName var) {
    return makeABTFunction("isNaN"_sd, makeVariable(std::move(var)));
}

optimizer::ABT generateABTInfinityCheck(optimizer::ProjectionName var) {
    return makeABTFunction("isInfinity"_sd, makeVariable(std::move(var)));
}

template <>
optimizer::ABT buildABTMultiBranchConditional(optimizer::ABT defaultCase) {
    return defaultCase;
}

optimizer::ABT buildABTMultiBranchConditionalFromCaseValuePairs(
    std::vector<ABTCaseValuePair> caseValuePairs, optimizer::ABT defaultValue) {
    return std::accumulate(
        std::make_reverse_iterator(std::make_move_iterator(caseValuePairs.end())),
        std::make_reverse_iterator(std::make_move_iterator(caseValuePairs.begin())),
        std::move(defaultValue),
        [](auto&& expression, auto&& caseValuePair) {
            return buildABTMultiBranchConditional(std::move(caseValuePair), std::move(expression));
        });
}

optimizer::ABT makeIfNullExpr(std::vector<optimizer::ABT> values,
                              sbe::value::FrameIdGenerator* frameIdGenerator) {
    tassert(6987505, "Expected 'values' to be non-empty", values.size() > 0);

    size_t idx = values.size() - 1;
    auto expr = std::move(values[idx]);

    while (idx > 0) {
        --idx;

        auto var = getABTLocalVariableName(frameIdGenerator->generate(), 0);

        expr = optimizer::make<optimizer::Let>(
            var,
            std::move(values[idx]),
            optimizer::make<optimizer::If>(
                generateABTNullMissingOrUndefined(var), std::move(expr), makeVariable(var)));
    }

    return expr;
}

optimizer::ABT makeIf(optimizer::ABT condExpr, optimizer::ABT thenExpr, optimizer::ABT elseExpr) {
    return optimizer::make<optimizer::If>(
        std::move(condExpr), std::move(thenExpr), std::move(elseExpr));
}

optimizer::ABT makeLet(const optimizer::ProjectionName& name,
                       optimizer::ABT bindExpr,
                       optimizer::ABT expr) {
    // Verify that 'name' was generated by calling 'getABTLocalVariableName(N, 0)' for some
    // frame ID 'N'.
    auto localVarInfo = getSbeLocalVariableInfo(name);
    tassert(7654322, "", localVarInfo.has_value() && localVarInfo->second == 0);

    return optimizer::make<optimizer::Let>(name, std::move(bindExpr), std::move(expr));
}

optimizer::ABT makeLet(sbe::FrameId frameId, optimizer::ABT bindExpr, optimizer::ABT expr) {
    return optimizer::make<optimizer::Let>(
        getABTLocalVariableName(frameId, 0), std::move(bindExpr), std::move(expr));
}

optimizer::ABT makeLet(sbe::FrameId frameId, optimizer::ABTVector bindExprs, optimizer::ABT expr) {
    for (size_t idx = bindExprs.size(); idx > 0;) {
        --idx;
        expr = optimizer::make<optimizer::Let>(
            getABTLocalVariableName(frameId, idx), std::move(bindExprs[idx]), std::move(expr));
    }

    return expr;
}

optimizer::ABT makeLocalLambda(sbe::FrameId frameId, optimizer::ABT expr) {
    optimizer::ProjectionName var = getABTLocalVariableName(frameId, 0);
    return optimizer::make<optimizer::LambdaAbstraction>(std::move(var), std::move(expr));
}

optimizer::ABT makeNumericConvert(optimizer::ABT expr, sbe::value::TypeTags tag) {
    return makeABTFunction(
        "convert"_sd, std::move(expr), optimizer::Constant::int32(static_cast<int32_t>(tag)));
}

optimizer::ABT makeABTFail(ErrorCodes::Error error, StringData errorMessage) {
    return makeABTFunction(
        "fail"_sd, optimizer::Constant::int32(error), makeABTConstant(errorMessage));
}
}  // namespace mongo::stage_builder
