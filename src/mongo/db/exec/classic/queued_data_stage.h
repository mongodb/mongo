/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/exec/classic/plan_stage.h"
#include "mongo/db/exec/classic/working_set.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"

#include <memory>
#include <queue>

namespace mongo {

class RecordId;

/**
 * QueuedDataStage is a data-producing stage.  Unlike the other two leaf stages (CollectionScan
 * and IndexScan) QueuedDataStage does not require any underlying storage layer.
 *
 * A QueuedDataStage is "programmed" by pushing return values from work() onto its internal
 * queue.  Calls to QueuedDataStage::work() pop values off that queue and return them in FIFO
 * order, annotating the working set with data when appropriate.
 */
class QueuedDataStage final : public PlanStage {
public:
    QueuedDataStage(ExpressionContext* expCtx, WorkingSet* ws);

    StageState doWork(WorkingSetID* out) final;

    bool isEOF() const final;

    StageType stageType() const final {
        return STAGE_QUEUED_DATA;
    }

    //
    // Exec stats
    //

    std::unique_ptr<PlanStageStats> getStats() final;

    const SpecificStats* getSpecificStats() const final;

    /**
     * Add a result to the back of the queue.
     *
     * The caller is responsible for allocating 'id' and filling out the WSM keyed by 'id'
     * appropriately.
     *
     * The QueuedDataStage takes ownership of 'id', so the caller should not call WorkingSet::free()
     * on it.
     */
    void pushBack(const WorkingSetID& id);

    static const char* kStageType;

private:
    // The data we return.
    std::queue<WorkingSetID> _members;

    // Stats
    MockStats _specificStats;
};

}  // namespace mongo
