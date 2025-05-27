/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/query/stage_builder/sbe/abt_lower.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/slots_provider.h"
#include "mongo/db/query/algebra/operator.h"
#include "mongo/db/query/algebra/polyvalue.h"
#include "mongo/db/query/stage_builder/sbe/abt/comparison_op.h"
#include "mongo/util/str.h"

#include <cstdint>
#include <limits>
#include <utility>

#include <absl/container/inlined_vector.h>
#include <absl/container/node_hash_map.h>
#include <absl/container/node_hash_set.h>
#include <absl/meta/type_traits.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo::stage_builder::abt_lower {

static sbe::EExpression::Vector toInlinedVector(
    std::vector<std::unique_ptr<sbe::EExpression>> args) {
    sbe::EExpression::Vector inlined;
    inlined.reserve(args.size());
    for (auto&& arg : args) {
        inlined.emplace_back(std::move(arg));
    }
    return inlined;
}

std::unique_ptr<sbe::EExpression> VarResolver::operator()(const ProjectionName& name) const {
    if (_slotMap) {
        if (auto it = _slotMap->find(name); it != _slotMap->end()) {
            return sbe::makeE<sbe::EVariable>(it->second);
        }
    }

    if (_lowerFn) {
        return _lowerFn(name);
    }

    return {};
}

std::unique_ptr<sbe::EExpression> SBEExpressionLowering::optimize(const ABT& n) {
    return algebra::transport<false>(n, *this);
}

std::unique_ptr<sbe::EExpression> SBEExpressionLowering::transport(const Constant& c) {
    auto [tag, val] = c.get();
    auto [copyTag, copyVal] = sbe::value::copyValue(tag, val);
    sbe::value::ValueGuard guard(copyTag, copyVal);

    auto result = sbe::makeE<sbe::EConstant>(copyTag, copyVal);

    guard.reset();
    return result;
}

std::unique_ptr<sbe::EExpression> SBEExpressionLowering::transport(const Source&) {
    tasserted(6624202, "not yet implemented");
    return nullptr;
}

void SBEExpressionLowering::prepare(const Let& let) {
    // Assign a frame ID for the local variable bound by this Let expression.
    _letMap[&let] = generateFrameId();
}

std::unique_ptr<sbe::EExpression> SBEExpressionLowering::transport(
    const Let& let, std::unique_ptr<sbe::EExpression> bind, std::unique_ptr<sbe::EExpression> in) {
    auto it = _letMap.find(&let);
    tassert(6624206, "incorrect let map", it != _letMap.end());
    auto frameId = it->second;
    _letMap.erase(it);

    return sbe::makeE<sbe::ELocalBind>(frameId, sbe::makeEs(std::move(bind)), std::move(in));
}

void SBEExpressionLowering::prepare(const MultiLet& multiLet) {
    // Assign a frame ID and slotId for the local variables bound by this MultiLet expression.

    abt::ProjectionNameMap<sbe::value::SlotId> slotIdMap{};
    auto frameId = generateFrameId();

    sbe::value::SlotId id = 0;
    for (auto& name : multiLet.varNames()) {
        slotIdMap.emplace(name, id++);
    }

    _multiLetMap[&multiLet] = {frameId, std::move(slotIdMap)};
}

std::unique_ptr<sbe::EExpression> SBEExpressionLowering::transport(
    const MultiLet& multiLet, std::vector<std::unique_ptr<sbe::EExpression>> args) {
    auto it = _multiLetMap.find(&multiLet);
    tassert(10130804, "incorrect multiLet map", it != _multiLetMap.end());
    auto frameId = it->second.first;
    _multiLetMap.erase(it);

    sbe::EExpression::Vector binds;
    binds.reserve(args.size() - 1);
    for (size_t idx = 0; idx < args.size() - 1; ++idx) {
        binds.emplace_back(std::move(args[idx]));
    }

    return sbe::makeE<sbe::ELocalBind>(frameId, std::move(binds), std::move(args.back()));
}

void SBEExpressionLowering::prepare(const LambdaAbstraction& lam) {
    // Assign a frame ID for the local variable bound by this LambdaAbstraction.
    _lambdaMap[&lam] = generateFrameId();
}

std::unique_ptr<sbe::EExpression> SBEExpressionLowering::transport(
    const LambdaAbstraction& lam, std::unique_ptr<sbe::EExpression> body) {
    auto it = _lambdaMap.find(&lam);
    tassert(6624207, "incorrect lambda map", it != _lambdaMap.end());
    auto frameId = it->second;
    _lambdaMap.erase(it);

    return sbe::makeE<sbe::ELocalLambda>(frameId, std::move(body));
}

std::unique_ptr<sbe::EExpression> SBEExpressionLowering::transport(
    const LambdaApplication&,
    std::unique_ptr<sbe::EExpression> lam,
    std::unique_ptr<sbe::EExpression> arg) {
    // lambda applications are not directly supported by SBE (yet) and must not be present.
    tasserted(6624208, "lambda application is not implemented");
    return nullptr;
}

std::unique_ptr<sbe::EExpression> SBEExpressionLowering::transport(const Variable& var) {
    auto def = _env.getDefinition(var);

    if (!def.definedBy.empty()) {
        // If this variable was defined by a Let expression, use the frame ID that was defined in
        // the prepare() step for the Let.
        if (auto let = def.definedBy.cast<Let>(); let) {
            auto it = _letMap.find(let);
            tassert(6624203, "incorrect let map", it != _letMap.end());

            return sbe::makeE<sbe::EVariable>(it->second, 0, _env.isLastRef(var));
        } else if (auto multiLet = def.definedBy.cast<MultiLet>(); multiLet) {
            auto it = _multiLetMap.find(multiLet);
            tassert(10130807, "incorrect multiLet map", it != _multiLetMap.end());

            auto slotId = it->second.second.find(var.name());
            tassert(10130808,
                    str::stream() << "couldn't find variable: " << var.name()
                                  << " in the slotId map",
                    slotId != it->second.second.end());

            return sbe::makeE<sbe::EVariable>(
                it->second.first, slotId->second, _env.isLastRef(var));
        } else if (auto lam = def.definedBy.cast<LambdaAbstraction>(); lam) {
            // Similarly if the variable was defined by a lambda abstraction, use a frame ID rather
            // than a slot.
            auto it = _lambdaMap.find(lam);
            tassert(6624204, "incorrect lambda map", it != _lambdaMap.end());

            return sbe::makeE<sbe::EVariable>(it->second, 0, _env.isLastRef(var));
        }
    }

    // If variable was not defined in the scope of the local expression via a Let or
    // LambdaAbstraction, it must be a reference that will be in the slotMap.
    if (auto expr = _varResolver(var.name())) {
        return expr;
    }

    tasserted(6624205, str::stream() << "undefined variable: " << var.name());
    return nullptr;
}

std::unique_ptr<sbe::EExpression> SBEExpressionLowering::transport(
    const BinaryOp& op,
    std::unique_ptr<sbe::EExpression> lhs,
    std::unique_ptr<sbe::EExpression> rhs) {

    if (op.op() == Operations::EqMember) {
        // We directly translate BinaryOp [EqMember] to the SBE function isMember.
        sbe::EExpression::Vector isMemberArgs;
        isMemberArgs.push_back(std::move(lhs));
        isMemberArgs.push_back(std::move(rhs));

        return sbe::makeE<sbe::EFunction>("isMember", std::move(isMemberArgs));
    }

    sbe::EPrimBinary::Op sbeOp = getEPrimBinaryOp(op.op());

    if (sbe::EPrimBinary::isComparisonOp(sbeOp)) {
        auto collatorSlot = _providedSlots.getSlotIfExists("collator"_sd);

        auto hasNonCollatableType = [](const std::unique_ptr<sbe::EExpression>& arg) {
            auto constExpr = arg->as<sbe::EConstant>();
            return constExpr && !sbe::value::isCollatableType(constExpr->getConstant().first);
        };

        // If either arg is a non-collatable type constant, we can always use the native
        // comparison op even when a collation is set (because the native comparison op
        // will behave the same as the collation-aware comparison op for such cases).
        const bool useCollationAwareOp =
            collatorSlot && !hasNonCollatableType(lhs) && !hasNonCollatableType(rhs);

        auto collationExpr = [&]() -> std::unique_ptr<sbe::EExpression> {
            if (useCollationAwareOp) {
                return sbe::makeE<sbe::EVariable>(*collatorSlot);
            }
            return nullptr;
        };

        return sbe::makeE<sbe::EPrimBinary>(sbeOp, std::move(lhs), std::move(rhs), collationExpr());
    }

    return sbe::makeE<sbe::EPrimBinary>(sbeOp, std::move(lhs), std::move(rhs));
}

std::unique_ptr<sbe::EExpression> SBEExpressionLowering::transport(
    const NaryOp& op, std::vector<std::unique_ptr<sbe::EExpression>> args) {

    sbe::EPrimNary::Op sbeOp;
    switch (op.op()) {
        case Operations::And:
            sbeOp = sbe::EPrimNary::logicAnd;
            break;
        case Operations::Or:
            sbeOp = sbe::EPrimNary::logicOr;
            break;
        case Operations::Add:
            sbeOp = sbe::EPrimNary::add;
            break;
        case Operations::Mult:
            sbeOp = sbe::EPrimNary::mul;
            break;
        default:
            MONGO_UNREACHABLE;
    }
    return sbe::makeE<sbe::EPrimNary>(sbeOp, std::move(args));
}

std::unique_ptr<sbe::EExpression> SBEExpressionLowering::transport(
    const UnaryOp& op, std::unique_ptr<sbe::EExpression> arg) {

    sbe::EPrimUnary::Op sbeOp = getEPrimUnaryOp(op.op());

    return sbe::makeE<sbe::EPrimUnary>(sbeOp, std::move(arg));
}

std::unique_ptr<sbe::EExpression> SBEExpressionLowering::transport(
    const If&,
    std::unique_ptr<sbe::EExpression> cond,
    std::unique_ptr<sbe::EExpression> thenBranch,
    std::unique_ptr<sbe::EExpression> elseBranch) {
    return sbe::makeE<sbe::EIf>(std::move(cond), std::move(thenBranch), std::move(elseBranch));
}

std::unique_ptr<sbe::EExpression> SBEExpressionLowering::transport(
    const Switch& fn, std::vector<std::unique_ptr<sbe::EExpression>> args) {
    return sbe::makeE<sbe::ESwitch>(std::move(args));
}

std::unique_ptr<sbe::EExpression> makeFillEmptyNull(std::unique_ptr<sbe::EExpression> e) {
    return sbe::makeE<sbe::EPrimBinary>(sbe::EPrimBinary::fillEmpty,
                                        std::move(e),
                                        sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Null, 0));
}

std::unique_ptr<sbe::EExpression> SBEExpressionLowering::transport(
    const FunctionCall& fn, std::vector<std::unique_ptr<sbe::EExpression>> args) {
    auto name = fn.name();

    if (name == "fail") {
        uassert(6250200, "Invalid number of arguments to fail()", fn.nodes().size() == 2);
        const auto* codeConstPtr = fn.nodes().at(0).cast<Constant>();
        const auto* messageConstPtr = fn.nodes().at(1).cast<Constant>();

        uassert(6250201,
                "First argument to fail() must be a 32-bit integer constant",
                codeConstPtr != nullptr && codeConstPtr->isValueInt32());
        uassert(6250202,
                "Second argument to fail() must be a string constant",
                messageConstPtr != nullptr && messageConstPtr->isString());

        return sbe::makeE<sbe::EFail>(static_cast<ErrorCodes::Error>(codeConstPtr->getValueInt32()),
                                      messageConstPtr->getString());
    }

    if (name == "convert") {
        uassert(6250203, "Invalid number of arguments to convert()", fn.nodes().size() == 2);
        const auto* constPtr = fn.nodes().at(1).cast<Constant>();

        uassert(6250204,
                "Second argument to convert() must be a 32-bit integer constant",
                constPtr != nullptr && constPtr->isValueInt32());
        int32_t constVal = constPtr->getValueInt32();

        uassert(6250205,
                "Second argument to convert() must be a numeric type tag",
                constVal >= static_cast<int32_t>(std::numeric_limits<uint8_t>::min()) &&
                    constVal <= static_cast<int32_t>(std::numeric_limits<uint8_t>::max()) &&
                    sbe::value::isNumber(static_cast<sbe::value::TypeTags>(constVal)));

        return sbe::makeE<sbe::ENumericConvert>(std::move(args.at(0)),
                                                static_cast<sbe::value::TypeTags>(constVal));
    }

    if (name == "typeMatch") {
        uassert(6250206, "Invalid number of arguments to typeMatch()", fn.nodes().size() == 2);
        const auto* constPtr = fn.nodes().at(1).cast<Constant>();

        uassert(6250207,
                "Second argument to typeMatch() must be a 32-bit integer constant",
                constPtr != nullptr && constPtr->isValueInt32());

        return sbe::makeE<sbe::EFunction>(
            "typeMatch",
            sbe::makeEs(std::move(args.at(0)),
                        sbe::makeE<sbe::EConstant>(
                            sbe::value::TypeTags::NumberInt32,
                            sbe::value::bitcastFrom<int32_t>(constPtr->getValueInt32()))));
    }

    if (name == kParameterFunctionName) {
        uassert(8128700, "Invalid number of arguments to getParam()", fn.nodes().size() == 2);
        const auto* paramId = fn.nodes().at(0).cast<Constant>();

        uassert(10367400,
                "First argument to getParam() must be a 32-bit integer constant",
                paramId != nullptr && paramId->isValueInt32());

        auto paramIdVal = paramId->getValueInt32();

        auto slotId = [&]() {
            auto it = _inputParamToSlotMap.find(paramIdVal);
            if (it != _inputParamToSlotMap.end()) {
                // This input parameter id has already been tied to a particular runtime environment
                // slot. Just return that slot to the caller. This can happen if a query planning
                // optimization or rewrite chose to clone one of the input expressions from the
                // user's query.
                return it->second;
            }

            auto newSlotId = _providedSlots.registerSlot(
                sbe::value::TypeTags::Nothing, 0, false /* owned */, &_slotIdGenerator);
            _inputParamToSlotMap.emplace(paramIdVal, newSlotId);
            return newSlotId;
        }();

        return sbe::makeE<sbe::EVariable>(slotId);
    }

    // TODO - this is an open question how to do the name mappings.
    if (name == "$sum") {
        name = "sum";
    } else if (name == "$first") {
        name = "first";
    } else if (name == "$last") {
        name = "last";
    } else if (name == "$min") {
        name = "min";
    } else if (name == "$max") {
        name = "max";
    } else if (name == "$addToSet") {
        name = "addToSet";
    } else if (name == "$push") {
        name = "addToArray";
    }

    return sbe::makeE<sbe::EFunction>(name, toInlinedVector(std::move(args)));
}
}  // namespace mongo::stage_builder::abt_lower
