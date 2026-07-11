// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/admission/execution_control/execution_admission_context.h"
#include "mongo/db/exec/classic/plan_stage.h"
#include "mongo/db/exec/classic/requires_collection_stage.h"
#include "mongo/db/exec/classic/working_set.h"
#include "mongo/db/exec/collection_scan_common.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/db/query/oplog_wait_config.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/record_id.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/util/modules.h"

#include <memory>

#include <boost/optional/optional.hpp>

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
                   CollectionAcquisition collection,
                   const CollectionScanParams& params,
                   WorkingSet* workingSet,
                   const MatchExpression* filter);

    StageState doWork(WorkingSetID* out) final;
    bool isEOF() const final;

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

    const CollectionScanParams& params() const {
        return _params;
    }

    bool initializedCursor() const {
        return _cursor != nullptr;
    }

    OplogWaitConfig* getOplogWaitConfig() {
        return _oplogWaitConfig ? &(*_oplogWaitConfig) : nullptr;
    }

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
     * to that time if it isn't already greater. Throws an exception if the 'ts' field cannot be
     * extracted.
     */
    void setLatestOplogEntryTimestamp(const Record& record);

    /**
     * Set up the cursor.
     */
    void initCursor(OperationContext* opCtx, const CollectionPtr& collPtr, bool forward);

    // WorkingSet is not owned by us.
    WorkingSet* _workingSet;

    // The filter is not owned by us.
    const MatchExpression* _filter;

    std::unique_ptr<SeekableRecordCursor> _cursor;

    CollectionScanParams _params;

    RecordId _lastSeenId;  // Null if nothing has been returned from _cursor yet.

    // If _params.shouldTrackLatestOplogTimestamp is set and the collection is the oplog, this is
    // the latest timestamp seen by the collection scan.
    Timestamp _latestOplogEntryTimestamp;

    boost::optional<ScopedAdmissionPriority<ExecutionAdmissionContext>> _priority;

    // Stats
    CollectionScanStats _specificStats;

    // Coordinates waiting for oplog visibility. Must be initialized if we are doing an oplog scan,
    // boost::none otherwise.
    boost::optional<OplogWaitConfig> _oplogWaitConfig;
};

}  // namespace mongo
