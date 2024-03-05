/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/exec/sbe/stages/stages.h"

namespace mongo::sbe {

class BaseMockStage : public PlanStage {
public:
    explicit BaseMockStage(PlanNodeId planNodeId,
                           PlanYieldPolicy* yieldPolicy = nullptr,
                           bool participateInTrialRunTracking = true);

    std::unique_ptr<PlanStage> clone() const override;

    void prepare(CompileCtx& ctx) override;

    value::SlotAccessor* getAccessor(CompileCtx& ctx, value::SlotId slot) override;

    void open(bool reOpen) final;

    void close() final;

    std::unique_ptr<PlanStageStats> getStats(bool includeDebugInfo) const final;

    const SpecificStats* getSpecificStats() const final;

    size_t estimateCompileTimeSize() const final;
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

    void doDetachFromTrialRunTracker();

    PlanStage::TrialRunTrackerAttachResultMask doAttachToTrialRunTracker(
        TrialRunTracker* tracker, TrialRunTrackerAttachResultMask childrenAttachResult);

private:
    TrialRunTracker* _tracker;
    std::unique_ptr<value::ViewOfValueAccessor> _recordAccessor;
};

}  // namespace mongo::sbe
