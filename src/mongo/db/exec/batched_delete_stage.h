/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/exec/batched_delete_stage_gen.h"
#include "mongo/db/exec/delete_stage.h"
#include "mongo/db/exec/write_stage_common.h"
#include "mongo/db/jsobj.h"

namespace mongo {

/**
 * Batch sizing parameters. A batch of documents staged for deletion is committed as soon
 * as one of the targets below is met, or upon reaching EOF.
 */
struct BatchedDeleteStageBatchParams {
    BatchedDeleteStageBatchParams()
        : targetBatchBytes(gBatchedDeletesTargetBatchBytes.load()),
          targetBatchDocs(gBatchedDeletesTargetBatchDocs.load()),
          targetBatchTimeMS(Milliseconds(gBatchedDeletesTargetBatchTimeMS.load())) {}

    // Documents staged for deletions are processed in a batch once this batch size target is met.
    // Accounts for documents and indexes. A value of zero means unlimited.
    long long targetBatchBytes = 0;
    // Documents staged for deletions are processed in a batch once this document count target is
    // met. A value of zero means unlimited.
    long long targetBatchDocs = 0;
    // A batch is committed as soon as this target execution time is met. Zero means unlimited.
    Milliseconds targetBatchTimeMS = Milliseconds(0);
};

/**
 * The BATCHED_DELETE stage deletes documents in batches, using RecordId's that are returned from
 * its child. In comparison, the base class DeleteStage deletes documents one by one. The stage
 * returns NEED_TIME after deleting a document, or after staging a document to be deleted in the
 * next batch.
 *
 * Callers of work() must be holding a write lock (and, for replicated deletes, callers must have
 * had the replication coordinator approve the write).
 */
class BatchedDeleteStage final : public DeleteStage {
    BatchedDeleteStage(const BatchedDeleteStage&) = delete;
    BatchedDeleteStage& operator=(const BatchedDeleteStage&) = delete;

public:
    static constexpr StringData kStageType = "BATCHED_DELETE"_sd;

    BatchedDeleteStage(ExpressionContext* expCtx,
                       std::unique_ptr<DeleteStageParams> params,
                       std::unique_ptr<BatchedDeleteStageBatchParams> batchParams,
                       WorkingSet* ws,
                       const CollectionPtr& collection,
                       PlanStage* child);
    ~BatchedDeleteStage();

    StageState doWork(WorkingSetID* out);

    StageType stageType() const final {
        return STAGE_BATCHED_DELETE;
    }

private:
    /**
     * Deletes the documents staged in _ridMap in a batch.
     * Returns NEED_TIME on success.
     */
    PlanStage::StageState _deleteBatch(WorkingSetID* out);

    // Maps records to delete to the latest snapshot their data matched the query. Records must be
    // deleted in a single WriteUnitOrWork. Operation order has no impact on the outcome of the
    // WriteUnitOfWork since all operations become visible at the same time.
    stdx::unordered_map<RecordId, SnapshotId, RecordId::Hasher> _ridMap;

    // Whether there are remaining docs in the buffer from a previous call to doWork() that should
    // be drained before fetching more documents.
    bool _drainRemainingBuffer = false;

    // Batch targeting parameters.
    std::unique_ptr<BatchedDeleteStageBatchParams> _batchParams;
};

}  // namespace mongo
