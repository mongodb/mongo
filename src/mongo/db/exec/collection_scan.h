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

#include <boost/optional/optional.hpp>
#include <memory>

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/exec/collection_scan_common.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/exec/requires_collection_stage.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/stage_types.h"
#include "mongo/db/record_id.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/s/resharding/resume_token_gen.h"

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
    CollectionScan(ExpressionContext* expCtx,
                   VariantCollectionPtrOrAcquisition collection,
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

    BSONObj getPostBatchResumeToken() const;

    std::unique_ptr<PlanStageStats> getStats() final;

    const SpecificStats* getSpecificStats() const final;

    CollectionScanParams::Direction getDirection() const {
        return _params.direction;
    }

protected:
    void doSaveStateRequiresCollection() final;

    void doRestoreStateRequiresCollection() final;

private:
    /**
     * If the member (with id memberID) passes our filter, set *out to memberID and return that
     * ADVANCED.  Otherwise, free memberID and return NEED_TIME.
     */
    StageState returnIfMatches(WorkingSetMember* member,
                               WorkingSetID memberID,
                               WorkingSetID* out,
                               bool needsStartBoundCheck);

    /**
     * Sets '_latestOplogEntryTimestamp' to the current read timestamp, if available. This is
     * equivalent to the latest visible timestamp in the oplog.
     */
    void setLatestOplogEntryTimestampToReadTimestamp();

    /**
     * Extracts the timestamp from the 'ts' field of 'record', and sets '_latestOplogEntryTimestamp'
     * to that time if it isn't already greater. Throws an exception if the 'ts' field cannot be
     * extracted.
     */
    void setLatestOplogEntryTimestamp(const Record& record);

    /**
     * Asserts that the minimum timestamp in the query filter has not already fallen off the oplog.
     */
    void assertTsHasNotFallenOff(const Record& record);

    // WorkingSet is not owned by us.
    WorkingSet* _workingSet;

    // The filter is not owned by us.
    const MatchExpression* _filter;

    std::unique_ptr<SeekableRecordCursor> _cursor;

    CollectionScanParams _params;

    RecordId _lastSeenId;  // Null if nothing has been returned from _cursor yet.

    // If _params.shouldTrackLatestOplogTimestamp is set and the collection is the oplog or a change
    // collection, this is the latest timestamp seen by the collection scan. For change collections,
    // on EOF we advance this timestamp to the latest timestamp in the global oplog.
    Timestamp _latestOplogEntryTimestamp;

    boost::optional<ScopedAdmissionPriority> _priority;

    // Stats
    CollectionScanStats _specificStats;

    bool _useSeek = false;
};

}  // namespace mongo
