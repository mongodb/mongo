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

#include "mongo/db/query/stage_builder/sbe/sbexpr.h"

#include "mongo/db/query/stage_builder/sbe/abt_lower.h"
#include "mongo/db/query/stage_builder/sbe/builder.h"
#include "mongo/db/query/stage_builder/sbe/expression_const_eval.h"
#include "mongo/db/query/stage_builder/sbe/type_checker.h"
#include "mongo/db/query/stage_builder/sbe/value_lifetime.h"

#include <charconv>

namespace mongo::stage_builder {
using SlotId = sbe::value::SlotId;
using FrameId = sbe::FrameId;

abt::ProjectionName SbBase::makeProjectionName(SlotId slotId) {
    constexpr StringData prefix = "__s"_sd;
    str::stream varName;
    varName << prefix << slotId;
    return abt::ProjectionName{std::string(varName)};
}

abt::ProjectionName SbBase::makeProjectionName(FrameId frameId, SlotId slotId) {
    constexpr StringData prefix = "__l"_sd;
    str::stream varName;
    varName << prefix << frameId << "_" << slotId;
    return abt::ProjectionName{std::string(varName)};
}

namespace {
boost::optional<SlotId> decodeSlotFromProjectionName(const abt::ProjectionName& var) {
    constexpr StringData prefix = "__s"_sd;
    StringData name = var.value();

    if (name.starts_with(prefix)) {
        const char* ptr = name.data() + prefix.size();
        const char* endPtr = name.data() + name.size();

        SlotId slotId;
        auto fromCharsResult = std::from_chars(ptr, endPtr, slotId);

        if (fromCharsResult.ec == std::errc{} && fromCharsResult.ptr == endPtr) {
            return slotId;
        }
    }

    return boost::none;
}

boost::optional<std::pair<FrameId, SlotId>> decodeLocalVarFromProjectionName(
    const abt::ProjectionName& var) {
    constexpr StringData prefix = "__l"_sd;
    StringData name = var.value();

    if (name.starts_with(prefix)) {
        const char* ptr = name.data() + prefix.size();
        const char* endPtr = name.data() + name.size();

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
}  // namespace

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

TypeSignature constantFold(abt::ABT& abt, StageBuilderState& state, const VariableTypes* slotInfo) {
    auto& runtimeEnv = *state.env;

    // Do not use descriptive names here.
    auto prefixId = PrefixId::create(false /*useDescriptiveNames*/);

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

    ValueLifetime{}.validate(abt);

    return signature;
}

boost::optional<SbSlot> SbSlot::fromProjectionName(const abt::ProjectionName& var) {
    boost::optional<SlotId> slotId = decodeSlotFromProjectionName(var);
    return slotId ? boost::make_optional(SbSlot{*slotId}) : boost::none;
}

boost::optional<SbLocalVar> SbLocalVar::fromProjectionName(const abt::ProjectionName& var) {
    if (auto localVarInfo = decodeLocalVarFromProjectionName(var)) {
        auto [frameId, slotId] = *localVarInfo;
        return SbLocalVar{frameId, slotId};
    }
    return boost::none;
}

boost::optional<SbVar> SbVar::fromProjectionName(const abt::ProjectionName& var) {
    if (auto slotId = decodeSlotFromProjectionName(var)) {
        return SbVar{*slotId};
    }
    if (auto localVarInfo = decodeLocalVarFromProjectionName(var)) {
        auto [frameId, slotId] = *localVarInfo;
        return SbVar{frameId, slotId};
    }
    return boost::none;
}

SbExpr::SbExpr(const abt::ABT& a, boost::optional<TypeSignature> typeSig) {
    _storage = Abt{a};
    _typeSig = typeSig;
}

SbExpr::SbExpr(abt::ABT&& a, boost::optional<TypeSignature> typeSig) noexcept
    : SbExpr(Abt{std::move(a)}, typeSig) {}

SbExpr::SbExpr(Abt a, boost::optional<TypeSignature> typeSig) noexcept {
    _storage = std::move(a);
    _typeSig = typeSig;
}

SbExpr::SbExpr(OptimizedAbt a, boost::optional<TypeSignature> typeSig) noexcept {
    _storage = std::move(a);
    _typeSig = typeSig;
}

SbExpr& SbExpr::operator=(const abt::ABT& a) {
    _storage = Abt{a};
    _typeSig.reset();
    return *this;
}

SbExpr& SbExpr::operator=(abt::ABT&& a) noexcept {
    *this = Abt{std::move(a)};
    return *this;
}

SbExpr& SbExpr::operator=(Abt a) noexcept {
    _storage = std::move(a);
    _typeSig.reset();
    return *this;
}

SbExpr& SbExpr::operator=(OptimizedAbt a) noexcept {
    _storage = std::move(a);
    _typeSig.reset();
    return *this;
}

std::unique_ptr<sbe::EExpression> SbExpr::lower(StageBuilderState& state,
                                                const VariableTypes* slotInfo) {
    // Optimize this expression (unless it's marked as "finished optimizing").
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
        return nullptr;
    }

    const auto& abt = getAbtInternal();
    auto env = abt::VariableEnvironment::build(abt);
    auto& runtimeEnv = *state.env;

    auto varResolver = abt_lower::VarResolver([](const abt::ProjectionName& name) {
        if (auto var = SbVar::fromProjectionName(name)) {
            if (var->isSlotExpr()) {
                return sbe::makeE<sbe::EVariable>(var->toSlot().getId());
            } else {
                auto l = var->toLocalVar();
                return sbe::makeE<sbe::EVariable>(l.getFrameId(), l.getSlotId());
            }
        }

        return std::unique_ptr<sbe::EExpression>{};
    });

    // Invoke 'SBEExpressionLowering' to lower the ABT to SBE.
    auto staticData = std::make_unique<stage_builder::PlanStageStaticData>();
    abt_lower::SBEExpressionLowering exprLower{env,
                                               std::move(varResolver),
                                               runtimeEnv,
                                               *state.slotIdGenerator,
                                               staticData->inputParamToSlotMap,
                                               state.frameIdGenerator};

    return exprLower.optimize(abt);
}

SbExpr SbExpr::clone() const {
    if (holds_alternative<SlotId>(_storage)) {
        return SbExpr{get<SlotId>(_storage), _typeSig.get()};
    }
    if (holds_alternative<LocalVarInfo>(_storage)) {
        return SbExpr{get<LocalVarInfo>(_storage), _typeSig.get()};
    }
    if (holds_alternative<Abt>(_storage)) {
        return SbExpr{Abt{getAbtInternal()}, _typeSig.get()};
    }
    if (holds_alternative<OptimizedAbt>(_storage)) {
        return SbExpr{OptimizedAbt{getAbtInternal()}, _typeSig.get()};
    }

    return SbExpr{};
}

bool SbExpr::isConstantExpr() const {
    return holdsAbtInternal() && getAbtInternal().is<abt::Constant>();
}

bool SbExpr::isVarExpr() const {
    if (holds_alternative<SlotId>(_storage) || holds_alternative<LocalVarInfo>(_storage)) {
        return true;
    }
    if (holdsAbtInternal()) {
        if (auto* abtVar = getAbtInternal().cast<abt::Variable>()) {
            return SbVar::fromProjectionName(abtVar->name()).has_value();
        }
    }
    return false;
}

bool SbExpr::isSlotExpr() const {
    if (holds_alternative<SlotId>(_storage)) {
        return true;
    }
    if (holdsAbtInternal()) {
        if (auto* abtVar = getAbtInternal().cast<abt::Variable>()) {
            return SbSlot::fromProjectionName(abtVar->name()).has_value();
        }
    }
    return false;
}

bool SbExpr::isLocalVarExpr() const {
    if (holds_alternative<LocalVarInfo>(_storage)) {
        return true;
    }
    if (holdsAbtInternal()) {
        if (auto* abtVar = getAbtInternal().cast<abt::Variable>()) {
            return SbLocalVar::fromProjectionName(abtVar->name()).has_value();
        }
    }
    return false;
}

std::pair<sbe::value::TypeTags, sbe::value::Value> SbExpr::getConstantValue() const {
    tassert(8455801, "Expected SbExpr to be a constant expression", isConstantExpr());

    return getAbtInternal().cast<abt::Constant>()->get();
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
        if (auto* abtVar = getAbtInternal().cast<abt::Variable>()) {
            if (auto var = SbVar::fromProjectionName(abtVar->name())) {
                var->setTypeSignature(getTypeSignature());
                return *var;
            }
        }
    }

    tasserted(8455804, "Expected variable info decoding to succeed");
}

SbSlot SbExpr::toSlot() const {
    tassert(8455807, "Expected a slot variable expression", isSlotExpr());

    if (holds_alternative<SlotId>(_storage)) {
        auto slotId = get<SlotId>(_storage);
        return SbSlot{slotId, getTypeSignature()};
    }

    if (holdsAbtInternal()) {
        if (auto* abtVar = getAbtInternal().cast<abt::Variable>()) {
            if (auto slot = SbSlot::fromProjectionName(abtVar->name())) {
                slot->setTypeSignature(getTypeSignature());
                return *slot;
            }
        }
    }

    tasserted(8455808, "Expected variable info decoding to succeed");
}

SbLocalVar SbExpr::toLocalVar() const {
    tassert(8455811, "Expected a local variable expression", isLocalVarExpr());

    if (holds_alternative<LocalVarInfo>(_storage)) {
        auto [frameId, slotId] = get<LocalVarInfo>(_storage);
        return SbLocalVar{frameId, slotId, getTypeSignature()};
    }

    if (holdsAbtInternal()) {
        if (auto* abtVar = getAbtInternal().cast<abt::Variable>()) {
            if (auto l = SbLocalVar::fromProjectionName(abtVar->name())) {
                l->setTypeSignature(getTypeSignature());
                return *l;
            }
        }
    }

    tasserted(8455812, "Expected variable info decoding to succeed");
}

abt::ABT SbExpr::extractABT() {
    tassert(6950800, "Expected isNull() to be false", !isNull());

    if (!holdsAbtInternal()) {
        if (isVarExpr()) {
            // Handle the slot variable case and the local variable case.
            return abt::make<abt::Variable>(toVar().toProjectionName());
        } else if (isConstantExpr()) {
            // Handle the constant case.
            auto [tag, val] = getConstantValue();
            auto [copyTag, copyVal] = sbe::value::copyValue(tag, val);
            return abt::make<abt::Constant>(copyTag, copyVal);
        }
    }

    // If we reach here, then we know we have an ABT. Extract the ABT, set this SbExpr to the
    // null state, and then return the ABT.
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
        auto name = toSlot().toProjectionName();
        if (auto it = slotInfo->find(name); it != slotInfo->end()) {
            setTypeSignature(it->second);
        }
    } else if (holdsAbtInternal()) {
        // Do constant folding optimization and run the typechecker, and then store the type
        // returned by the typechecker into _typeSig and return.
        auto typeSig = constantFold(getAbtInternal(), state, slotInfo);
        setTypeSignature(typeSig);
    }
}

void SbExpr::setFinishedOptimizing() {
    if (!holds_alternative<OptimizedAbt>(_storage) && !isNull()) {
        // extractABT() may mutate '_typeSig', so we need to read '_typeSig' in advance.
        auto typeSig = _typeSig;

        // Call extractABT() to get the ABT, wrap it with 'OptimizedAbt' and store it in '_storage'.
        abt::ABT abt = extractABT();
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

    _storage = Abt{abt::make<abt::Variable>(l.toProjectionName())};
}
}  // namespace mongo::stage_builder
