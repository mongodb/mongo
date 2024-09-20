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

#pragma once

#include <vector>

#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/query/stage_builder/sbe/abt_holder_def.h"
#include "mongo/db/query/stage_builder/sbe/type_signature.h"

#include "mongo/db/exec/sbe/abt/abt_lower_defs.h"
#include "mongo/db/exec/sbe/abt/slots_provider.h"
#include "mongo/db/query/optimizer/node.h"  // IWYU pragma: keep
#include "mongo/db/query/optimizer/node_defs.h"
#include "mongo/db/query/optimizer/syntax/expr.h"
#include "mongo/db/query/optimizer/syntax/syntax.h"
#include "mongo/db/query/optimizer/utils/utils.h"

namespace mongo::stage_builder {

class PlanStageSlots;
struct StageBuilderState;
class SbSlot;
class SbLocalVar;
class SbVar;
class SbExpr;

optimizer::ProjectionName getABTVariableName(SbSlot s);

optimizer::ProjectionName getABTVariableName(sbe::value::SlotId slotId);

optimizer::ProjectionName getABTLocalVariableName(sbe::FrameId frameId, sbe::value::SlotId slotId);

boost::optional<sbe::value::SlotId> getSbeVariableInfo(const optimizer::ProjectionName& var);

boost::optional<std::pair<sbe::FrameId, sbe::value::SlotId>> getSbeLocalVariableInfo(
    const optimizer::ProjectionName& var);

optimizer::ABT makeABTVariable(SbSlot s);

optimizer::ABT makeABTVariable(sbe::value::SlotId slotId);

using VariableTypes = stdx::
    unordered_map<optimizer::ProjectionName, TypeSignature, optimizer::ProjectionName::Hasher>;

// Run constant folding on the provided ABT tree and return its type signature. If the type
// information for the visible slots is available in the slotInfo argument, it is used to perform a
// more precise type checking optimization. On return, the abt argument points to the modified tree.
TypeSignature constantFold(optimizer::ABT& abt,
                           StageBuilderState& state,
                           const VariableTypes* slotInfo = nullptr);

/**
 * The SbSlot struct is used to represent slot variables in the SBE stage builder. "SbSlot" is short
 * for "stage builder slot".
 */
class SbSlot {
public:
    using SlotId = sbe::value::SlotId;

    struct Less {
        bool operator()(const SbSlot& lhs, const SbSlot& rhs) const {
            return lhs.getId() < rhs.getId();
        }
    };

    struct EqualTo {
        bool operator()(const SbSlot& lhs, const SbSlot& rhs) const {
            return lhs.getId() == rhs.getId();
        }
    };

    SbSlot() = default;

    explicit SbSlot(SlotId slotId, boost::optional<TypeSignature> sig = boost::none)
        : slotId(slotId), typeSig(sig) {}

    bool isVarExpr() const {
        return true;
    }
    bool isSlotExpr() const {
        return true;
    }
    bool isLocalVarExpr() const {
        return false;
    }
    bool isConstantExpr() const {
        return false;
    }

    SbVar toVar() const;

    SlotId getId() const {
        return slotId;
    }
    void setId(SlotId s) {
        slotId = s;
    }

    boost::optional<TypeSignature> getTypeSignature() const {
        return typeSig.get();
    }
    void setTypeSignature(boost::optional<TypeSignature> sig) {
        typeSig = sig;
    }

private:
    SlotId slotId{0};
    OptTypeSignature typeSig;
};

/**
 * The SbSlotVector is SbExpr's equivalent of the 'sbe::value::SlotVector' type.
 */
using SbSlotVector = absl::InlinedVector<SbSlot, 2>;

/**
 * The SbLocalVar class is used to represent local variables in the SBE stage builder. "SbLocalVar"
 * is short for "stage builder local variable".
 */
class SbLocalVar {
public:
    using SlotId = sbe::value::SlotId;
    using FrameId = sbe::FrameId;

    SbLocalVar() = default;

    explicit SbLocalVar(FrameId frameId,
                        SlotId slotId,
                        boost::optional<TypeSignature> sig = boost::none)
        : _frameId(frameId), _slotId(slotId), _typeSig(sig) {}

    bool isVarExpr() const {
        return true;
    }
    bool isSlotExpr() const {
        return false;
    }
    bool isLocalVarExpr() const {
        return true;
    }
    bool isConstantExpr() const {
        return false;
    }

    SbVar toVar() const;

    FrameId getFrameId() const {
        return _frameId;
    }
    void setFrameId(FrameId f) {
        _frameId = f;
    }

    SlotId getSlotId() const {
        return _slotId;
    }
    void setSlotId(SlotId s) {
        _slotId = s;
    }

    boost::optional<TypeSignature> getTypeSignature() const {
        return _typeSig.get();
    }
    void setTypeSignature(boost::optional<TypeSignature> sig) {
        _typeSig = sig;
    }

private:
    FrameId _frameId{0};
    SlotId _slotId{0};
    OptTypeSignature _typeSig;
};

/**
 * The SbVar class is used to represent variables in the SBE stage builder. "SbVar" is short for
 * "stage builder variable". A given SbVar can represent either be a "slot variable" or a "local
 * variable".
 *
 * An SbVar can be constructed from an EVariable or ProjectionName, and likewise an SbVar can be
 * converted to EVariable or ProjectionName.
 */
class SbVar {
public:
    using SlotId = sbe::value::SlotId;
    using FrameId = sbe::FrameId;

    explicit SbVar(SlotId slotId, boost::optional<TypeSignature> typeSig = boost::none)
        : _slotId(slotId), _typeSig(typeSig) {}

    explicit SbVar(FrameId frameId,
                   SlotId slotId,
                   boost::optional<TypeSignature> typeSig = boost::none)
        : _frameId(frameId), _slotId(slotId), _typeSig(typeSig) {}

    SbVar(const sbe::EVariable& var, boost::optional<TypeSignature> typeSig = boost::none)
        : _frameId(var.getFrameId()), _slotId(var.getSlotId()), _typeSig(typeSig) {}

    SbVar(const optimizer::ProjectionName& name,
          boost::optional<TypeSignature> typeSig = boost::none);

    SbVar(SbSlot s) : _slotId(s.getId()), _typeSig(s.getTypeSignature()) {}

    SbVar(SbLocalVar l)
        : _frameId(l.getFrameId()), _slotId(l.getSlotId()), _typeSig(l.getTypeSignature()) {}

    SbVar& operator=(SbSlot s) {
        _frameId.reset();
        _slotId = s.getId();
        _typeSig = s.getTypeSignature();
        return *this;
    }

    SbVar& operator=(SbLocalVar l) {
        _frameId = l.getFrameId();
        _slotId = l.getSlotId();
        _typeSig = l.getTypeSignature();
        return *this;
    }

    bool isVarExpr() const {
        return true;
    }
    bool isSlotExpr() const {
        return !_frameId;
    }
    bool isLocalVarExpr() const {
        return _frameId.has_value();
    }
    bool isConstantExpr() const {
        return false;
    }

    SbVar toVar() const {
        return *this;
    }
    SbSlot toSlot() const {
        tassert(8455816, "Expected slot variable expression", isSlotExpr());
        return SbSlot{_slotId, getTypeSignature()};
    }
    SbLocalVar toLocalVar() const {
        tassert(8455817, "Expected local variable expression", isLocalVarExpr());
        return SbLocalVar{*_frameId, _slotId, getTypeSignature()};
    }

    optimizer::ProjectionName getABTName() const {
        return _frameId ? getABTLocalVariableName(*_frameId, _slotId) : getABTVariableName(_slotId);
    }

    operator optimizer::ProjectionName() const {
        return getABTName();
    }

    bool hasTypeSignature() const {
        return _typeSig.has_value();
    }

    boost::optional<TypeSignature> getTypeSignature() const {
        return _typeSig.get();
    }

    void setTypeSignature(boost::optional<TypeSignature> typeSig) {
        _typeSig = typeSig;
    }

private:
    boost::optional<FrameId> _frameId;
    SlotId _slotId{0};
    OptTypeSignature _typeSig;
};

inline SbVar SbSlot::toVar() const {
    return SbVar{*this};
}

inline SbVar SbLocalVar::toVar() const {
    return SbVar{*this};
}

/**
 * The SbExpr class is used to represent expressions in the SBE stage builder. "SbExpr" is short
 * for "stage builder expression".
 */
class SbExpr {
public:
    using Vector = std::vector<SbExpr>;
    using CaseValuePair = std::pair<SbExpr, SbExpr>;

    using SlotId = sbe::value::SlotId;
    using FrameId = sbe::FrameId;
    using LocalVarInfo = std::pair<int32_t, int32_t>;

    struct Abt {
        abt::HolderPtr ptr;
    };
    struct OptimizedAbt {
        abt::HolderPtr ptr;
    };

    /**
     * At any given time, an SbExpr object can be in one of 5 states:
     *  1) Null - The SbExpr doesn't hold anything.
     *  2) Slot - The SbExpr holds a slot variable (slot ID).
     *  3) LocalVar - The SbExpr holds a local variable (frame ID and slot ID).
     *  4) Abt - The SbExpr holds an ABT expression.
     *  5) OptimizedAbt - The SbExpr holds an ABT expression that has been marked
     *                    as "finished optimizing".
     */
    using VariantType = std::variant<std::monostate, SlotId, LocalVarInfo, Abt, OptimizedAbt>;

    template <typename... Args>
    static Vector makeSeq(Args&&... args) {
        Vector seq;
        (seq.emplace_back(std::forward<Args>(args)), ...);
        return seq;
    }

    template <typename... Args>
    static SbSlotVector makeSV(Args&&... args) {
        SbSlotVector sv;
        (sv.emplace_back(std::forward<Args>(args)), ...);
        return sv;
    }

    SbExpr() noexcept = default;

    SbExpr(SbExpr&& e) noexcept : _storage(std::move(e._storage)), _typeSig(e._typeSig) {
        e.reset();
    }

    SbExpr(const SbExpr&) = delete;

    SbExpr(SbSlot s) noexcept : _storage(s.getId()), _typeSig(s.getTypeSignature()) {}

    SbExpr(SbLocalVar l) noexcept {
        set(l);
    }

    SbExpr(SbVar var) {
        if (var.isSlotExpr()) {
            auto slot = var.toSlot();
            _storage = slot.getId();
            _typeSig = slot.getTypeSignature();
        } else {
            set(var.toLocalVar());
        }
    }

    SbExpr(boost::optional<SbSlot> s) : SbExpr(s ? SbExpr{*s} : SbExpr{}) {}

    SbExpr(boost::optional<SbLocalVar> l) : SbExpr(l ? SbExpr{*l} : SbExpr{}) {}

    SbExpr(boost::optional<SbVar> var) : SbExpr(var ? SbExpr{*var} : SbExpr{}) {}

    SbExpr(const abt::HolderPtr& a, boost::optional<TypeSignature> typeSig = boost::none);

    SbExpr(abt::HolderPtr&& a, boost::optional<TypeSignature> typeSig = boost::none) noexcept;

    SbExpr(Abt a, boost::optional<TypeSignature> typeSig = boost::none) noexcept;

    SbExpr(OptimizedAbt a, boost::optional<TypeSignature> typeSig = boost::none) noexcept;

    ~SbExpr() = default;

    SbExpr& operator=(SbExpr&& e) noexcept {
        if (this == &e) {
            return *this;
        }

        _storage = std::move(e._storage);
        _typeSig = e._typeSig;
        e.reset();
        return *this;
    }

    SbExpr& operator=(const SbExpr&) = delete;

    SbExpr& operator=(SbSlot s) {
        _storage = s.getId();
        _typeSig = s.getTypeSignature();
        return *this;
    }

    SbExpr& operator=(SbLocalVar l) {
        set(l);
        return *this;
    }

    SbExpr& operator=(SbVar var) {
        if (var.isSlotExpr()) {
            *this = var.toSlot();
        } else {
            set(var.toLocalVar());
        }
        return *this;
    }

    SbExpr& operator=(boost::optional<SbSlot> s) {
        *this = (s ? SbExpr{*s} : SbExpr{});
        return *this;
    }

    SbExpr& operator=(boost::optional<SbLocalVar> l) {
        *this = (l ? SbExpr{*l} : SbExpr{});
        return *this;
    }

    SbExpr& operator=(boost::optional<SbVar> var) {
        *this = (var ? SbExpr{*var} : SbExpr{});
        return *this;
    }

    SbExpr& operator=(const abt::HolderPtr& a);

    SbExpr& operator=(abt::HolderPtr&& a) noexcept;

    SbExpr& operator=(Abt a) noexcept;

    SbExpr& operator=(OptimizedAbt a) noexcept;

    bool isNull() const noexcept {
        return holds_alternative<std::monostate>(_storage);
    }

    explicit operator bool() const noexcept {
        return !isNull();
    }

    void reset() noexcept {
        _storage.emplace<std::monostate>();
        _typeSig.reset();
    }

    SbExpr clone() const;

    bool isVarExpr() const;
    bool isSlotExpr() const;
    bool isLocalVarExpr() const;
    bool isConstantExpr() const;

    SbVar toVar() const;
    SbSlot toSlot() const;
    SbLocalVar toLocalVar() const;
    std::pair<sbe::value::TypeTags, sbe::value::Value> getConstantValue() const;

    /**
     * Lowers the contents of this SbExpr into SBE EExpression and returns it.
     *
     * Note that this method calls optimize(), which may modify '_typeSig' and '_storage' (if the
     * typechecker deduces an updated type, or if constant folding can simplify the expression).
     *
     * Aside from calling optimize(), this method will not make any other modifications to
     * '_typeSig' or '_storage'.
     */
    std::unique_ptr<sbe::EExpression> lower(StageBuilderState& state,
                                            const VariableTypes* slotInfo = nullptr);

    /**
     * Extracts the contents of this SbExpr in the form of an ABT expression and returns it.
     * This method will tassert if it's invoked when isNull() is true.
     *
     * As its name suggests, extractABT() should be treated like a "move-from" style operation
     * that leaves 'this' in a valid but indeterminate state.
     */
    abt::HolderPtr extractABT();

    bool hasTypeSignature() const {
        return _typeSig.has_value();
    }

    boost::optional<TypeSignature> getTypeSignature() const {
        return _typeSig.get();
    }

    void setTypeSignature(boost::optional<TypeSignature> typeSig) {
        _typeSig = typeSig;
    }

    // Optimize this SbExpr if possible. If this SbExpr holds an ABT, the ABT will be modified
    // in place. The type information for the visible slots provided in the slotInfo argument
    // is forwarded to the constant folding operation and to the typechecker (if it's not null).
    void optimize(StageBuilderState& state, const VariableTypes* slotInfo = nullptr);

    void optimize(StageBuilderState& state, const VariableTypes& slotInfo) {
        optimize(state, &slotInfo);
    }

    bool isFinishedOptimizing() const {
        return holds_alternative<OptimizedAbt>(_storage);
    }

    void setFinishedOptimizing();

private:
    explicit SbExpr(SlotId s, boost::optional<TypeSignature> typeSig)
        : _storage(s), _typeSig(typeSig) {}

    explicit SbExpr(LocalVarInfo localVarInfo, boost::optional<TypeSignature> typeSig)
        : _storage(localVarInfo), _typeSig(typeSig) {}

    void set(SbLocalVar l);

    bool holdsAbtInternal() const {
        return holds_alternative<Abt>(_storage) || holds_alternative<OptimizedAbt>(_storage);
    }

    const abt::HolderPtr& getAbtInternal() const {
        tassert(8455819, "Expected ABT expression", holdsAbtInternal());
        return holds_alternative<Abt>(_storage) ? get<Abt>(_storage).ptr
                                                : get<OptimizedAbt>(_storage).ptr;
    }

    abt::HolderPtr& getAbtInternal() {
        tassert(8455820, "Expected ABT expression", holdsAbtInternal());
        return holds_alternative<Abt>(_storage) ? get<Abt>(_storage).ptr
                                                : get<OptimizedAbt>(_storage).ptr;
    }

    VariantType _storage;
    OptTypeSignature _typeSig;
};

/**
 * "SbStage" is short for "stage builder stage". SbStage is an alias for a unique pointer type.
 */
using SbStage = std::unique_ptr<sbe::PlanStage>;

/**
 * For a number of EExpression-related structures, we have corresponding "SbExpr" versions
 * of these structures. Here is a list:
 *    EExpression::Vector -> SbExpr::Vector
 *    SlotExprPairVector  -> SbExprSbSlotVector or SbExprOptSbSlotVector
 *    AggExprPair         -> SbAggExpr
 *    AggExprVector       -> SbAggExprVector
 */
using SbExprSbSlotPair = std::pair<SbExpr, SbSlot>;
using SbExprSbSlotVector = std::vector<SbExprSbSlotPair>;

using SbExprOptSbSlotPair = std::pair<SbExpr, boost::optional<SbSlot>>;
using SbExprOptSbSlotVector = std::vector<SbExprOptSbSlotPair>;

struct SbAggExpr {
    SbExpr init;
    SbExpr blockAgg;
    SbExpr agg;
};

using SbAggExprVector = std::vector<std::pair<SbAggExpr, boost::optional<SbSlot>>>;

struct SbWindow {
    SbSlotVector windowExprSlots;
    SbSlotVector frameFirstSlots;
    SbSlotVector frameLastSlots;
    SbExpr::Vector initExprs;
    SbExpr::Vector addExprs;
    SbExpr::Vector removeExprs;
    SbExpr lowBoundExpr;
    SbExpr highBoundExpr;
};

inline void addVariableTypesHelper(VariableTypes& varTypes, SbSlot slot) {
    if (auto typeSig = slot.getTypeSignature()) {
        varTypes[getABTVariableName(slot)] = *typeSig;
    }
}

inline void addVariableTypesHelper(VariableTypes& varTypes, boost::optional<SbSlot> slot) {
    if (slot) {
        if (auto typeSig = slot->getTypeSignature()) {
            varTypes[getABTVariableName(*slot)] = *typeSig;
        }
    }
}

template <typename IterT>
inline void addVariableTypesHelper(VariableTypes& varTypes, IterT it, IterT endIt) {
    for (; it != endIt; ++it) {
        addVariableTypesHelper(varTypes, *it);
    }
}

void addVariableTypesHelper(VariableTypes& varTypes, const PlanStageSlots& outputs);

inline void buildVariableTypesHelper(VariableTypes& varTypes) {
    return;
}

template <typename... Args>
inline void buildVariableTypesHelper(VariableTypes& varTypes,
                                     const PlanStageSlots& outputs,
                                     Args&&... args) {
    addVariableTypesHelper(varTypes, outputs);
    buildVariableTypesHelper(varTypes, std::forward<Args>(args)...);
}

template <typename... Args>
inline void buildVariableTypesHelper(VariableTypes& varTypes, SbSlot slot, Args&&... args) {
    addVariableTypesHelper(varTypes, slot);
    buildVariableTypesHelper(varTypes, std::forward<Args>(args)...);
}

template <typename... Args>
inline void buildVariableTypesHelper(VariableTypes& varTypes,
                                     const SbSlotVector& slots,
                                     Args&&... args) {
    addVariableTypesHelper(varTypes, slots.begin(), slots.end());
    buildVariableTypesHelper(varTypes, std::forward<Args>(args)...);
}

template <typename... Args>
inline void buildVariableTypesHelper(VariableTypes& varTypes,
                                     const std::vector<SbSlot>& slots,
                                     Args&&... args) {
    addVariableTypesHelper(varTypes, slots.begin(), slots.end());
    buildVariableTypesHelper(varTypes, std::forward<Args>(args)...);
}

// Collect the type information of the slots declared in the provided stage output.
template <typename... Args>
inline VariableTypes buildVariableTypes(Args&&... args) {
    VariableTypes varTypes;
    buildVariableTypesHelper(varTypes, std::forward<Args>(args)...);

    return varTypes;
}

// This function takes a VariableTypes object ('varTypes'), changes the type of each variable
// from 'T' to 'T.exclude(typesToExclude)', and then returns the updated VariableTypes object.
VariableTypes excludeTypes(VariableTypes varTypes, TypeSignature typesToExclude);

// Given an expression built on top of scalar processing, along with the definition of the
// visible slots (some of which could be marked as holding block of values), produce an
// expression tree that can be executed directly on top of them. Returns an empty result if the
// expression isn't vectorizable.
SbExpr buildVectorizedExpr(StageBuilderState& state,
                           SbExpr scalarExpression,
                           const PlanStageSlots& outputs,
                           bool forFilterStage);

}  // namespace mongo::stage_builder
