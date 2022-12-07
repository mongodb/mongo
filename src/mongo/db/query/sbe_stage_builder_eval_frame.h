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

#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/stages/co_scan.h"
#include "mongo/db/exec/sbe/stages/limit_skip.h"
#include "mongo/stdx/variant.h"

namespace mongo::stage_builder {

/**
 * EvalExpr is a wrapper around an EExpression that can also carry a SlotId. It is used to eliminate
 * extra project stages. If 'slot' field is set, it contains the result of an expression. The user
 * of the class can just use this slot instead of projecting an expression into a new slot.
 */
class EvalExpr {
public:
    EvalExpr() = default;

    EvalExpr(EvalExpr&& e) : _exprOrSlot(std::move(e._exprOrSlot)) {
        e.reset();
    }

    EvalExpr(std::unique_ptr<sbe::EExpression>&& e) : _exprOrSlot(std::move(e)) {}

    EvalExpr(sbe::value::SlotId s) : _exprOrSlot(s) {}

    EvalExpr& operator=(EvalExpr&& e) {
        if (this == &e) {
            return *this;
        }

        _exprOrSlot = std::move(e._exprOrSlot);
        e.reset();
        return *this;
    }

    EvalExpr& operator=(std::unique_ptr<sbe::EExpression>&& e) {
        _exprOrSlot = std::move(e);
        e.reset();
        return *this;
    }

    EvalExpr& operator=(sbe::value::SlotId s) {
        _exprOrSlot = s;
        return *this;
    }

    boost::optional<sbe::value::SlotId> getSlot() const {
        return hasSlot() ? boost::make_optional(stdx::get<sbe::value::SlotId>(_exprOrSlot))
                         : boost::none;
    }

    bool hasSlot() const {
        return stdx::holds_alternative<sbe::value::SlotId>(_exprOrSlot);
    }

    EvalExpr clone() const {
        if (hasSlot()) {
            return stdx::get<sbe::value::SlotId>(_exprOrSlot);
        }

        const auto& expr = stdx::get<std::unique_ptr<sbe::EExpression>>(_exprOrSlot);

        tassert(
            6897007, "Unexpected: clone() method invoked on null EvalExpr", expr.get() != nullptr);

        return expr->clone();
    }

    explicit operator bool() const {
        return hasSlot() || stdx::get<std::unique_ptr<sbe::EExpression>>(_exprOrSlot) != nullptr;
    }

    void reset() {
        _exprOrSlot = std::unique_ptr<sbe::EExpression>();
    }

    std::unique_ptr<sbe::EExpression> extractExpr() {
        if (hasSlot()) {
            return sbe::makeE<sbe::EVariable>(stdx::get<sbe::value::SlotId>(_exprOrSlot));
        }

        return std::move(stdx::get<std::unique_ptr<sbe::EExpression>>(_exprOrSlot));
    }

private:
    stdx::variant<std::unique_ptr<sbe::EExpression>, sbe::value::SlotId> _exprOrSlot;
};

/**
 * EvalStage contains a PlanStage (_stage) and a vector of slots (_outSlots).
 *
 * _stage can be nullptr or it can point to an SBE PlanStage tree. If _stage is nullptr, the
 * extractStage() method will return a Limit-1/CoScan tree. If _stage is not nullptr, then the
 * extractStage() method will return _stage. EvalStage's default constructor initializes
 * _stage to be nullptr.
 *
 * The stageIsNull() method allows callers to check if _state is nullptr. Some helper functions
 * (such as makeLoopJoin()) take advantage of this knowledge and are able to perform optimizations
 * in the case where stageIsNull() == true.
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

    bool stageIsNull() const {
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

/**
 * To support non-leaf operators in general, SBE builders maintain a stack of EvalFrames. An
 * EvalFrame holds a subtree to build on top of (stage), a stack of expressions (exprs) and extra
 * data useful for particular builder (data).
 * Initially there is only one EvalFrame on the stack which holds the main tree. Non-leaf operators
 * can decide to push an EvalFrame on the stack before each of their children is evaluated if
 * desired. If a non-leaf operator pushes one or more EvalFrames onto the stack, it is responsible
 * for removing these EvalFrames from the stack later.
 */
template <typename T>
class EvalFrame {
public:
    template <typename... Args>
    EvalFrame(EvalStage stage, Args&&... args)
        : _data{std::forward<Args>(args)...}, _stage(std::move(stage)) {}

    const EvalExpr& topExpr() const {
        invariant(!_exprs.empty());
        return _exprs.top();
    }

    void pushExpr(EvalExpr expr) {
        _exprs.push(std::move(expr));
    }

    EvalExpr popExpr() {
        invariant(!_exprs.empty());
        auto expr = std::move(_exprs.top());
        _exprs.pop();
        return expr;
    }

    size_t exprsCount() const {
        return _exprs.size();
    }

    const T& data() const {
        return _data;
    }

    T& data() {
        return _data;
    }

    void setStage(EvalStage stage) {
        _stage = std::move(stage);
    }

    const EvalStage& getStage() const {
        return _stage;
    }

    EvalStage extractStage() {
        return std::move(_stage);
    }

private:
    T _data;
    EvalStage _stage;
    std::stack<EvalExpr> _exprs;
};

/**
 * Empty struct for 'data' field in case builder does not need to carry any additional data with
 * each frame.
 */
struct NoExtraFrameData {};

using EvalExprStagePair = std::pair<EvalExpr, EvalStage>;

template <typename Data = NoExtraFrameData>
class EvalStack {
public:
    using Frame = EvalFrame<Data>;

    EvalStack() = default;

    template <typename... Args>
    void emplaceFrame(Args&&... args) {
        stack.emplace(std::forward<Args>(args)...);
    }

    Frame& topFrame() {
        invariant(!stack.empty());
        return stack.top();
    }

    const Frame& topFrame() const {
        invariant(!stack.empty());
        return stack.top();
    }

    EvalExprStagePair popFrame() {
        invariant(framesCount() > 0);
        auto& frame = topFrame();

        invariant(frame.exprsCount() == 1);
        auto expr = frame.popExpr();
        auto stage = frame.extractStage();

        stack.pop();
        return {std::move(expr), std::move(stage)};
    }

    size_t framesCount() const {
        return stack.size();
    }

private:
    std::stack<Frame> stack;
};

}  // namespace mongo::stage_builder
