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

#pragma once

#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/expressions/runtime_environment.h"
#include "mongo/db/query/stage_builder/sbe/abt/reference_tracker.h"
#include "mongo/db/query/stage_builder/sbe/abt_lower_defs.h"

namespace mongo::stage_builder::abt_lower {

using namespace abt;

static constexpr auto kParameterFunctionName = "getParam";

class VarResolver {
public:
    using LowerFuncT = std::function<std::unique_ptr<sbe::EExpression>(const ProjectionName&)>;

    VarResolver(SlotVarMap& slotMap) : _slotMap(&slotMap) {}

    template <typename FuncT>
    VarResolver(FuncT lowerFn) : _lowerFn(std::move(lowerFn)) {}

    template <typename FuncT>
    VarResolver(SlotVarMap& slotMap, FuncT lowerFn)
        : _slotMap(&slotMap), _lowerFn(std::move(lowerFn)) {}

    std::unique_ptr<sbe::EExpression> operator()(const ProjectionName& name) const;

private:
    SlotVarMap* _slotMap = nullptr;
    LowerFuncT _lowerFn;
};

class SBEExpressionLowering {
public:
    SBEExpressionLowering(const VariableEnvironment& env,
                          VarResolver vr,
                          sbe::SlotsProvider& providedSlots,
                          sbe::value::SlotIdGenerator& ids,
                          sbe::InputParamToSlotMap& inputParamToSlotMap,
                          sbe::value::FrameIdGenerator* frameIdGenerator = nullptr)
        : _env(env),
          _varResolver(vr),
          _providedSlots(providedSlots),
          _slotIdGenerator(ids),
          _inputParamToSlotMap(inputParamToSlotMap),
          _frameIdGenerator(frameIdGenerator) {}

    // The default noop transport.
    template <typename T, typename... Ts>
    std::unique_ptr<sbe::EExpression> transport(const T&, Ts&&...) {
        uasserted(6624237,
                  "ABT expression lowering encountered operator which cannot be directly lowered "
                  "to an SBE expression.");
        return nullptr;
    }

    std::unique_ptr<sbe::EExpression> transport(const Constant&);
    std::unique_ptr<sbe::EExpression> transport(const Variable& var);
    std::unique_ptr<sbe::EExpression> transport(const Source&);
    std::unique_ptr<sbe::EExpression> transport(const BinaryOp& op,
                                                std::unique_ptr<sbe::EExpression> lhs,
                                                std::unique_ptr<sbe::EExpression> rhs);
    std::unique_ptr<sbe::EExpression> transport(
        const NaryOp& op, std::vector<std::unique_ptr<sbe::EExpression>> args);
    std::unique_ptr<sbe::EExpression> transport(const UnaryOp& op,
                                                std::unique_ptr<sbe::EExpression> arg);
    std::unique_ptr<sbe::EExpression> transport(const If&,
                                                std::unique_ptr<sbe::EExpression> cond,
                                                std::unique_ptr<sbe::EExpression> thenBranch,
                                                std::unique_ptr<sbe::EExpression> elseBranch);
    std::unique_ptr<sbe::EExpression> transport(
        const Switch& fn, std::vector<std::unique_ptr<sbe::EExpression>> args);

    void prepare(const Let& let);
    std::unique_ptr<sbe::EExpression> transport(const Let& let,
                                                std::unique_ptr<sbe::EExpression> bind,
                                                std::unique_ptr<sbe::EExpression> in);
    void prepare(const MultiLet& multiLet);
    std::unique_ptr<sbe::EExpression> transport(
        const MultiLet& multiLet, std::vector<std::unique_ptr<sbe::EExpression>> args);
    void prepare(const LambdaAbstraction& lam);
    std::unique_ptr<sbe::EExpression> transport(const LambdaAbstraction& lam,
                                                std::unique_ptr<sbe::EExpression> body);
    std::unique_ptr<sbe::EExpression> transport(const LambdaApplication&,
                                                std::unique_ptr<sbe::EExpression> lam,
                                                std::unique_ptr<sbe::EExpression> arg);
    std::unique_ptr<sbe::EExpression> transport(
        const FunctionCall& fn, std::vector<std::unique_ptr<sbe::EExpression>> args);

    std::unique_ptr<sbe::EExpression> optimize(const ABT& n);

private:
    sbe::FrameId generateFrameId() {
        if (_frameIdGenerator) {
            return _frameIdGenerator->generate();
        } else {
            return _localFrameIdGenerator.generate();
        }
    }

    const VariableEnvironment& _env;
    VarResolver _varResolver;
    sbe::SlotsProvider& _providedSlots;
    sbe::value::SlotIdGenerator& _slotIdGenerator;

    // Map to record newly allocated slots and the parameter ids they were generated from.
    // For more details see PlanStageStaticData::inputParamToSlotMap
    sbe::InputParamToSlotMap& _inputParamToSlotMap;

    // If '_frameIdGenerator' is not null then we use it to generate frame IDs, otherwise we
    // use '_localFrameIdGenerator' to generate frame IDs.
    sbe::value::FrameIdGenerator* const _frameIdGenerator;
    sbe::value::FrameIdGenerator _localFrameIdGenerator{100};

    stdx::unordered_map<const Let*, sbe::FrameId> _letMap;
    stdx::unordered_map<const MultiLet*,
                        std::pair<sbe::FrameId, abt::ProjectionNameMap<sbe::value::SlotId>>>
        _multiLetMap;
    stdx::unordered_map<const LambdaAbstraction*, sbe::FrameId> _lambdaMap;
};

inline sbe::EPrimUnary::Op getEPrimUnaryOp(Operations op) {
    switch (op) {
        case Operations::Neg:
            return sbe::EPrimUnary::negate;
        case Operations::Not:
            return sbe::EPrimUnary::logicNot;
        default:
            MONGO_UNREACHABLE;
    }
}

inline sbe::EPrimBinary::Op getEPrimBinaryOp(Operations op) {
    switch (op) {
        case Operations::Eq:
            return sbe::EPrimBinary::eq;
        case Operations::Neq:
            return sbe::EPrimBinary::neq;
        case Operations::Gt:
            return sbe::EPrimBinary::greater;
        case Operations::Gte:
            return sbe::EPrimBinary::greaterEq;
        case Operations::Lt:
            return sbe::EPrimBinary::less;
        case Operations::Lte:
            return sbe::EPrimBinary::lessEq;
        case Operations::Add:
            return sbe::EPrimBinary::add;
        case Operations::Sub:
            return sbe::EPrimBinary::sub;
        case Operations::FillEmpty:
            return sbe::EPrimBinary::fillEmpty;
        case Operations::And:
            return sbe::EPrimBinary::logicAnd;
        case Operations::Or:
            return sbe::EPrimBinary::logicOr;
        case Operations::Cmp3w:
            return sbe::EPrimBinary::cmp3w;
        case Operations::Div:
            return sbe::EPrimBinary::div;
        case Operations::Mult:
            return sbe::EPrimBinary::mul;
        default:
            MONGO_UNREACHABLE;
    }
}
}  // namespace mongo::stage_builder::abt_lower
