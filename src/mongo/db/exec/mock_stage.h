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

#include <queue>

#include "mongo/db/exec/plan_stage.h"

namespace mongo {

/**
 * A stage designed for use in unit tests. The test can queue a sequence of results which will be
 * returned to the parent stage using the 'enqueue*()' methods.
 */
class MockStage final : public PlanStage {
public:
    static constexpr StringData kStageType = "MOCK"_sd;

    MockStage(ExpressionContext* expCtx, WorkingSet* ws);

    StageState doWork(WorkingSetID* out) final;

    bool isEOF() final {
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
     * The QueuedDataStage takes ownership of 'id', so the caller should not call WorkingSet::free()
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
    using MockResult = stdx::variant<WorkingSetID, PlanStage::StageState, Status>;

    WorkingSet* _ws;

    std::queue<MockResult> _results;

    MockStats _specificStats;
};

}  // namespace mongo
