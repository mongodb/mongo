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

#include "mongo/db/query/sbe_stage_builder_sbexpr.h"

#include <charconv>

#include "mongo/db/exec/sbe/abt/abt_lower.h"
#include "mongo/db/query/optimizer/reference_tracker.h"
#include "mongo/db/query/optimizer/rewrites/path_lower.h"
#include "mongo/db/query/sbe_stage_builder.h"
#include "mongo/db/query/sbe_stage_builder_abt_holder_impl.h"
#include "mongo/db/query/sbe_stage_builder_const_eval.h"
#include "mongo/db/query/sbe_stage_builder_type_checker.h"

namespace mongo::stage_builder {
using EExpr = std::unique_ptr<sbe::EExpression>;

optimizer::ProjectionName getABTVariableName(sbe::value::SlotId slot) {
    constexpr StringData prefix = "__s"_sd;
    str::stream varName;
    varName << prefix << slot;
    return optimizer::ProjectionName{std::string(varName)};
}

optimizer::ProjectionName getABTLocalVariableName(sbe::FrameId frameId, sbe::value::SlotId slotId) {
    constexpr StringData prefix = "__l"_sd;
    str::stream varName;
    varName << prefix << frameId << "_" << slotId;
    return optimizer::ProjectionName{std::string(varName)};
}

boost::optional<sbe::value::SlotId> getSbeVariableInfo(const optimizer::ProjectionName& var) {
    constexpr StringData prefix = "__s"_sd;
    StringData name = var.value();

    if (name.startsWith(prefix)) {
        const char* ptr = name.rawData() + prefix.size();
        const char* endPtr = name.rawData() + name.size();

        sbe::value::SlotId slotId;
        auto fromCharsResult = std::from_chars(ptr, endPtr, slotId);

        if (fromCharsResult.ec == std::errc{} && fromCharsResult.ptr == endPtr) {
            return slotId;
        }
    }

    return boost::none;
}

boost::optional<std::pair<sbe::FrameId, sbe::value::SlotId>> getSbeLocalVariableInfo(
    const optimizer::ProjectionName& var) {
    constexpr StringData prefix = "__l"_sd;
    StringData name = var.value();

    if (name.startsWith(prefix)) {
        const char* ptr = name.rawData() + prefix.size();
        const char* endPtr = name.rawData() + name.size();

        sbe::FrameId frameId;
        auto fromCharsResult = std::from_chars(ptr, endPtr, frameId);

        if (fromCharsResult.ec == std::errc{}) {
            ptr = fromCharsResult.ptr;
            if (endPtr - ptr >= 2 && *ptr == '_') {
                ++ptr;

                sbe::value::SlotId slotId;
                fromCharsResult = std::from_chars(ptr, endPtr, slotId);

                if (fromCharsResult.ec == std::errc{} && fromCharsResult.ptr == endPtr) {
                    return std::pair(frameId, slotId);
                }
            }
        }
    }

    return boost::none;
}

optimizer::ABT makeABTVariable(sbe::value::SlotId slotId) {
    return optimizer::make<optimizer::Variable>(getABTVariableName(slotId));
}

optimizer::ABT makeABTLocalVariable(sbe::FrameId frameId, sbe::value::SlotId slotId) {
    return optimizer::make<optimizer::Variable>(getABTLocalVariableName(frameId, slotId));
}

optimizer::ABT makeVariable(optimizer::ProjectionName var) {
    return optimizer::make<optimizer::Variable>(std::move(var));
}

VariableTypes buildVariableTypes(const PlanStageSlots& outputs) {
    VariableTypes varTypes;
    for (const TypedSlot& slot : outputs.getAllSlotsInOrder()) {
        varTypes.emplace(getABTVariableName(slot.slotId), slot.typeSignature);
    }
    return varTypes;
}

bool hasBlockOutput(const PlanStageSlots& outputs) {
    for (const TypedSlot& slot : outputs.getAllSlotsInOrder()) {
        if (TypeSignature::kBlockType.isSubset(slot.typeSignature) ||
            TypeSignature::kCellType.isSubset(slot.typeSignature)) {
            return true;
        }
    }
    return false;
}

TypeSignature constantFold(optimizer::VariableEnvironment& env,
                           optimizer::ABT& abt,
                           StageBuilderState& state,
                           const VariableTypes* slotInfo) {
    auto& runtimeEnv = *state.env;

    // Do not use descriptive names here.
    auto prefixId = optimizer::PrefixId::create(false /*useDescriptiveNames*/);
    // Convert paths into ABT expressions.
    optimizer::EvalPathLowering pathLower{prefixId, env};
    pathLower.optimize(abt);

    const CollatorInterface* collator = nullptr;
    boost::optional<sbe::value::SlotId> collatorSlot = state.getCollatorSlot();
    if (collatorSlot) {
        auto [collatorTag, collatorValue] = runtimeEnv.getAccessor(*collatorSlot)->getViewOfValue();
        tassert(7158700,
                "Not a collator in collatorSlot",
                collatorTag == sbe::value::TypeTags::collator);
        collator = sbe::value::bitcastTo<const CollatorInterface*>(collatorValue);
    }

    TypeSignature signature = TypeSignature::kAnyScalarType;
    bool modified = false;
    do {
        // Run the constant folding to eliminate lambda applications as they are not directly
        // supported by the SBE VM.
        ExpressionConstEval constEval{env, collator};

        constEval.optimize(abt);

        TypeChecker typeChecker;
        if (slotInfo) {
            for (const auto& var : *slotInfo) {
                typeChecker.bind(var.first, var.second);
            }
        }
        signature = typeChecker.typeCheck(abt);

        modified = typeChecker.modified();
        if (modified) {
            env.rebuild(abt);
        }
    } while (modified);

    return signature;
}

TypeSignature constantFold(optimizer::ABT& abt,
                           StageBuilderState& state,
                           const VariableTypes* slotInfo) {
    auto env = optimizer::VariableEnvironment::build(abt);

    return constantFold(env, abt, state, slotInfo);
}

TypedExpression abtToExpr(optimizer::ABT& abt,
                          StageBuilderState& state,
                          const VariableTypes* slotInfo) {
    auto env = optimizer::VariableEnvironment::build(abt);

    TypeSignature signature = constantFold(env, abt, state, slotInfo);

    auto& runtimeEnv = *state.env;

    auto varResolver = optimizer::VarResolver([](const optimizer::ProjectionName& var) {
        if (auto slotId = getSbeVariableInfo(var)) {
            return sbe::makeE<sbe::EVariable>(*slotId);
        }

        if (auto localVarInfo = getSbeLocalVariableInfo(var)) {
            auto [frameId, slotId] = *localVarInfo;
            return sbe::makeE<sbe::EVariable>(frameId, slotId);
        }

        return EExpr{};
    });

    // And finally convert to the SBE expression.
    sbe::value::SlotIdGenerator ids;
    auto staticData = std::make_unique<stage_builder::PlanStageStaticData>();
    optimizer::SBEExpressionLowering exprLower{
        env,
        std::move(varResolver),
        runtimeEnv,
        ids,
        staticData->inputParamToSlotMap,
        nullptr /*metadata*/,
        nullptr /*nodeProps*/,
        // SBE stage builders assume that binary comparison operations in ABT are type bracketed and
        // must specify this to the class responsible for lowering to SBE.
        optimizer::ComparisonOpSemantics::kTypeBracketing};
    return {exprLower.optimize(abt), signature};
}

SbVar::SbVar(const optimizer::ProjectionName& name) {
    if (auto slotId = getSbeVariableInfo(name)) {
        _slotId = *slotId;
        return;
    }

    if (auto localVarInfo = getSbeLocalVariableInfo(name)) {
        auto [frameId, slotId] = *localVarInfo;
        _frameId = frameId;
        _slotId = slotId;
        return;
    }

    tasserted(7654321, str::stream() << "Unable to decode variable info for: " << name.value());
}

SbExpr::SbExpr(const abt::HolderPtr& a) : _storage(abt::wrap(a->_value)) {}

TypedExpression SbExpr::extractExpr(StageBuilderState& state) {
    if (hasSlot()) {
        auto slotId = get<sbe::value::SlotId>(_storage);
        return {sbe::makeE<sbe::EVariable>(slotId), TypeSignature::kAnyScalarType};
    }

    if (holds_alternative<LocalVarInfo>(_storage)) {
        auto [frameId, slotId] = get<LocalVarInfo>(_storage);
        return {sbe::makeE<sbe::EVariable>(frameId, slotId), TypeSignature::kAnyScalarType};
    }

    if (holds_alternative<abt::HolderPtr>(_storage)) {
        return abtToExpr(get<abt::HolderPtr>(_storage)->_value, state);
    }

    if (holds_alternative<bool>(_storage)) {
        return {EExpr{}, TypeSignature::kAnyScalarType};
    }

    return {std::move(get<EExpr>(_storage)), TypeSignature::kAnyScalarType};
}

TypedExpression SbExpr::getExpr(StageBuilderState& state) const {
    return clone().extractExpr(state);
}

abt::HolderPtr SbExpr::extractABT() {
    if (hasSlot()) {
        auto slotId = get<sbe::value::SlotId>(_storage);
        return abt::wrap(makeABTVariable(slotId));
    }

    if (holds_alternative<LocalVarInfo>(_storage)) {
        auto [frameId, slotId] = get<LocalVarInfo>(_storage);
        return abt::wrap(makeABTLocalVariable(frameId, slotId));
    }

    tassert(6950800,
            "Unexpected: extractABT() method invoked on an EExpression object",
            holds_alternative<abt::HolderPtr>(_storage));

    return std::move(get<abt::HolderPtr>(_storage));
}

void SbExpr::set(sbe::FrameId frameId, sbe::value::SlotId slotId) {
    auto frameIdInt32 = representAs<int32_t>(frameId);
    auto slotIdInt32 = representAs<int32_t>(slotId);

    if (frameIdInt32 && slotIdInt32) {
        _storage = std::pair(*frameIdInt32, *slotIdInt32);
    } else {
        _storage = abt::wrap(makeVariable(getABTLocalVariableName(frameId, slotId)));
    }
}
}  // namespace mongo::stage_builder
