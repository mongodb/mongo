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
#include "mongo/db/record_id.h"

#include <cstddef>
#include <memory>
#include <queue>

namespace mongo {

/**
 * Reads from N children, each of which must have a valid RecordId. Assumes each child produces
 * RecordIds in sorted order. Outputs the intersection of the RecordIds outputted by the children.
 *
 * Preconditions: Valid RecordId. More than one child.
 */
class AndSortedStage final : public PlanStage {
public:
    AndSortedStage(ExpressionContext* expCtx, WorkingSet* ws);

    void addChild(std::unique_ptr<PlanStage> child);

    StageState doWork(WorkingSetID* out) final;
    bool isEOF() const final;

    StageType stageType() const final {
        return STAGE_AND_SORTED;
    }

    std::unique_ptr<PlanStageStats> getStats() final;

    const SpecificStats* getSpecificStats() const final;

    static const char* kStageType;

private:
    // Find a node to AND against.
    PlanStage::StageState getTargetRecordId(WorkingSetID* out);

    // Move a child which hasn't advanced to the target node forward.
    // Returns the target node in 'out' if all children successfully advance to it.
    PlanStage::StageState moveTowardTargetRecordId(WorkingSetID* out);

    // Not owned by us.
    WorkingSet* _ws;

    // The current node we're AND-ing against.
    size_t _targetNode;
    RecordId _targetRecordId;
    WorkingSetID _targetId;

    // Nodes we're moving forward until they hit the element we're AND-ing.
    // Everything in here has not advanced to _targetRecordId yet.
    // These are indices into _children.
    std::queue<size_t> _workingTowardRep;

    // If any child hits EOF or if we have any errors, we're EOF.
    bool _isEOF;

    // Stats
    AndSortedStats _specificStats;
};

}  // namespace mongo
