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
using SlotId = sbe::value::SlotId;
using FrameId = sbe::FrameId;
using EExpr = std::unique_ptr<sbe::EExpression>;

optimizer::ProjectionName getABTVariableName(SbSlot ts) {
    constexpr StringData prefix = "__s"_sd;
    str::stream varName;
    varName << prefix << ts.slotId;
    return optimizer::ProjectionName{std::string(varName)};
}

optimizer::ProjectionName getABTLocalVariableName(FrameId frameId, SlotId slotId) {
    constexpr StringData prefix = "__l"_sd;
    str::stream varName;
    varName << prefix << frameId << "_" << slotId;
    return optimizer::ProjectionName{std::string(varName)};
}

boost::optional<SlotId> getSbeVariableInfo(const optimizer::ProjectionName& var) {
    constexpr StringData prefix = "__s"_sd;
    StringData name = var.value();

    if (name.startsWith(prefix)) {
        const char* ptr = name.rawData() + prefix.size();
        const char* endPtr = name.rawData() + name.size();

        SlotId slotId;
        auto fromCharsResult = std::from_chars(ptr, endPtr, slotId);

        if (fromCharsResult.ec == std::errc{} && fromCharsResult.ptr == endPtr) {
            return slotId;
        }
    }

    return boost::none;
}

boost::optional<std::pair<FrameId, SlotId>> getSbeLocalVariableInfo(
    const optimizer::ProjectionName& var) {
    constexpr StringData prefix = "__l"_sd;
    StringData name = var.value();

    if (name.startsWith(prefix)) {
        const char* ptr = name.rawData() + prefix.size();
        const char* endPtr = name.rawData() + name.size();

        FrameId frameId;
        auto fromCharsResult = std::from_chars(ptr, endPtr, frameId);

        if (fromCharsResult.ec == std::errc{}) {
            ptr = fromCharsResult.ptr;
            if (endPtr - ptr >= 2 && *ptr == '_') {
                ++ptr;

                SlotId slotId;
                fromCharsResult = std::from_chars(ptr, endPtr, slotId);

                if (fromCharsResult.ec == std::errc{} && fromCharsResult.ptr == endPtr) {
                    return std::pair(frameId, slotId);
                }
            }
        }
    }

    return boost::none;
}

optimizer::ABT makeABTVariable(SbSlot ts) {
    return optimizer::make<optimizer::Variable>(getABTVariableName(ts));
}

optimizer::ABT makeABTLocalVariable(FrameId frameId, SlotId slotId) {
    return optimizer::make<optimizer::Variable>(getABTLocalVariableName(frameId, slotId));
}

optimizer::ABT makeVariable(optimizer::ProjectionName var) {
    return optimizer::make<optimizer::Variable>(std::move(var));
}

void addVariableTypesHelper(VariableTypes& varTypes, const PlanStageSlots& outputs) {
    auto slots = outputs.getAllSlotsInOrder();
    addVariableTypesHelper(varTypes, slots.begin(), slots.end());
}

VariableTypes excludeTypes(VariableTypes varTypes, TypeSignature typesToExclude) {
    for (auto& [_, typeSig] : varTypes) {
        typeSig = typeSig.exclude(typesToExclude);
    }
    return varTypes;
}

TypeSignature constantFold(optimizer::ABT& abt,
                           StageBuilderState& state,
                           const VariableTypes* slotInfo) {
    auto& runtimeEnv = *state.env;

    // Do not use descriptive names here.
    auto prefixId = optimizer::PrefixId::create(false /*useDescriptiveNames*/);

    // Convert paths into ABT expressions.
    optimizer::EvalPathLowering pathLower{prefixId};
    pathLower.optimize(abt);

    const CollatorInterface* collator = nullptr;
    boost::optional<SlotId> collatorSlot = state.getCollatorSlot();
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
        ExpressionConstEval constEval{collator};

        constEval.optimize(abt);

        TypeChecker typeChecker;
        if (slotInfo) {
            for (const auto& var : *slotInfo) {
                typeChecker.bind(var.first, var.second);
            }
        }
        signature = typeChecker.typeCheck(abt);

        modified = typeChecker.modified();
    } while (modified);

    return signature;
}

SbVar::SbVar(const optimizer::ProjectionName& name, boost::optional<TypeSignature> typeSig)
    : _typeSig(typeSig) {
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

    tasserted(8455800, str::stream() << "Unable to decode variable info for: " << name.value());
}

SbExpr::SbExpr(const abt::HolderPtr& a, boost::optional<TypeSignature> typeSig) {
    if (a) {
        _storage = Abt{abt::wrap(a->_value)};
        _typeSig = typeSig;
    }
}

SbExpr::SbExpr(abt::HolderPtr&& a, boost::optional<TypeSignature> typeSig) noexcept
    : SbExpr(Abt{std::move(a)}, typeSig) {}

SbExpr::SbExpr(Abt a, boost::optional<TypeSignature> typeSig) noexcept {
    if (a.ptr) {
        _storage = std::move(a);
        _typeSig = typeSig;
    }
}

SbExpr::SbExpr(OptimizedAbt a, boost::optional<TypeSignature> typeSig) noexcept {
    if (a.ptr) {
        _storage = std::move(a);
        _typeSig = typeSig;
    }
}

SbExpr& SbExpr::operator=(const abt::HolderPtr& a) {
    if (a) {
        _storage = Abt{abt::wrap(a->_value)};
        _typeSig.reset();
    } else {
        reset();
    }
    return *this;
}

SbExpr& SbExpr::operator=(abt::HolderPtr&& a) noexcept {
    *this = Abt{std::move(a)};
    return *this;
}

SbExpr& SbExpr::operator=(Abt a) noexcept {
    if (a.ptr) {
        _storage = std::move(a);
        _typeSig.reset();
    } else {
        reset();
    }
    return *this;
}

SbExpr& SbExpr::operator=(OptimizedAbt a) noexcept {
    if (a.ptr) {
        _storage = std::move(a);
        _typeSig.reset();
    } else {
        reset();
    }
    return *this;
}

EExpr SbExpr::extractExpr(StageBuilderState& state, const VariableTypes* slotInfo) {
    optimize(state, slotInfo);

    if (holds_alternative<SlotId>(_storage)) {
        auto slotId = get<SlotId>(_storage);
        return sbe::makeE<sbe::EVariable>(slotId);
    }

    if (holds_alternative<LocalVarInfo>(_storage)) {
        auto [frameId, slotId] = get<LocalVarInfo>(_storage);
        return sbe::makeE<sbe::EVariable>(frameId, slotId);
    }

    if (holds_alternative<std::monostate>(_storage)) {
        return EExpr{};
    }

    if (holds_alternative<EExpr>(_storage)) {
        // Extract the EExpr, set this SbExpr to the null state, and return the EExpr.
        auto result = std::move(get<EExpr>(_storage));
        reset();
        return result;
    }

    auto& abt = getAbtInternal()->_value;
    auto env = optimizer::VariableEnvironment::build(abt);
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

    // Invoke 'SBEExpressionLowering' to lower the ABT to SBE.
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

    return exprLower.optimize(abt);
}

SbExpr SbExpr::clone() const {
    if (holds_alternative<SlotId>(_storage)) {
        return {get<SlotId>(_storage), _typeSig.get()};
    }
    if (holds_alternative<LocalVarInfo>(_storage)) {
        return {get<LocalVarInfo>(_storage), _typeSig.get()};
    }
    if (holds_alternative<Abt>(_storage)) {
        return {Abt{abt::wrap(getAbtInternal()->_value)}, _typeSig.get()};
    }
    if (holds_alternative<OptimizedAbt>(_storage)) {
        return {OptimizedAbt{abt::wrap(getAbtInternal()->_value)}, _typeSig.get()};
    }
    if (holds_alternative<EExpr>(_storage)) {
        const auto& expr = get<EExpr>(_storage);
        return {expr->clone(), _typeSig.get()};
    }

    return {};
}

bool SbExpr::isConstantExpr() const {
    if (holdsAbtInternal()) {
        return getAbtInternal()->_value.is<optimizer::Constant>();
    }
    if (holds_alternative<EExpr>(_storage)) {
        return get<EExpr>(_storage)->as<sbe::EConstant>() != nullptr;
    }
    return false;
}

bool SbExpr::isVarExpr() const {
    if (holds_alternative<SlotId>(_storage) || holds_alternative<LocalVarInfo>(_storage)) {
        return true;
    }
    if (holdsAbtInternal()) {
        return getAbtInternal()->_value.is<optimizer::Variable>();
    }
    if (holds_alternative<EExpr>(_storage)) {
        return get<EExpr>(_storage)->as<sbe::EVariable>() != nullptr;
    }
    return false;
}

bool SbExpr::isSlotExpr() const {
    if (holds_alternative<SlotId>(_storage)) {
        return true;
    }
    if (holds_alternative<LocalVarInfo>(_storage) || !isVarExpr()) {
        return false;
    }
    if (holdsAbtInternal()) {
        auto* var = getAbtInternal()->_value.cast<optimizer::Variable>();
        return getSbeVariableInfo(var->name()).has_value();
    }
    if (holds_alternative<EExpr>(_storage)) {
        auto& e = get<EExpr>(_storage);
        auto* var = e->as<sbe::EVariable>();
        return !var->getFrameId().has_value();
    }
    return false;
}

bool SbExpr::isLocalVarExpr() const {
    if (holds_alternative<LocalVarInfo>(_storage)) {
        return true;
    }
    if (holds_alternative<SlotId>(_storage) || !isVarExpr()) {
        return false;
    }
    if (holdsAbtInternal()) {
        auto* var = getAbtInternal()->_value.cast<optimizer::Variable>();
        return !getSbeVariableInfo(var->name()).has_value();
    }
    if (holds_alternative<EExpr>(_storage)) {
        auto& e = get<EExpr>(_storage);
        auto* var = e->as<sbe::EVariable>();
        return var->getFrameId().has_value();
    }
    return false;
}

std::pair<sbe::value::TypeTags, sbe::value::Value> SbExpr::getConstantValue() const {
    tassert(8455801, "Expected SbExpr to be a constant expression", isConstantExpr());

    if (holdsAbtInternal()) {
        return getAbtInternal()->_value.cast<optimizer::Constant>()->get();
    }

    tassert(8455802, "Expected an EExpression", holds_alternative<EExpr>(_storage));

    auto& e = get<EExpr>(_storage);
    return e->as<sbe::EConstant>()->getConstant();
}

SbVar SbExpr::toVar() const {
    tassert(8455803, "Expected SbExpr to be a variable expression", isVarExpr());

    if (holds_alternative<SlotId>(_storage)) {
        auto slotId = get<SlotId>(_storage);
        return SbVar{slotId, getTypeSignature()};
    }
    if (holds_alternative<LocalVarInfo>(_storage)) {
        auto [frameId, slotId] = get<LocalVarInfo>(_storage);
        return SbVar{frameId, slotId, getTypeSignature()};
    }

    if (holdsAbtInternal()) {
        auto* var = getAbtInternal()->_value.cast<optimizer::Variable>();
        auto& name = var->name();
        if (auto slotId = getSbeVariableInfo(name)) {
            return SbVar{*slotId, getTypeSignature()};
        }

        auto localVarInfo = getSbeLocalVariableInfo(name);
        tassert(8455804, "Expected variable info decoding to succeed", localVarInfo.has_value());

        auto [frameId, slotId] = *localVarInfo;
        return SbVar{frameId, slotId, getTypeSignature()};
    }

    tassert(8455805, "Expected an EExpression", holds_alternative<EExpr>(_storage));
    auto& e = get<EExpr>(_storage);
    auto* var = e->as<sbe::EVariable>();

    tassert(8455806, "Expected an EVariable", var != nullptr);

    if (!var->getFrameId().has_value()) {
        return SbVar{var->getSlotId(), getTypeSignature()};
    } else {
        return SbVar{*var->getFrameId(), var->getSlotId(), getTypeSignature()};
    }
}

SbSlot SbExpr::toSlot() const {
    tassert(8455807, "Expected a slot variable expression", isSlotExpr());

    if (holds_alternative<SlotId>(_storage)) {
        auto slotId = get<SlotId>(_storage);
        return SbSlot{slotId, getTypeSignature()};
    }

    if (holdsAbtInternal()) {
        auto* var = getAbtInternal()->_value.cast<optimizer::Variable>();
        auto slotId = var ? getSbeVariableInfo(var->name()) : boost::none;

        tassert(8455808, "Expected variable info decoding to succeed", slotId.has_value());

        return SbSlot{*slotId, getTypeSignature()};
    }

    tassert(8455809, "Expected EExpression", holds_alternative<EExpr>(_storage));
    auto& e = get<EExpr>(_storage);
    auto* var = e->as<sbe::EVariable>();

    tassert(8455810, "Expected EVariable without frameId", var && !var->getFrameId());

    return SbSlot{var->getSlotId(), getTypeSignature()};
}

SbLocalVar SbExpr::toLocalVar() const {
    tassert(8455811, "Expected a local variable expression", isLocalVarExpr());

    if (holds_alternative<LocalVarInfo>(_storage)) {
        auto [frameId, slotId] = get<LocalVarInfo>(_storage);
        return SbLocalVar{frameId, slotId, getTypeSignature()};
    }

    if (holdsAbtInternal()) {
        auto* var = getAbtInternal()->_value.cast<optimizer::Variable>();
        auto localVarInfo = getSbeLocalVariableInfo(var->name());
        tassert(8455812, "Expected variable info decoding to succeed", localVarInfo.has_value());

        auto [frameId, slotId] = *localVarInfo;
        return SbLocalVar{frameId, slotId, getTypeSignature()};
    }

    tassert(8455813, "Expected EExpression", holds_alternative<EExpr>(_storage));
    auto& e = get<EExpr>(_storage);
    auto* var = e->as<sbe::EVariable>();

    tassert(8455814, "Expected EVariable", var != nullptr);

    return SbLocalVar{*var->getFrameId(), var->getSlotId(), getTypeSignature()};
}

abt::HolderPtr SbExpr::extractABT() {
    tassert(6950800, "Expected canExtractABT() to be true", canExtractABT());

    if (!holdsAbtInternal()) {
        if (isSlotExpr()) {
            // Handle the slot variable case.
            return abt::wrap(makeABTVariable(toSlot()));
        } else if (isLocalVarExpr()) {
            // Handle the local variable case.
            auto var = toLocalVar();
            return abt::wrap(makeABTLocalVariable(var.getFrameId(), var.getSlotId()));
        } else if (isConstantExpr()) {
            // Handle the constant case.
            auto [tag, val] = getConstantValue();
            auto [copyTag, copyVal] = sbe::value::copyValue(tag, val);
            return abt::wrap(optimizer::make<optimizer::Constant>(copyTag, copyVal));
        }
    }

    // If we reach here, then we know we have an abt::Holder. Extract the Holder, set this
    // SbExpr to the null state, and then return the Holder.
    auto abtHolder = std::move(getAbtInternal());
    reset();
    return abtHolder;
}

void SbExpr::optimize(StageBuilderState& state, const VariableTypes* slotInfo) {
    if (isNull() || isFinishedOptimizing()) {
        // If this SbExpr is null or if it's marked as "finished optimizing", then do nothing
        // and return.
        return;
    }

    if (isConstantExpr()) {
        // If this is a constant expression, set this SbExpr's type signature to be equal to
        // the constant's type.
        auto [tag, _] = getConstantValue();
        setTypeSignature(stage_builder::getTypeSignature(tag));
    } else if (isSlotExpr() && slotInfo) {
        // If this is a slot variable and 'slotInfo' has a type signature for the slot, then set
        // this SbExpr's type signature to be equal to the slot's type signature.
        auto name = getABTVariableName(toSlot());
        if (auto it = slotInfo->find(name); it != slotInfo->end()) {
            setTypeSignature(it->second);
        }
    } else if (holdsAbtInternal()) {
        // Do constant folding optimization and run the typechecker, and then store the type
        // returned by the typechecker into _typeSig and return.
        auto typeSig = constantFold(getAbtInternal()->_value, state, slotInfo);
        setTypeSignature(typeSig);
    }
}

void SbExpr::setFinishedOptimizing() {
    if (!holds_alternative<OptimizedAbt>(_storage) && canExtractABT()) {
        // extractABT() may set '*this' to the null state (in which case '_typeSig' would be set
        // to boost::none), so we need to read '_typeSig' before calling extractABT().
        auto typeSig = _typeSig;

        // Call extractABT() to get the ABT, wrap it with 'OptimizedAbt' and store it in '_storage'.
        abt::HolderPtr abt = extractABT();
        _storage = OptimizedAbt{std::move(abt)};

        // Restore '_typeSig' in case it got clobbered by the call to extractABT().
        _typeSig = typeSig;
    }
}

void SbExpr::set(SbLocalVar l) {
    _typeSig = l.getTypeSignature();

    auto frameIdInt32 = representAs<int32_t>(l.getFrameId());
    auto slotIdInt32 = representAs<int32_t>(l.getSlotId());
    if (frameIdInt32 && slotIdInt32) {
        _storage = std::pair(*frameIdInt32, *slotIdInt32);
        return;
    }

    _storage = Abt{abt::wrap(makeVariable(getABTLocalVariableName(l.getFrameId(), l.getSlotId())))};
}
}  // namespace mongo::stage_builder
