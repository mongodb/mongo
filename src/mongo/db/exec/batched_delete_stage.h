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

#include "mongo/db/exec/batched_delete_stage_buffer.h"
#include "mongo/db/exec/batched_delete_stage_gen.h"
#include "mongo/db/exec/delete_stage.h"
#include "mongo/db/exec/write_stage_common.h"
#include "mongo/db/jsobj.h"

namespace mongo {

struct BatchedDeleteStageParams {
    BatchedDeleteStageParams()
        : targetBatchDocs(gBatchedDeletesTargetBatchDocs.load()),
          targetBatchTimeMS(Milliseconds(gBatchedDeletesTargetBatchTimeMS.load())),
          targetStagedDocBytes(gBatchedDeletesTargetStagedDocBytes.load()),
          targetPassDocs(0),
          targetPassTimeMS(Milliseconds(0)) {}

    //
    // A 'batch' refers to the deletes executed in a single WriteUnitOfWork. A batch of staged
    // document deletes is committed as soon as one of the batch targets is met, or upon reach EOF.
    //
    // 'Batch' targets have no impact on the total number of documents removed in the batched delete
    // operation.
    //

    // Documents staged for deletions are processed in a batch once this document count target is
    // met. A value of zero means unlimited.
    long long targetBatchDocs = 0;

    // A batch is committed as soon as this target execution time is met. Zero means unlimited.
    Milliseconds targetBatchTimeMS = Milliseconds(0);

    // Documents staged for deletions are processed in a batch once this size target is met.
    // Accounts for document size, not for indexes. A value of zero means unlimited.
    long long targetStagedDocBytes = 0;

    //
    // A 'pass' defines a approximate target number of documents or runtime after which the
    // deletion stops staging documents, executes any remaining deletes, and eventually returns
    // completion. 'Pass' parameters are approximate because they are checked at a per batch commit
    // granularity.
    //
    // 'Pass' targets may impact the total number of documents removed in the batched delete
    // operation. When set, there is no guarantee all matching documents will be removed in the
    // operation. For this reason, 'pass' targets are only exposed to internal users for specific
    // use cases.
    //

    // Limits the amount of documents processed in a single pass. Once met, no more documents will
    // be fetched for delete - any remaining staged deletes will be executed provided they still
    // match the query and haven't been deleted by a concurrent operation. A value of zero means
    // unlimited.
    long long targetPassDocs;

    // Limits the time spent staging and executing deletes in a single pass. Once met, no more
    // documents will be fetched for delete - any remaining staged deletes will be executed provided
    // they still match the query and haven't been deleted by a concurrent operation. A value of
    // zero means unlimited.
    Milliseconds targetPassTimeMS;
};

/**
 * The BATCHED_DELETE stage deletes documents in batches. In comparison, the base class DeleteStage
 * deletes documents one by one. The stage returns NEED_TIME after executing a batch of deletes, or
 * after staging a delete for the next batch.
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
                       std::unique_ptr<BatchedDeleteStageParams> batchedDeleteParams,
                       WorkingSet* ws,
                       const CollectionPtr& collection,
                       PlanStage* child);
    ~BatchedDeleteStage();

    // Returns true when no more work can be done (there are no more deletes to commit).
    bool isEOF() final;

    std::unique_ptr<PlanStageStats> getStats() final;

    const SpecificStats* getSpecificStats() const final;

    StageState doWork(WorkingSetID* out);

    StageType stageType() const final {
        return STAGE_BATCHED_DELETE;
    }

private:
    // Returns NEED_TIME when some, or all, of the documents staged in the _stagedDeletesBuffer are
    // successfully deleted. Returns NEED_YIELD otherwise.
    PlanStage::StageState _deleteBatch(WorkingSetID* out);

    // Attempts to delete the documents staged for deletion in a WriteUnitOfWork. Updates
    // recordsToSkip, docsDeleted, and buffferOffset to reflect which document deletes are skipped,
    // executed, or remaining when the WriteUnitOfWork is committed.
    //
    // Returns the time spent (milliseconds) committing the batch.
    long long _commitBatch(WorkingSetID* out,
                           std::set<WorkingSetID>* recordsToSkip,
                           unsigned int* docsDeleted,
                           unsigned int* bytesDeleted,
                           unsigned int* bufferOffset);

    // Attempts to stage a new delete in the _stagedDeletesBuffer. Returns the PlanStage::StageState
    // fetched directly from the child except when there is a document to stage. Converts
    // PlanStage::ADVANCED to PlanStage::NEED_TIME before returning when a document is staged for
    // delete - PlanStage:ADVANCED doesn't hold meaning in a batched delete since nothing will ever
    // be directly returned from this stage.
    PlanStage::StageState _doStaging(WorkingSetID* out);

    // Stages the document tied to workingSetMemberID into the _stagedDeletesBuffer.
    void _stageNewDelete(WorkingSetID* workingSetMemberID);

    // Tries to restore the child's state. Returns NEED_TIME if the restore succeeds, NEED_YIELD
    // otherwise.
    PlanStage::StageState _tryRestoreState(WorkingSetID* out);

    // Prepares to retry draining the _stagedDeletesBuffer after a WriteConflictException or a
    // TemporarilyUnavailableException. Removes 'recordsThatNoLongerMatch' then yields.
    void _prepareToRetryDrainAfterYield(WorkingSetID* out,
                                        const std::set<WorkingSetID>& recordsThatNoLongerMatch);

    BatchedDeleteStats _specificStats;

    // Returns true if one or more of the batch targets are met and it is time to delete the batch.
    bool _batchTargetMet();

    // Returns true if one or more of the pass targets are met and it is time to drain the remaining
    // buffer and return completion. Note - this method checks a timer and repeated calls can become
    // expensive.
    bool _passTargetMet();

    // Batch targeting parameters.
    std::unique_ptr<BatchedDeleteStageParams> _batchedDeleteParams;

    // Holds information for each document staged for delete.
    BatchedDeleteStageBuffer _stagedDeletesBuffer;

    // Holds the maximum cumulative size of all documents staged for delete. It is a watermark in
    // that it resets to zero once the target is met and the staged documents start being processed,
    // regardless of whether all staged deletes have been committed yet.
    size_t _stagedDeletesWatermarkBytes;

    // Tracks the cumulative number of documents staged for deletes over the operation.
    long long _passTotalDocsStaged;

    // Tracks the cumulative elapsed time since the operation began.
    Timer _passTimer;

    // True when the deletes in the buffer must be committed before more documents can be staged.
    bool _commitStagedDeletes;

    // True when the operation is done staging new documents. The only work left is to drain the
    // remaining buffer.
    bool _passStagingComplete;
};

}  // namespace mongo
