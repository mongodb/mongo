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

#include "mongo/db/query/stage_builder/sbe/gen_abt_helpers.h"

#include <algorithm>
#include <boost/optional/optional.hpp>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <numeric>

#include <absl/container/node_hash_map.h>

#include "mongo/bson/bsontypes.h"
#include "mongo/db/query/bson_typemask.h"
#include "mongo/db/query/optimizer/algebra/polyvalue.h"
#include "mongo/db/query/stage_builder/sbe/abt_holder_impl.h"
#include "mongo/util/assert_util.h"

namespace mongo::stage_builder {
SbExpr makeBooleanOpTree(optimizer::Operations logicOp,
                         std::vector<SbExpr> leaves,
                         StageBuilderState& state) {
    std::vector<optimizer::ABT> abtExprs;
    abtExprs.reserve(leaves.size());
    for (auto&& e : leaves) {
        abtExprs.push_back(abt::unwrap(e.extractABT()));
    }
    return abt::wrap(makeBooleanOpTree(logicOp, std::move(abtExprs)));
}

optimizer::ABT makeBooleanOpTree(optimizer::Operations logicOp,
                                 std::vector<optimizer::ABT> leaves) {
    invariant(!leaves.empty());
    if (leaves.size() == 1) {
        return std::move(leaves[0]);
    }
    if ((logicOp == optimizer::Operations::And || logicOp == optimizer::Operations::Or) &&
        feature_flags::gFeatureFlagSbeUpgradeBinaryTrees.isEnabled()) {
        return optimizer::make<optimizer::NaryOp>(logicOp, std::move(leaves));
    } else {
        auto builder = [=](optimizer::ABT lhs, optimizer::ABT rhs) {
            return optimizer::make<optimizer::BinaryOp>(logicOp, std::move(lhs), std::move(rhs));
        };
        return makeBalancedTreeImpl(builder, leaves, 0, leaves.size());
    }
}

optimizer::ABT makeFillEmpty(optimizer::ABT expr, optimizer::ABT altExpr) {
    return optimizer::make<optimizer::BinaryOp>(
        optimizer::Operations::FillEmpty, std::move(expr), std::move(altExpr));
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

optimizer::ABT makeNaryOp(optimizer::Operations op, optimizer::ABTVector args) {
    tassert(10199700, "Expected at least one argument", !args.empty());
    if (feature_flags::gFeatureFlagSbeUpgradeBinaryTrees.isEnabled()) {
        return optimizer::make<optimizer::NaryOp>(op, std::move(args));
    } else {
        return std::accumulate(
            args.begin() + 1, args.end(), std::move(args.front()), [&](auto&& acc, auto&& ex) {
                return optimizer::make<optimizer::BinaryOp>(op, std::move(acc), std::move(ex));
            });
    }
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
    return makeBooleanOpTree(
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
    if (!feature_flags::gFeatureFlagSbeUpgradeBinaryTrees.isEnabled()) {
        return std::accumulate(
            std::make_reverse_iterator(std::make_move_iterator(caseValuePairs.end())),
            std::make_reverse_iterator(std::make_move_iterator(caseValuePairs.begin())),
            std::move(defaultValue),
            [](auto&& expression, auto&& caseValuePair) {
                return buildABTMultiBranchConditional(std::move(caseValuePair),
                                                      std::move(expression));
            });
    } else {
        return optimizer::make<optimizer::Switch>(std::move(caseValuePairs),
                                                  std::move(defaultValue));
    }
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
    return optimizer::make<optimizer::Let>(name, std::move(bindExpr), std::move(expr));
}

optimizer::ABT makeLet(sbe::FrameId frameId, optimizer::ABT bindExpr, optimizer::ABT expr) {
    return optimizer::make<optimizer::Let>(
        getABTLocalVariableName(frameId, 0), std::move(bindExpr), std::move(expr));
}

optimizer::ABT makeLet(sbe::FrameId frameId, optimizer::ABTVector bindExprs, optimizer::ABT expr) {
    if (!feature_flags::gFeatureFlagSbeUpgradeBinaryTrees.isEnabled()) {
        for (size_t idx = bindExprs.size(); idx > 0;) {
            --idx;
            expr = optimizer::make<optimizer::Let>(
                getABTLocalVariableName(frameId, idx), std::move(bindExprs[idx]), std::move(expr));
        }

        return expr;
    } else {
        std::vector<optimizer::ProjectionName> bindNames;
        bindNames.reserve(bindExprs.size());
        for (size_t idx = 0; idx < bindExprs.size(); ++idx) {
            bindNames.emplace_back(getABTLocalVariableName(frameId, idx));
        }

        bindExprs.emplace_back(std::move(expr));
        return optimizer::make<optimizer::MultiLet>(std::move(bindNames), std::move(bindExprs));
    }
}

optimizer::ABT makeLet(std::vector<optimizer::ProjectionName> bindNames,
                       optimizer::ABTVector bindExprs,
                       optimizer::ABT inExpr) {
    if (!feature_flags::gFeatureFlagSbeUpgradeBinaryTrees.isEnabled()) {
        for (size_t idx = bindExprs.size(); idx > 0;) {
            --idx;
            inExpr = optimizer::make<optimizer::Let>(
                std::move(bindNames[idx]), std::move(bindExprs[idx]), std::move(inExpr));
        }
        return inExpr;
    } else {
        bindExprs.emplace_back(std::move(inExpr));
        return optimizer::make<optimizer::MultiLet>(std::move(bindNames), std::move(bindExprs));
    }
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
