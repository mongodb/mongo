// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/classic/plan_stage.h"
#include "mongo/db/exec/classic/requires_collection_stage.h"
#include "mongo/db/exec/classic/working_set.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/exec/write_stage_common.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/profile_filter.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <string_view>

namespace mongo {
using namespace std::literals::string_view_literals;

class CanonicalQuery;
class OpDebug;
class OperationContext;
class PlanExecutor;

struct [[MONGO_MOD_PUBLIC]] DeleteStageParams {
    using DocumentCounter = std::function<size_t(const BSONObj&)>;

    DeleteStageParams()
        : isMulti(false),
          fromMigrate(false),
          isExplain(false),
          returnDeleted(false),
          canonicalQuery(nullptr),
          opDebug(nullptr) {}

    // Should we delete all documents returned from the child (a "multi delete"), or at most one
    // (a "single delete")?
    bool isMulti;

    // Is this delete part of a migrate operation that is essentially like a no-op
    // when the cluster is observed by an external client.
    bool fromMigrate;

    // Are we explaining a delete command rather than actually executing it?
    bool isExplain;

    // Should we return the document we just deleted?
    bool returnDeleted;

    // The stmtId for this particular delete.
    StmtId stmtId = kUninitializedStmtId;

    // The parsed query predicate for this delete. Not owned here.
    CanonicalQuery* canonicalQuery;

    // The user-requested sort specification. Currently used just for findAndModify.
    BSONObj sort;

    // Optional. When not null, delete metrics are recorded here.
    OpDebug* opDebug;

    // Determines how the delete stats should be incremented. Will be incremented by 1 if the
    // function is empty.
    DocumentCounter numStatsForDoc;
};

/**
 * This stage delete documents by RecordId that are returned from its child. If the deleted
 * document was requested to be returned, then ADVANCED is returned after deleting a document.
 * Otherwise, NEED_TIME is returned after deleting a document.
 *
 * Callers of work() must be holding a write lock (and, for replicated deletes, callers must have
 * had the replication coordinator approve the write).
 */
class DeleteStage : public RequiresWritableCollectionStage {
    DeleteStage(const DeleteStage&) = delete;
    DeleteStage& operator=(const DeleteStage&) = delete;

public:
    static constexpr std::string_view kStageType = "DELETE"sv;

    DeleteStage(ExpressionContext* expCtx,
                DeleteStageParams params,
                WorkingSet* ws,
                CollectionAcquisition collection,
                PlanStage* child);

    DeleteStage(std::string_view stageType,
                ExpressionContext* expCtx,
                DeleteStageParams params,
                WorkingSet* ws,
                CollectionAcquisition collection,
                PlanStage* child);

    bool isEOF() const override;
    StageState doWork(WorkingSetID* out) override;

    StageType stageType() const override {
        return STAGE_DELETE;
    }

    std::unique_ptr<mongo::PlanStageStats> getStats() override;

    const SpecificStats* getSpecificStats() const override;

protected:
    void doSaveStateRequiresCollection() final {
        _preWriteFilter.saveState();
    }

    void doRestoreStateRequiresCollection() final;

    DeleteStageParams _params;

    // Not owned by us.
    WorkingSet* _ws;

    // Stats
    DeleteStats _specificStats;

    /**
     * This member is used to check whether the write should be performed, and if so, any other
     * behavior that should be done as part of the write (e.g. skipping it because it affects an
     * orphan document). A yield cannot happen between the check and the write, so the checks are
     * embedded in the stage.
     *
     * It's refreshed after yielding and reacquiring the locks.
     */
    write_stage_common::PreWriteFilter _preWriteFilter;

private:
    /**
     * Stores 'idToRetry' in '_idRetrying' so the delete can be retried during the next call to
     * work(). Sets 'out' to WorkingSet::INVALID_ID.
     */
    void prepareToRetryWSM(WorkingSetID idToRetry, WorkingSetID* out);

    // If not WorkingSet::INVALID_ID, we use this rather than asking our child what to do next.
    WorkingSetID _idRetrying;

    // If not WorkingSet::INVALID_ID, we return this member to our caller.
    WorkingSetID _idReturning;
};

}  // namespace mongo
