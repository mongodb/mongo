/**
 *    Copyright (C) 2013 10gen Inc.
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

#include <queue>

#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/exec/working_set.h"

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
    QueuedDataStage(OperationContext* opCtx, WorkingSet* ws);

    StageState doWork(WorkingSetID* out) final;

    bool isEOF() final;

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
     * Note: do not add PlanStage::ADVANCED with this method, ADVANCED can
     * only be added with a data member.
     *
     * Work() goes through the queue.
     * Either no data is returned (just a state), or...
     */
    void pushBack(const PlanStage::StageState state);

    /**
     * ...data is returned (and we ADVANCED)
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
    // We don't own this.
    WorkingSet* _ws;

    // The data we return.
    std::queue<PlanStage::StageState> _results;
    std::queue<WorkingSetID> _members;

    // Stats
    MockStats _specificStats;
};

}  // namespace mongo
