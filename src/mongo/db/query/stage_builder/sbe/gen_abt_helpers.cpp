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

#include "mongo/bson/bsontypes.h"
#include "mongo/db/query/algebra/polyvalue.h"
#include "mongo/db/query/bson_typemask.h"
#include "mongo/db/query/stage_builder/sbe/abt_holder_impl.h"
#include "mongo/util/assert_util.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <numeric>

#include <absl/container/node_hash_map.h>
#include <boost/optional/optional.hpp>

namespace mongo::stage_builder {
SbExpr makeBooleanOpTree(abt::Operations logicOp,
                         std::vector<SbExpr> leaves,
                         StageBuilderState& state) {
    std::vector<abt::ABT> abtExprs;
    abtExprs.reserve(leaves.size());
    for (auto&& e : leaves) {
        abtExprs.push_back(unwrap(e.extractABT()));
    }
    return wrap(makeBooleanOpTree(logicOp, std::move(abtExprs)));
}

abt::ABT makeBooleanOpTree(abt::Operations logicOp, std::vector<abt::ABT> leaves) {
    invariant(!leaves.empty());
    if (leaves.size() == 1) {
        return std::move(leaves[0]);
    }
    if ((logicOp == abt::Operations::And || logicOp == abt::Operations::Or) &&
        feature_flags::gFeatureFlagSbeUpgradeBinaryTrees.checkEnabled()) {
        return abt::make<abt::NaryOp>(logicOp, std::move(leaves));
    } else {
        auto builder = [=](abt::ABT lhs, abt::ABT rhs) {
            return abt::make<abt::BinaryOp>(logicOp, std::move(lhs), std::move(rhs));
        };
        return makeBalancedTreeImpl(builder, leaves, 0, leaves.size());
    }
}

abt::ABT makeFillEmpty(abt::ABT expr, abt::ABT altExpr) {
    return abt::make<abt::BinaryOp>(
        abt::Operations::FillEmpty, std::move(expr), std::move(altExpr));
}

abt::ABT makeFillEmptyFalse(abt::ABT e) {
    return abt::make<abt::BinaryOp>(
        abt::Operations::FillEmpty, std::move(e), abt::Constant::boolean(false));
}

abt::ABT makeFillEmptyTrue(abt::ABT e) {
    return abt::make<abt::BinaryOp>(
        abt::Operations::FillEmpty, std::move(e), abt::Constant::boolean(true));
}

abt::ABT makeFillEmptyNull(abt::ABT e) {
    return abt::make<abt::BinaryOp>(
        abt::Operations::FillEmpty, std::move(e), abt::Constant::null());
}

abt::ABT makeFillEmptyUndefined(abt::ABT e) {
    return abt::make<abt::BinaryOp>(abt::Operations::FillEmpty,
                                    std::move(e),
                                    makeABTConstant(sbe::value::TypeTags::bsonUndefined, 0));
}

abt::ABT makeNot(abt::ABT e) {
    return makeUnaryOp(abt::Operations::Not, std::move(e));
}

abt::ABT makeUnaryOp(abt::Operations unaryOp, abt::ABT operand) {
    return abt::make<abt::UnaryOp>(unaryOp, std::move(operand));
}

abt::ABT makeBinaryOp(abt::Operations binaryOp, abt::ABT lhs, abt::ABT rhs) {
    return abt::make<abt::BinaryOp>(binaryOp, std::move(lhs), std::move(rhs));
}

abt::ABT makeNaryOp(abt::Operations op, abt::ABTVector args) {
    tassert(10199700, "Expected at least one argument", !args.empty());
    if (feature_flags::gFeatureFlagSbeUpgradeBinaryTrees.checkEnabled()) {
        return abt::make<abt::NaryOp>(op, std::move(args));
    } else {
        return std::accumulate(
            args.begin() + 1, args.end(), std::move(args.front()), [&](auto&& acc, auto&& ex) {
                return abt::make<abt::BinaryOp>(op, std::move(acc), std::move(ex));
            });
    }
}

abt::ABT generateABTNullOrMissing(abt::ABT var) {
    return makeFillEmptyTrue(makeABTFunction(
        "typeMatch"_sd, std::move(var), abt::Constant::int32(getBSONTypeMask(BSONType::null))));
}

abt::ABT generateABTNullOrMissing(abt::ProjectionName var) {
    return generateABTNullOrMissing(makeVariable(std::move(var)));
}

abt::ABT generateABTNullMissingOrUndefined(abt::ABT var) {
    return makeFillEmptyTrue(
        makeABTFunction("typeMatch"_sd,
                        std::move(var),
                        abt::Constant::int32(getBSONTypeMask(BSONType::null) |
                                             getBSONTypeMask(BSONType::undefined))));
}

abt::ABT generateABTNullMissingOrUndefined(abt::ProjectionName var) {
    return generateABTNullMissingOrUndefined(makeVariable(std::move(var)));
}

abt::ABT generateABTNonStringCheck(abt::ABT var) {
    return makeNot(makeABTFunction("isString"_sd, std::move(var)));
}

abt::ABT generateABTNonStringCheck(abt::ProjectionName var) {
    return generateABTNonStringCheck(makeVariable(std::move(var)));
}

abt::ABT generateABTNonTimestampCheck(abt::ProjectionName var) {
    return makeNot(makeABTFunction("isTimestamp"_sd, makeVariable(std::move(var))));
}

abt::ABT generateABTNegativeCheck(abt::ProjectionName var) {
    return abt::make<abt::BinaryOp>(
        abt::Operations::And,
        makeNot(makeABTFunction("isNaN"_sd, makeVariable(var))),
        abt::make<abt::BinaryOp>(abt::Operations::Lt, makeVariable(var), abt::Constant::int32(0)));
}

abt::ABT generateABTNonPositiveCheck(abt::ProjectionName var) {
    return abt::make<abt::BinaryOp>(
        abt::Operations::Lte, makeVariable(std::move(var)), abt::Constant::int32(0));
}

abt::ABT generateABTPositiveCheck(abt::ABT var) {
    return abt::make<abt::BinaryOp>(abt::Operations::Gt, std::move(var), abt::Constant::int32(0));
}

abt::ABT generateABTNonNumericCheck(abt::ProjectionName var) {
    return makeNot(makeABTFunction("isNumber"_sd, makeVariable(std::move(var))));
}

abt::ABT generateABTLongLongMinCheck(abt::ProjectionName var) {
    return abt::make<abt::BinaryOp>(
        abt::Operations::And,
        makeABTFunction("typeMatch"_sd,
                        makeVariable(var),
                        abt::Constant::int32(getBSONTypeMask(BSONType::numberLong))),
        abt::make<abt::BinaryOp>(abt::Operations::Eq,
                                 makeVariable(var),
                                 abt::Constant::int64(std::numeric_limits<int64_t>::min())));
}

abt::ABT generateABTNonArrayCheck(abt::ProjectionName var) {
    return makeNot(makeABTFunction("isArray"_sd, makeVariable(std::move(var))));
}

abt::ABT generateABTNonObjectCheck(abt::ProjectionName var) {
    return makeNot(makeABTFunction("isObject"_sd, makeVariable(std::move(var))));
}

abt::ABT generateABTNullishOrNotRepresentableInt32Check(abt::ProjectionName var) {
    return abt::make<abt::BinaryOp>(
        abt::Operations::Or,
        generateABTNullMissingOrUndefined(var),
        makeNot(makeABTFunction("exists"_sd,
                                makeABTFunction("convert"_sd,
                                                makeVariable(var),
                                                abt::Constant::int32(static_cast<int32_t>(
                                                    sbe::value::TypeTags::NumberInt32))))));
}

abt::ABT generateInvalidRoundPlaceArgCheck(const abt::ProjectionName& var) {
    return makeBooleanOpTree(
        abt::Operations::Or,
        {
            // We can perform our numerical test with trunc. trunc will return nothing if we pass a
            // non-number to it. We return true if the comparison returns nothing, or if
            // var != trunc(var), indicating this is not a whole number.
            makeFillEmptyTrue(
                abt::make<abt::BinaryOp>(abt::Operations::Neq,
                                         makeVariable(var),
                                         makeABTFunction("trunc", makeVariable(var)))),
            abt::make<abt::BinaryOp>(
                abt::Operations::Lt, makeVariable(var), abt::Constant::int32(-20)),
            abt::make<abt::BinaryOp>(
                abt::Operations::Gt, makeVariable(var), abt::Constant::int32(100)),
        });
}

abt::ABT generateABTNaNCheck(abt::ProjectionName var) {
    return makeABTFunction("isNaN"_sd, makeVariable(std::move(var)));
}

abt::ABT generateABTInfinityCheck(abt::ProjectionName var) {
    return makeABTFunction("isInfinity"_sd, makeVariable(std::move(var)));
}

template <>
abt::ABT buildABTMultiBranchConditional(abt::ABT defaultCase) {
    return defaultCase;
}

abt::ABT buildABTMultiBranchConditionalFromCaseValuePairs(
    std::vector<ABTCaseValuePair> caseValuePairs, abt::ABT defaultValue) {
    if (!feature_flags::gFeatureFlagSbeUpgradeBinaryTrees.checkEnabled()) {
        return std::accumulate(
            std::make_reverse_iterator(std::make_move_iterator(caseValuePairs.end())),
            std::make_reverse_iterator(std::make_move_iterator(caseValuePairs.begin())),
            std::move(defaultValue),
            [](auto&& expression, auto&& caseValuePair) {
                return buildABTMultiBranchConditional(std::move(caseValuePair),
                                                      std::move(expression));
            });
    } else {
        return abt::make<abt::Switch>(std::move(caseValuePairs), std::move(defaultValue));
    }
}

abt::ABT makeIfNullExpr(std::vector<abt::ABT> values,
                        sbe::value::FrameIdGenerator* frameIdGenerator) {
    tassert(6987505, "Expected 'values' to be non-empty", values.size() > 0);

    size_t idx = values.size() - 1;
    auto expr = std::move(values[idx]);

    while (idx > 0) {
        --idx;

        auto var = getABTLocalVariableName(frameIdGenerator->generate(), 0);

        expr = abt::make<abt::Let>(var,
                                   std::move(values[idx]),
                                   abt::make<abt::If>(generateABTNullMissingOrUndefined(var),
                                                      std::move(expr),
                                                      makeVariable(var)));
    }

    return expr;
}

abt::ABT makeIf(abt::ABT condExpr, abt::ABT thenExpr, abt::ABT elseExpr) {
    return abt::make<abt::If>(std::move(condExpr), std::move(thenExpr), std::move(elseExpr));
}

abt::ABT makeLet(const abt::ProjectionName& name, abt::ABT bindExpr, abt::ABT expr) {
    return abt::make<abt::Let>(name, std::move(bindExpr), std::move(expr));
}

abt::ABT makeLet(sbe::FrameId frameId, abt::ABT bindExpr, abt::ABT expr) {
    return abt::make<abt::Let>(
        getABTLocalVariableName(frameId, 0), std::move(bindExpr), std::move(expr));
}

abt::ABT makeLet(sbe::FrameId frameId, abt::ABTVector bindExprs, abt::ABT expr) {
    if (!feature_flags::gFeatureFlagSbeUpgradeBinaryTrees.checkEnabled()) {
        for (size_t idx = bindExprs.size(); idx > 0;) {
            --idx;
            expr = abt::make<abt::Let>(
                getABTLocalVariableName(frameId, idx), std::move(bindExprs[idx]), std::move(expr));
        }

        return expr;
    } else {
        std::vector<abt::ProjectionName> bindNames;
        bindNames.reserve(bindExprs.size());
        for (size_t idx = 0; idx < bindExprs.size(); ++idx) {
            bindNames.emplace_back(getABTLocalVariableName(frameId, idx));
        }

        bindExprs.emplace_back(std::move(expr));
        return abt::make<abt::MultiLet>(std::move(bindNames), std::move(bindExprs));
    }
}

abt::ABT makeLet(std::vector<abt::ProjectionName> bindNames,
                 abt::ABTVector bindExprs,
                 abt::ABT inExpr) {
    if (!feature_flags::gFeatureFlagSbeUpgradeBinaryTrees.checkEnabled()) {
        for (size_t idx = bindExprs.size(); idx > 0;) {
            --idx;
            inExpr = abt::make<abt::Let>(
                std::move(bindNames[idx]), std::move(bindExprs[idx]), std::move(inExpr));
        }
        return inExpr;
    } else {
        bindExprs.emplace_back(std::move(inExpr));
        return abt::make<abt::MultiLet>(std::move(bindNames), std::move(bindExprs));
    }
}

abt::ABT makeLocalLambda(sbe::FrameId frameId, abt::ABT expr) {
    abt::ProjectionName var = getABTLocalVariableName(frameId, 0);
    return abt::make<abt::LambdaAbstraction>(std::move(var), std::move(expr));
}

abt::ABT makeNumericConvert(abt::ABT expr, sbe::value::TypeTags tag) {
    return makeABTFunction(
        "convert"_sd, std::move(expr), abt::Constant::int32(static_cast<int32_t>(tag)));
}

abt::ABT makeABTFail(ErrorCodes::Error error, StringData errorMessage) {
    return makeABTFunction("fail"_sd, abt::Constant::int32(error), makeABTConstant(errorMessage));
}
}  // namespace mongo::stage_builder
