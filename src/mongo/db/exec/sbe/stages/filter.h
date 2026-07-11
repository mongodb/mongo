// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/sbe/expressions/compile_ctx.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/size_estimator.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/vm/vm.h"
#include "mongo/util/modules.h"

namespace mongo::sbe {
using namespace std::literals::string_view_literals;
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
        : PlanStage(IsConst ? "cfilter"sv : (IsEof ? "efilter" : "filter"sv),
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
                                                             participateInTrialRunTracking());
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

    std::unique_ptr<PlanStageStats> getStats(bool includeDebugInfo) const override {
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

    void doDebugPrint(std::vector<DebugPrinter::Block>& ret,
                      DebugPrintInfo& debugPrintInfo) const final {
        ret.emplace_back("{`");
        DebugPrinter::addBlocks(ret, _filter->debugPrint());
        ret.emplace_back("`}");

        DebugPrinter::addNewLine(ret);

        if (debugPrintInfo.printBytecode) {
            PlanStage::debugPrintBytecode(ret, _filterCode, "FILTER" /*title*/);
        }

        DebugPrinter::addBlocks(ret, _children[0]->debugPrint(debugPrintInfo));
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
