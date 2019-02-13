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

#include <memory>

#include "mongo/db/exec/collection_scan_common.h"
#include "mongo/db/exec/requires_collection_stage.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/record_id.h"

namespace mongo {

struct Record;
class SeekableRecordCursor;
class WorkingSet;
class OperationContext;

/**
 * Scans over a collection, starting at the RecordId provided in params and continuing until
 * there are no more records in the collection.
 *
 * Preconditions: Valid RecordId.
 */
class CollectionScan final : public RequiresCollectionStage {
public:
    static const char* kStageType;

    CollectionScan(OperationContext* opCtx,
                   const Collection* collection,
                   const CollectionScanParams& params,
                   WorkingSet* workingSet,
                   const MatchExpression* filter);

    StageState doWork(WorkingSetID* out) final;
    bool isEOF() final;

    void doDetachFromOperationContext() final;
    void doReattachToOperationContext() final;

    StageType stageType() const final {
        return STAGE_COLLSCAN;
    }

    Timestamp getLatestOplogTimestamp() const {
        return _latestOplogEntryTimestamp;
    }

    std::unique_ptr<PlanStageStats> getStats() final;

    const SpecificStats* getSpecificStats() const final;

protected:
    void doSaveStateRequiresCollection() final;

    void doRestoreStateRequiresCollection() final;

private:
    /**
     * If the member (with id memberID) passes our filter, set *out to memberID and return that
     * ADVANCED.  Otherwise, free memberID and return NEED_TIME.
     */
    StageState returnIfMatches(WorkingSetMember* member, WorkingSetID memberID, WorkingSetID* out);

    /**
     * Extracts the timestamp from the 'ts' field of 'record', and sets '_latestOplogEntryTimestamp'
     * to that time if it isn't already greater.  Returns an error if the 'ts' field cannot be
     * extracted.
     */
    Status setLatestOplogEntryTimestamp(const Record& record);

    // WorkingSet is not owned by us.
    WorkingSet* _workingSet;

    // The filter is not owned by us.
    const MatchExpression* _filter;

    // If a document does not pass '_filter' but passes '_endCondition', stop scanning and return
    // IS_EOF.
    BSONObj _endConditionBSON;
    std::unique_ptr<GTEMatchExpression> _endCondition;

    std::unique_ptr<SeekableRecordCursor> _cursor;

    CollectionScanParams _params;

    RecordId _lastSeenId;  // Null if nothing has been returned from _cursor yet.

    // If _params.shouldTrackLatestOplogTimestamp is set and the collection is the oplog, the latest
    // timestamp seen in the collection.  Otherwise, this is a null timestamp.
    Timestamp _latestOplogEntryTimestamp;

    // Stats
    CollectionScanStats _specificStats;
};

}  // namespace mongo
