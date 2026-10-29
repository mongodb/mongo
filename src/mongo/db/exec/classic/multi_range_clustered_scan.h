// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/classic/plan_stage.h"
#include "mongo/db/exec/classic/requires_collection_stage.h"
#include "mongo/db/exec/classic/working_set.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/db/query/record_id_range_list.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/util/modules.h"

#include <memory>

namespace mongo {

struct Record;
class SeekableRecordCursor;
class WorkingSet;
class OperationContext;

/**
 * Parameters for MultiRangeClusteredScan. Restricted to the features needed for non-contiguous
 * RecordId scans on clustered collections — does not support oplog tracking, tailable cursors,
 * resume tokens, or initial-sync visibility waits.
 */
struct [[MONGO_MOD_PUBLIC]] MultiRangeClusteredScanParams {
    // The set of RecordId ranges to scan, in value order. Each range encodes its own
    // inclusivity on min/max. A default-constructed list is unbounded.
    RecordIdRangeList rangeList;

    CollectionScanParams::Direction direction = CollectionScanParams::FORWARD;
};

/**
 * Scans a clustered collection over a list of RecordId ranges. The ranges are expected to be
 * disjoint and ordered (in value order); the stage walks them in scan order, seeking from the
 * end of one range to the start of the next.
 */
class MultiRangeClusteredScan final : public RequiresCollectionStage {
public:
    MultiRangeClusteredScan(ExpressionContext* expCtx,
                            CollectionAcquisition collection,
                            const MultiRangeClusteredScanParams& params,
                            WorkingSet* workingSet,
                            const MatchExpression* filter);

    StageState doWork(WorkingSetID* out) final;
    bool isEOF() const final;

    void doDetachFromOperationContext() final;
    void doReattachToOperationContext() final;

    StageType stageType() const final {
        return STAGE_COLLSCAN_MULTI_RANGE;
    }

    std::unique_ptr<PlanStageStats> getStats() final;
    const SpecificStats* getSpecificStats() const final;

    CollectionScanParams::Direction getDirection() const {
        return _params.direction;
    }

    const MultiRangeClusteredScanParams& params() const {
        return _params;
    }

    bool initializedCursor() const {
        return _cursor != nullptr;
    }

protected:
    void doSaveStateRequiresCollection() final;
    void doRestoreStateRequiresCollection() final;

private:
    StageState returnIfMatches(WorkingSetMember* member, WorkingSetID memberID, WorkingSetID* out);

    WorkingSet* _workingSet;
    const MatchExpression* _filter;

    std::unique_ptr<SeekableRecordCursor> _cursor;

    MultiRangeClusteredScanParams _params;

    // Index into _params.rangeList.getRanges() for the range currently being scanned. Starts at 0
    // for forward scans, ranges.size()-1 for backward scans.
    size_t _currentRangeIdx = 0;

    // If true, the next doWork() call should seek to the start of
    // _params.rangeList.getRanges()[_currentRangeIdx] before advancing the cursor.
    bool _pendingSeek = false;

    CollectionScanStats _specificStats;
};

}  // namespace mongo
