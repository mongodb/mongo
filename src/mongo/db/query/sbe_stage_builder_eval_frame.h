/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include <stack>

#include "mongo/db/exec/sbe/abt/abt_lower_defs.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/stages/co_scan.h"
#include "mongo/db/exec/sbe/stages/limit_skip.h"
#include "mongo/db/query/sbe_stage_builder_abt_holder_def.h"
#include "mongo/stdx/variant.h"

namespace mongo::sbe {
class RuntimeEnvironment;
}

namespace mongo::stage_builder {

struct StageBuilderState;

/**
 * EvalExpr is a wrapper around an EExpression that can also carry a SlotId. It is used to eliminate
 * extra project stages. If 'slot' field is set, it contains the result of an expression. The user
 * of the class can just use this slot instead of projecting an expression into a new slot.
 */
class EvalExpr {
public:
    EvalExpr() : _storage{false} {}

    EvalExpr(EvalExpr&& e) : _storage(std::move(e._storage)) {
        e.reset();
    }

    EvalExpr(std::unique_ptr<sbe::EExpression>&& e) : _storage(std::move(e)) {}

    EvalExpr(sbe::value::SlotId s) : _storage(s) {}

    EvalExpr(const abt::HolderPtr& a);

    EvalExpr(abt::HolderPtr&& a) : _storage(std::move(a)) {}

    EvalExpr& operator=(EvalExpr&& e) {
        if (this == &e) {
            return *this;
        }

        _storage = std::move(e._storage);
        e.reset();
        return *this;
    }

    EvalExpr& operator=(std::unique_ptr<sbe::EExpression>&& e) {
        _storage = std::move(e);
        e.reset();
        return *this;
    }

    EvalExpr& operator=(sbe::value::SlotId s) {
        _storage = s;
        return *this;
    }

    EvalExpr& operator=(abt::HolderPtr&& a) {
        _storage = std::move(a);
        return *this;
    }

    boost::optional<sbe::value::SlotId> getSlot() const {
        return hasSlot() ? boost::make_optional(stdx::get<sbe::value::SlotId>(_storage))
                         : boost::none;
    }

    bool hasSlot() const {
        return stdx::holds_alternative<sbe::value::SlotId>(_storage);
    }

    bool hasExpr() const {
        return stdx::holds_alternative<std::unique_ptr<sbe::EExpression>>(_storage);
    }

    bool hasABT() const {
        return stdx::holds_alternative<abt::HolderPtr>(_storage);
    }

    EvalExpr clone() const {
        if (hasSlot()) {
            return stdx::get<sbe::value::SlotId>(_storage);
        }

        if (hasABT()) {
            return stdx::get<abt::HolderPtr>(_storage);
        }

        if (stdx::holds_alternative<bool>(_storage)) {
            return EvalExpr{};
        }

        const auto& expr = stdx::get<std::unique_ptr<sbe::EExpression>>(_storage);
        if (expr) {
            return expr->clone();
        }

        return {};
    }

    bool isNull() const {
        return stdx::holds_alternative<bool>(_storage);
    }

    explicit operator bool() const {
        return !isNull();
    }

    void reset() {
        _storage = false;
    }

    std::unique_ptr<sbe::EExpression> getExpr(optimizer::SlotVarMap& varMap,
                                              const sbe::RuntimeEnvironment& runtimeEnv) const {
        return clone().extractExpr(varMap, runtimeEnv);
    }

    /**
     * Extract the expression on top of the stack as an SBE EExpression node. If the expression is
     * stored as an ABT node, it is lowered into an SBE expression, using the provided map to
     * convert variable names into slot ids.
     */
    std::unique_ptr<sbe::EExpression> extractExpr(optimizer::SlotVarMap& varMap,
                                                  const sbe::RuntimeEnvironment& runtimeEnv);

    /**
     * Helper function that obtains data needed for EvalExpr::extractExpr from StageBuilderState
     */
    std::unique_ptr<sbe::EExpression> extractExpr(StageBuilderState& state);

    /**
     * Extract the expression on top of the stack as an ABT node. If the expression is stored as a
     * slot id, the mapping between the generated ABT node and the slot id is recorded in the map.
     * Throws an exception if the expression is stored as an SBE EExpression.
     */
    abt::HolderPtr extractABT(optimizer::SlotVarMap& varMap);

private:
    // The bool type as the first option is used to represent the empty storage.
    stdx::variant<bool, std::unique_ptr<sbe::EExpression>, sbe::value::SlotId, abt::HolderPtr>
        _storage;
};

/**
 * EvalStage contains a PlanStage (_stage) and a vector of slots (_outSlots).
 *
 * _stage can be nullptr or it can point to an SBE PlanStage tree. If _stage is nullptr, the
 * extractStage() method will return a Limit-1/CoScan tree. If _stage is not nullptr, then the
 * extractStage() method will return _stage. EvalStage's default constructor initializes
 * _stage to be nullptr.
 *
 * The isNull() method allows callers to check if _state is nullptr. Some helper functions (such
 * as makeLoopJoin()) take advantage of this knowledge and are able to perform optimizations in
 * the case where isNull() == true.
 *
 * The _outSlots vector keeps track of all of the "output" slots that are produced by the current
 * sbe::PlanStage tree (_stage). The _outSlots vector is used by makeLoopJoin() and makeTraverse()
 * to ensure that all of the slots produced by the left side are visible to the right side and are
 * also visible to the parent of the LoopJoinStage/TraverseStage.
 */
class EvalStage {
public:
    EvalStage() {}

    EvalStage(std::unique_ptr<sbe::PlanStage> stage, sbe::value::SlotVector outSlots)
        : _stage(std::move(stage)), _outSlots(std::move(outSlots)) {}

    EvalStage(EvalStage&& other)
        : _stage(std::move(other._stage)), _outSlots(std::move(other._outSlots)) {}

    EvalStage& operator=(EvalStage&& other) {
        _stage = std::move(other._stage);
        _outSlots = std::move(other._outSlots);
        return *this;
    }

    bool isNull() const {
        return !_stage;
    }

    std::unique_ptr<sbe::PlanStage> extractStage(PlanNodeId planNodeId) {
        return _stage ? std::move(_stage)
                      : sbe::makeS<sbe::LimitSkipStage>(
                            sbe::makeS<sbe::CoScanStage>(planNodeId), 1, boost::none, planNodeId);
    }

    void setStage(std::unique_ptr<sbe::PlanStage> stage) {
        _stage = std::move(stage);
    }

    const sbe::value::SlotVector& getOutSlots() const {
        return _outSlots;
    }

    sbe::value::SlotVector extractOutSlots() {
        return std::move(_outSlots);
    }

    void setOutSlots(sbe::value::SlotVector outSlots) {
        _outSlots = std::move(outSlots);
    }

    void addOutSlot(sbe::value::SlotId slot) {
        _outSlots.push_back(slot);
    }

private:
    std::unique_ptr<sbe::PlanStage> _stage;
    sbe::value::SlotVector _outSlots;
};

using EvalExprStagePair = std::pair<EvalExpr, EvalStage>;

}  // namespace mongo::stage_builder
