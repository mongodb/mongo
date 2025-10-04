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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/classic/plan_stage.h"
#include "mongo/db/exec/classic/requires_collection_stage.h"
#include "mongo/db/exec/classic/working_set.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/exec/write_stage_common.h"
#include "mongo/db/local_catalog/shard_role_api/shard_role.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/profile_filter.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/db/session/logical_session_id.h"

#include <cstddef>
#include <functional>
#include <memory>

namespace mongo {

class CanonicalQuery;
class OpDebug;
class OperationContext;
class PlanExecutor;

struct DeleteStageParams {
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
    static constexpr StringData kStageType = "DELETE"_sd;

    DeleteStage(ExpressionContext* expCtx,
                std::unique_ptr<DeleteStageParams> params,
                WorkingSet* ws,
                CollectionAcquisition collection,
                PlanStage* child);

    DeleteStage(const char* stageType,
                ExpressionContext* expCtx,
                std::unique_ptr<DeleteStageParams> params,
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

    std::unique_ptr<DeleteStageParams> _params;

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
