/**
 *    Copyright (C) 2013-2014 MongoDB Inc.
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

#include "mongo/db/exec/collection_scan_common.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/record_id.h"

namespace mongo {

class RecordCursor;
class WorkingSet;
class OperationContext;

/**
 * Scans over a collection, starting at the RecordId provided in params and continuing until
 * there are no more records in the collection.
 *
 * Preconditions: Valid RecordId.
 */
class CollectionScan : public PlanStage {
public:
    CollectionScan(OperationContext* txn,
                   const CollectionScanParams& params,
                   WorkingSet* workingSet,
                   const MatchExpression* filter);

    virtual StageState work(WorkingSetID* out);
    virtual bool isEOF();

    virtual void invalidate(OperationContext* txn, const RecordId& dl, InvalidationType type);
    virtual void saveState();
    virtual void restoreState(OperationContext* opCtx);

    virtual std::vector<PlanStage*> getChildren() const;

    virtual StageType stageType() const {
        return STAGE_COLLSCAN;
    }

    virtual std::unique_ptr<PlanStageStats> getStats();

    virtual const CommonStats* getCommonStats() const;

    virtual const SpecificStats* getSpecificStats() const;

    static const char* kStageType;

private:
    /**
     * If the member (with id memberID) passes our filter, set *out to memberID and return that
     * ADVANCED.  Otherwise, free memberID and return NEED_TIME.
     */
    StageState returnIfMatches(WorkingSetMember* member, WorkingSetID memberID, WorkingSetID* out);

    // transactional context for read locks. Not owned by us
    OperationContext* _txn;

    // WorkingSet is not owned by us.
    WorkingSet* _workingSet;

    // The filter is not owned by us.
    const MatchExpression* _filter;

    std::unique_ptr<RecordCursor> _cursor;

    CollectionScanParams _params;

    bool _isDead;

    RecordId _lastSeenId;  // Null if nothing has been returned from _cursor yet.

    // We allocate a working set member with this id on construction of the stage. It gets
    // used for all fetch requests, changing the RecordId as appropriate.
    const WorkingSetID _wsidForFetch;

    // Stats
    CommonStats _commonStats;
    CollectionScanStats _specificStats;
};

}  // namespace mongo
