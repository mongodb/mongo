// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/util/modules.h"

namespace mongo::sbe {

class BaseMockStage : public PlanStage {
public:
    explicit BaseMockStage(PlanNodeId planNodeId,
                           PlanYieldPolicySBE* yieldPolicy = nullptr,
                           bool participateInTrialRunTracking = true);

    std::unique_ptr<PlanStage> clone() const override;

    void prepare(CompileCtx& ctx) override;

    value::SlotAccessor* getAccessor(CompileCtx& ctx, value::SlotId slot) override;

    void open(bool reOpen) final;

    void close() final;

    std::unique_ptr<PlanStageStats> getStats(bool includeDebugInfo) const final;

    const SpecificStats* getSpecificStats() const final;

    size_t estimateCompileTimeSize() const final;

    void doDebugPrint(std::vector<DebugPrinter::Block>& ret,
                      DebugPrintInfo& debugPrintInfo) const final {}

protected:
};

class MockExceededMemoryLimitStage : public BaseMockStage {
public:
    using BaseMockStage::BaseMockStage;

    value::SlotAccessor* getAccessor(CompileCtx& ctx, value::SlotId slot) final;

    PlanState getNext() final;
};

class MockExceededMaxReadsStage : public BaseMockStage {
public:
    using BaseMockStage::BaseMockStage;

    void prepare(CompileCtx& ctx) final;

    value::SlotAccessor* getAccessor(CompileCtx& ctx, value::SlotId slot) final;

    PlanState getNext() final;

private:
    std::unique_ptr<value::ViewOfValueAccessor> _recordAccessor;
};

}  // namespace mongo::sbe
