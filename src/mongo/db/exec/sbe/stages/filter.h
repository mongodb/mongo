/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/db/exec/sbe/expressions/compile_ctx.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/size_estimator.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/vm/vm.h"

namespace mongo::sbe {
/**
 * A plan stage which discards or retains values based on a predicate expression.
 *
 * If the 'IsConst' template parameter is true then the filter expression is 'constant' i.e. it does
 * not depend on values coming from its input. In this case, the stage is notated as "cfilter"
 * rather than plain "filter". The predicate is evaluated in the open() call. If the result is
 * false, then 'getNext()' returns EOF immediately.
 *
 * The 'IsEof' template parameter controls 'early out' behavior of the filter expression. If this
 * template parameter is true, then the stage is notated as "efilter" rather than plain "filter".
 * Once the filter evaluates to false then the getNext() call returns EOF.
 *
 * Only one of 'IsConst' and 'IsEof' may be true.
 *
 * Records pass through the filter when the 'filter' expression evaluates to true.
 *
 * Debug string representations:
 *
 *  filter { predicate } childStage
 *  cfilter { predicate } childStage
 *  efilter { predicate } childStage
 */
template <bool IsConst, bool IsEof = false>
class FilterStage final : public PlanStage {
public:
    FilterStage(std::unique_ptr<PlanStage> input,
                std::unique_ptr<EExpression> filter,
                PlanNodeId planNodeId,
                bool participateInTrialRunTracking = true)
        : PlanStage(IsConst ? "cfilter"_sd : (IsEof ? "efilter" : "filter"_sd),
                    nullptr /* yieldPolicy */,
                    planNodeId,
                    participateInTrialRunTracking),
          _filter(std::move(filter)) {
        static_assert(!IsEof || !IsConst);
        _children.emplace_back(std::move(input));
        tassert(8400101, "Filter must be passed a filter", _filter);
    }

    std::unique_ptr<PlanStage> clone() const final {
        return std::make_unique<FilterStage<IsConst, IsEof>>(_children[0]->clone(),
                                                             _filter->clone(),
                                                             _commonStats.nodeId,
                                                             _participateInTrialRunTracking);
    }

    void prepare(CompileCtx& ctx) final {
        _children[0]->prepare(ctx);

        ctx.root = this;
        _filterCode = _filter->compile(ctx);
    }

    value::SlotAccessor* getAccessor(CompileCtx& ctx, value::SlotId slot) final {
        return _children[0]->getAccessor(ctx, slot);
    }

    void open(bool reOpen) final {
        auto optTimer(getOptTimer(_opCtx));

        _commonStats.opens++;

        if constexpr (IsConst) {
            _specificStats.numTested++;

            auto pass = _bytecode.runPredicate(_filterCode.get());
            if (!pass) {
                close();
                return;
            }
        }
        _children[0]->open(reOpen);
        _childOpened = true;
    }

    PlanState getNext() final {
        auto optTimer(getOptTimer(_opCtx));

        // The constant filter evaluates the predicate in the open method.
        if constexpr (IsConst) {
            if (!_childOpened) {
                return trackPlanState(PlanState::IS_EOF);
            } else {
                return trackPlanState(_children[0]->getNext());
            }
        }

        auto state = PlanState::IS_EOF;
        bool pass = false;

        do {
            state = _children[0]->getNext();

            if (state == PlanState::ADVANCED) {
                _specificStats.numTested++;

                pass = _bytecode.runPredicate(_filterCode.get());

                if constexpr (IsEof) {
                    if (!pass) {
                        return trackPlanState(PlanState::IS_EOF);
                    }
                }
            }
        } while (state == PlanState::ADVANCED && !pass);

        return trackPlanState(state);
    }

    void close() final {
        auto optTimer(getOptTimer(_opCtx));

        trackClose();

        if (_childOpened) {
            _children[0]->close();
            _childOpened = false;
        }
    }

    std::unique_ptr<PlanStageStats> getStats(bool includeDebugInfo) const {
        auto ret = std::make_unique<PlanStageStats>(_commonStats);
        ret->specific = std::make_unique<FilterStats>(_specificStats);

        if (includeDebugInfo) {
            BSONObjBuilder bob;
            bob.appendNumber("numTested", static_cast<long long>(_specificStats.numTested));
            bob.append("filter", DebugPrinter{}.print(_filter->debugPrint()));
            ret->debugInfo = bob.obj();
        }

        ret->children.emplace_back(_children[0]->getStats(includeDebugInfo));
        return ret;
    }

    const SpecificStats* getSpecificStats() const final {
        return &_specificStats;
    }

    std::vector<DebugPrinter::Block> debugPrint() const final {
        auto ret = PlanStage::debugPrint();

        ret.emplace_back("{`");
        DebugPrinter::addBlocks(ret, _filter->debugPrint());
        ret.emplace_back("`}");

        DebugPrinter::addNewLine(ret);

        DebugPrinter::addBlocks(ret, _children[0]->debugPrint());

        return ret;
    }

    size_t estimateCompileTimeSize() const final {
        size_t size = sizeof(*this);
        size += size_estimator::estimate(_children);
        size += _filter->estimateSize();
        size += size_estimator::estimate(_specificStats);
        return size;
    }

protected:
    bool shouldOptimizeSaveState(size_t) const final {
        return true;
    }

private:
    const std::unique_ptr<EExpression> _filter;
    std::unique_ptr<vm::CodeFragment> _filterCode;

    vm::ByteCode _bytecode;

    bool _childOpened{false};
    FilterStats _specificStats;
};
}  // namespace mongo::sbe
