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
#include "mongo/db/query/sbe_stage_builder_abt_holder_def.h"
#include "mongo/db/query/sbe_stage_builder_type_signature.h"

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

optimizer::ProjectionName getABTVariableName(sbe::value::SlotId slotId);

optimizer::ProjectionName getABTLocalVariableName(sbe::FrameId frameId, sbe::value::SlotId slotId);

boost::optional<sbe::value::SlotId> getSbeVariableInfo(const optimizer::ProjectionName& var);

boost::optional<std::pair<sbe::FrameId, sbe::value::SlotId>> getSbeLocalVariableInfo(
    const optimizer::ProjectionName& var);

optimizer::ABT makeABTVariable(sbe::value::SlotId slot);

/**
 * Associate an expression with a signature representing all the possible types that the value
 * evalutated at runtime by the corresponding VM code can assume.
 */
struct TypedExpression {
    std::unique_ptr<sbe::EExpression> expr;
    TypeSignature typeSignature;
};

using VariableTypes = stdx::
    unordered_map<optimizer::ProjectionName, TypeSignature, optimizer::ProjectionName::Hasher>;

// Collect the type information of the slots declared in the provided stage output.
VariableTypes buildVariableTypes(const PlanStageSlots& outputs);

// Return whether the declared outputs contain a block value.
bool hasBlockOutput(const PlanStageSlots& outputs);

// Run constant folding on the provided ABT tree and return its type signature. If the type
// information for the visible slots is available in the slotInfo argument, it is used to perform a
// more precise type checking optimization. On return, the abt argument points to the modified tree.
TypeSignature constantFold(optimizer::ABT& abt,
                           StageBuilderState& state,
                           const VariableTypes* slotInfo = nullptr);

// Optimize (by modifying it in place via a call to constantFold) and convert the provided ABT tree
// into an equivalent typed EExpression tree. The type information for the visible slots provided in
// the slotInfo argument is forwarded to the constantFold operation.
TypedExpression abtToExpr(optimizer::ABT& abt,
                          StageBuilderState& state,
                          const VariableTypes* slotInfo = nullptr);

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
    SbVar(sbe::value::SlotId slotId) : _slotId(slotId) {}
    SbVar(sbe::FrameId frameId, sbe::value::SlotId slotId) : _frameId(frameId), _slotId(slotId) {}
    SbVar(const sbe::EVariable& var) : _frameId(var.getFrameId()), _slotId(var.getSlotId()) {}
    SbVar(const optimizer::ProjectionName& name);

    bool isSlot() const {
        return !_frameId;
    }

    bool isLocalVar() const {
        return _frameId.has_value();
    }

    boost::optional<sbe::value::SlotId> getSlot() const {
        return isSlot() ? boost::make_optional(_slotId) : boost::none;
    }

    boost::optional<std::pair<sbe::FrameId, sbe::value::SlotId>> getLocalVarInfo() const {
        return isLocalVar() ? boost::make_optional(std::pair(*_frameId, _slotId)) : boost::none;
    }

    optimizer::ProjectionName getABTName() const {
        return _frameId ? getABTLocalVariableName(*_frameId, _slotId) : getABTVariableName(_slotId);
    }

    operator optimizer::ProjectionName() const {
        return getABTName();
    }

private:
    boost::optional<sbe::FrameId> _frameId;
    sbe::value::SlotId _slotId;
};

/**
 * The SbExpr class is used to represent expressions in the SBE stage builder. "SbExpr" is short
 * for "stage builder expression".
 *
 * At any given time, an SbExpr object can be in one of 4 states:
 *  1) Null - The SbExpr doesn't hold anything.
 *  2) Slot - The SbExpr holds a slot variable.
 *  3) ABT - The SbExpr holds an ABT that is not known to be a slot variable.
 *  4) Expr - The SbExpr holds an EExpression that is not known to be a slot variable.
 *
 * 'e.isNull()' returns true if 'e' is in state 1. 'e.hasSlot()' returns true if 'e' is in state 2.
 * 'e.hasABT()' returns true if 'e' is in state 2 or state 3.
 */
class SbExpr {
public:
    using Vector = std::vector<SbExpr>;
    using CaseValuePair = std::pair<SbExpr, SbExpr>;

    using LocalVarInfo = std::pair<int32_t, int32_t>;
    using EExpr = std::unique_ptr<sbe::EExpression>;

    template <typename... Args>
    static inline Vector makeSeq(Args&&... args) {
        Vector seq;
        (seq.emplace_back(std::forward<Args>(args)), ...);
        return seq;
    }

    SbExpr() : _storage{false} {}

    SbExpr(SbExpr&& e) noexcept : _storage(std::move(e._storage)) {
        e.reset();
    }

    SbExpr(const SbExpr&) = delete;

    SbExpr(SbVar var) {
        if (var.isSlot()) {
            _storage = *var.getSlot();
        } else {
            auto [frameId, slotId] = *var.getLocalVarInfo();
            set(frameId, slotId);
        }
    }

    SbExpr(EExpr&& e) noexcept {
        if (e) {
            _storage = std::move(e);
        }
    }

    SbExpr(sbe::value::SlotId s) noexcept : _storage(s) {}

    SbExpr(boost::optional<sbe::value::SlotId> s) noexcept {
        if (s) {
            _storage = *s;
        }
    }

    SbExpr(const abt::HolderPtr& a);

    SbExpr(abt::HolderPtr&& a) noexcept : _storage(std::move(a)) {}

    ~SbExpr() = default;

    SbExpr& operator=(SbExpr&& e) noexcept {
        if (this == &e) {
            return *this;
        }

        _storage = std::move(e._storage);
        e.reset();
        return *this;
    }

    SbExpr& operator=(const SbExpr&) = delete;

    SbExpr& operator=(SbVar var) {
        if (var.isSlot()) {
            _storage = *var.getSlot();
        } else {
            auto [frameId, slotId] = *var.getLocalVarInfo();
            set(frameId, slotId);
        }

        return *this;
    }

    SbExpr& operator=(EExpr&& e) noexcept {
        if (e) {
            _storage = std::move(e);
        } else {
            reset();
        }

        e.reset();

        return *this;
    }

    SbExpr& operator=(sbe::value::SlotId s) noexcept {
        _storage = s;
        return *this;
    }

    SbExpr& operator=(boost::optional<sbe::value::SlotId> s) noexcept {
        if (s) {
            _storage = *s;
        } else {
            reset();
        }

        return *this;
    }

    SbExpr& operator=(abt::HolderPtr&& a) noexcept {
        _storage = std::move(a);
        return *this;
    }

    boost::optional<sbe::value::SlotId> getSlot() const noexcept {
        return hasSlot() ? boost::make_optional(get<sbe::value::SlotId>(_storage)) : boost::none;
    }

    bool hasSlot() const noexcept {
        return holds_alternative<sbe::value::SlotId>(_storage);
    }

    bool hasABT() const noexcept {
        return holds_alternative<sbe::value::SlotId>(_storage) ||
            holds_alternative<LocalVarInfo>(_storage) ||
            holds_alternative<abt::HolderPtr>(_storage);
    }

    SbExpr clone() const {
        if (hasSlot()) {
            return get<sbe::value::SlotId>(_storage);
        }

        if (holds_alternative<LocalVarInfo>(_storage)) {
            return get<LocalVarInfo>(_storage);
        }

        if (holds_alternative<abt::HolderPtr>(_storage)) {
            return get<abt::HolderPtr>(_storage);
        }

        if (holds_alternative<EExpr>(_storage)) {
            const auto& expr = get<EExpr>(_storage);
            return expr->clone();
        }

        return {};
    }

    bool isNull() const noexcept {
        return holds_alternative<bool>(_storage);
    }

    explicit operator bool() const noexcept {
        return !isNull();
    }

    void reset() noexcept {
        _storage = false;
    }

    TypedExpression getExpr(StageBuilderState& state) const;

    /**
     * Extract the expression on top of the stack as an SBE EExpression node. If the expression is
     * stored as an ABT node, it is lowered into an SBE expression, using the provided map to
     * convert variable names into slot ids.
     */
    TypedExpression extractExpr(StageBuilderState& state);

    /**
     * Extract the expression on top of the stack as an ABT node. Throws an exception if the
     * expression is stored as an EExpression.
     */
    abt::HolderPtr extractABT();

private:
    SbExpr(LocalVarInfo localVarInfo) : _storage(localVarInfo) {}

    void set(sbe::FrameId frameId, sbe::value::SlotId slotId);

    // The bool type as the first option is used to represent the empty storage.
    std::variant<bool, EExpr, sbe::value::SlotId, LocalVarInfo, abt::HolderPtr> _storage;
};

/**
 * "SbStage" is short for "stage builder stage". SbStage is an alias for a unique pointer type.
 */
using SbStage = std::unique_ptr<sbe::PlanStage>;

/**
 * In the past, "SbExpr" used to be named "EvalExpr". For now we have this type alias so that code
 * that refers to "EvalExpr" still works.
 *
 * TODO SERVER-80366: Remove this type alias when it's no longer needed.
 */
using EvalExpr = SbExpr;

}  // namespace mongo::stage_builder
