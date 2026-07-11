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
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string_view>

namespace mongo {
using namespace std::literals::string_view_literals;

class SeekableRecordCursor;

/**
 * This stage turns a RecordId into a BSONObj.
 *
 * In WorkingSetMember terms, it transitions from RID_AND_IDX to RID_AND_OBJ by reading
 * the record at the provided RecordId.  Returns verbatim any data that already has an object.
 *
 * Preconditions: Valid RecordId.
 */
class FetchStage : public RequiresCollectionStage {
public:
    FetchStage(ExpressionContext* expCtx,
               WorkingSet* ws,
               std::unique_ptr<PlanStage> child,
               const MatchExpression* filter,
               CollectionAcquisition collection);

    ~FetchStage() override;

    bool isEOF() const final;
    StageState doWork(WorkingSetID* out) final;

    void doDetachFromOperationContext() final;
    void doReattachToOperationContext() final;

    StageType stageType() const final {
        return STAGE_FETCH;
    }

    std::unique_ptr<PlanStageStats> getStats() override;

    const SpecificStats* getSpecificStats() const final;

    static constexpr std::string_view kStageType = "FETCH"sv;

protected:
    void doSaveStateRequiresCollection() final;

    void doRestoreStateRequiresCollection() final;

private:
    /**
     * If the member (with id memberID) passes our filter, set *out to memberID and return that
     * ADVANCED.  Otherwise, free memberID and return NEED_TIME.
     */
    StageState returnIfMatches(WorkingSetMember* member, WorkingSetID memberID, WorkingSetID* out);

    // Used to fetch Records from _collection.
    std::unique_ptr<SeekableRecordCursor> _cursor;

    // _ws is not owned by us.
    WorkingSet* _ws;

    // The filter is not owned by us.
    const MatchExpression* _filter;

    // If not Null, we use this rather than asking our child what to do next.
    WorkingSetID _idRetrying;

    // Stats
    FetchStats _specificStats;
};

}  // namespace mongo
