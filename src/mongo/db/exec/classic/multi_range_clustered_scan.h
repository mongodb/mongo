/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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
struct MONGO_MOD_PUBLIC MultiRangeClusteredScanParams {
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
