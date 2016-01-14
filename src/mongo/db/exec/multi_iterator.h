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

#include <memory>
#include <vector>

#include "mongo/db/catalog/collection.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/record_id.h"

namespace mongo {

/**
 * Iterates over a collection using multiple underlying RecordCursors.
 *
 * This is a special stage which is not used automatically by queries. It is intended for
 * special commands that work with RecordCursors. For example, it is used by the
 * parallelCollectionScan and repairCursor commands
 */
class MultiIteratorStage final : public PlanStage {
public:
    MultiIteratorStage(OperationContext* txn, WorkingSet* ws, Collection* collection);

    void addIterator(std::unique_ptr<RecordCursor> it);

    PlanStage::StageState doWork(WorkingSetID* out) final;

    bool isEOF() final;

    void kill();

    void doSaveState() final;
    void doRestoreState() final;
    void doDetachFromOperationContext() final;
    void doReattachToOperationContext() final;
    void doInvalidate(OperationContext* txn, const RecordId& dl, InvalidationType type) final;

    // Returns empty PlanStageStats object
    std::unique_ptr<PlanStageStats> getStats() final;

    // Not used.
    SpecificStats* getSpecificStats() const final {
        return NULL;
    }

    // Not used.
    StageType stageType() const final {
        return STAGE_MULTI_ITERATOR;
    }

    static const char* kStageType;

private:
    OperationContext* _txn;
    Collection* _collection;
    std::vector<std::unique_ptr<RecordCursor>> _iterators;

    // Not owned by us.
    WorkingSet* _ws;

    // We allocate a working set member with this id on construction of the stage. It gets used for
    // all fetch requests. This should only be used for passing up the Fetcher for a NEED_YIELD, and
    // should remain in the INVALID state.
    const WorkingSetID _wsidForFetch;
};

}  // namespace mongo
