// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/db/exec/classic/plan_stage.h"
#include "mongo/db/exec/classic/working_set.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <memory>
#include <queue>
#include <string_view>
#include <variant>

namespace mongo {
using namespace std::literals::string_view_literals;

/**
 * A stage designed for use in unit tests. The test can queue a sequence of results which will be
 * returned to the parent stage using the 'enqueue*()' methods.
 */
class MockStage final : public PlanStage {
public:
    static constexpr std::string_view kStageType = "MOCK"sv;

    MockStage(ExpressionContext* expCtx, WorkingSet* ws);

    StageState doWork(WorkingSetID* out) final;

    bool isEOF() const final {
        return _results.empty();
    }

    StageType stageType() const final {
        return STAGE_MOCK;
    }

    std::unique_ptr<PlanStageStats> getStats() final;

    const SpecificStats* getSpecificStats() const final {
        return &_specificStats;
    }

    /**
     * Adds a WorkingSetMember to the back of the queue.
     *
     * The caller is responsible for allocating 'id' and filling out the WSM keyed by 'id'
     * appropriately.
     *
     * The MockStage takes ownership of 'id', so the caller should not call WorkingSet::free()
     * on it.
     */
    void enqueueAdvanced(WorkingSetID wsid) {
        _results.push(wsid);
    }

    /**
     * Adds a StageState code such as 'NEED_TIME' or 'NEED_YIELD' to the back of the queue. Illegal
     * to call with 'ADVANCED' -- 'enqueueAdvanced()' should be used instead. Also illegal to call
     * with 'IS_EOF', since EOF is implied when the mock stage's queue is emptied.
     */
    void enqueueStateCode(StageState stageState) {
        invariant(stageState != PlanStage::ADVANCED);
        invariant(stageState != PlanStage::IS_EOF);
        _results.push(stageState);
    }

    /**
     * Adds 'status' to the queue. When the 'status' is dequeued, it will be thrown from 'work()' as
     * an exception.
     */
    void enqueueError(Status status) {
        invariant(!status.isOK());
        _results.push(status);
    }

private:
    // The mock stage holds a queue of objects of this type. Each element in the queue can either be
    // a document to return, a StageState code, or a Status representing an error.
    using MockResult = std::variant<WorkingSetID, PlanStage::StageState, Status>;

    std::queue<MockResult> _results;

    MockStats _specificStats;
};

}  // namespace mongo
