/**
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once


#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/record_id.h"

namespace mongo {

/**
 * KeepMutationsStage passes all of its child's data through until the child is EOF.
 * It then returns all flagged elements in the WorkingSet that pass the stage's filter.
 *
 * This stage is used to merge results that are invalidated mid-query back into the query
 * results when possible.  The query planner is responsible for determining when it's valid to
 * merge these results.
 */
class KeepMutationsStage final : public PlanStage {
public:
    KeepMutationsStage(OperationContext* opCtx,
                       const MatchExpression* filter,
                       WorkingSet* ws,
                       PlanStage* child);
    ~KeepMutationsStage();

    bool isEOF() final;
    StageState doWork(WorkingSetID* out) final;

    StageType stageType() const final {
        return STAGE_KEEP_MUTATIONS;
    }

    std::unique_ptr<PlanStageStats> getStats() final;

    const SpecificStats* getSpecificStats() const final;

    static const char* kStageType;

private:
    // Not owned here.
    WorkingSet* _workingSet;

    // Not owned here.  Should be the full query expression tree.
    const MatchExpression* _filter;

    // We read from our child...
    bool _doneReadingChild;

    // ...until it's out of results, at which point we put any flagged results back in the query
    // stream.
    bool _doneReturningFlagged;

    // Our copy of the working set's flagged results.
    std::vector<WorkingSetID> _flagged;

    // Iterator pointing into _flagged.
    std::vector<WorkingSetID>::const_iterator _flaggedIterator;
};

}  // namespace mongo
