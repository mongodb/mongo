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


#include "mongo/db/exec/classic/batched_delete_stage.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/collection_crud/collection_write_path.h"
#include "mongo/db/commands/server_status/server_status.h"
#include "mongo/db/exec/classic/plan_stage.h"
#include "mongo/db/exec/classic/working_set.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/write_stage_common.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/index_catalog.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_executor_impl.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/exceptions.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/snapshot.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/versioning_protocol/shard_version.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>


#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kWrite


namespace mongo {

MONGO_FAIL_POINT_DEFINE(throwWriteConflictExceptionInBatchedDeleteStage);
MONGO_FAIL_POINT_DEFINE(batchedDeleteStageSleepAfterNDocuments);
MONGO_FAIL_POINT_DEFINE(batchedDeleteStageThrowTemporarilyUnavailableException);

namespace {

/**
 * Constants that (conservatively) estimate the size of the oplog entry that would result from
 * committing a batch, so as to ensure that a batch fits within a 16MB oplog entry. These constants
 * translate to a maximum of ~63k documents deleted per batch on non-clustered collections.
 */
// Size of an array member of an applyOps entry, excluding the RecordId. Accounts for the maximum
// size of the internal fields.
static size_t kApplyOpsNonArrayEntryPaddingBytes = 256;
// Size of an applyOps entry, excluding its array.
static size_t kApplyOpsArrayEntryPaddingBytes = 256;

void incrementSSSMetricNoOverflow(AtomicWord<long long>& metric, long long value) {
    const int64_t MAX = 1ULL << 60;

    if (metric.loadRelaxed() > MAX) {
        metric.store(value);
    } else {
        metric.fetchAndAdd(value);
    }
}
}  // namespace

/**
 * Reports globally-aggregated batch stats.
 */
struct BatchedDeletesSSS : ServerStatusSection {
    using ServerStatusSection::ServerStatusSection;

    bool includeByDefault() const override {
        return true;
    }

    BSONObj generateSection(OperationContext* opCtx, const BSONElement& configElem) const override {
        BSONObjBuilder bob;
        bob.appendNumber("batches", batches.loadRelaxed());
        bob.appendNumber("docs", docs.loadRelaxed());
        bob.appendNumber("stagedSizeBytes", stagedSizeBytes.loadRelaxed());
        bob.appendNumber("timeInBatchMillis", timeInBatchMillis.loadRelaxed());
        bob.appendNumber("refetchesDueToYield", refetchesDueToYield.loadRelaxed());

        return bob.obj();
    }

    AtomicWord<long long> batches{0};
    AtomicWord<long long> docs{0};
    AtomicWord<long long> stagedSizeBytes{0};
    AtomicWord<long long> timeInBatchMillis{0};
    AtomicWord<long long> refetchesDueToYield{0};
};
auto& batchedDeletesSSS =
    *ServerStatusSectionBuilder<BatchedDeletesSSS>("batchedDeletes").forShard();

// Wrapper for write_stage_common::ensureStillMatches() which also updates the 'refetchesDueToYield'
// serverStatus metric. As with ensureStillMatches, if false is returned, the WorkingSetMember
// referenced by 'id' is no longer valid, and must not be used except for freeing the WSM.
bool ensureStillMatchesAndUpdateStats(const CollectionPtr& collection,
                                      OperationContext* opCtx,
                                      WorkingSet* ws,
                                      WorkingSetID id,
                                      const CanonicalQuery* cq) {
    WorkingSetMember* member = ws->get(id);
    if (shard_role_details::getRecoveryUnit(opCtx)->getSnapshotId() != member->doc.snapshotId()) {
        incrementSSSMetricNoOverflow(batchedDeletesSSS.refetchesDueToYield, 1);
    }
    return write_stage_common::ensureStillMatches(collection, opCtx, ws, id, cq);
}

BatchedDeleteStage::BatchedDeleteStage(
    ExpressionContext* expCtx,
    std::unique_ptr<DeleteStageParams> params,
    std::unique_ptr<BatchedDeleteStageParams> batchedDeleteParams,
    WorkingSet* ws,
    CollectionAcquisition collection,
    PlanStage* child)
    : DeleteStage::DeleteStage(kStageType.data(), expCtx, std::move(params), ws, collection, child),
      _batchedDeleteParams(std::move(batchedDeleteParams)),
      _stagedDeletesBuffer(ws),
      _stagedDeletesWatermarkBytes(0),
      _passTotalDocsStaged(0),
      _passTimer(expCtx->getOperationContext()->getServiceContext()->getTickSource()),
      _commitStagedDeletes(false),
      _passStagingComplete(false) {
    tassert(6303800,
            "batched deletions only support multi-document deletions (multi: true)",
            _params->isMulti);
    tassert(
        6303803, "batched deletions do not support the 'sort' parameter", _params->sort.isEmpty());
    tassert(6303805,
            "batched deletions do not support the 'numStatsForDoc' parameter",
            !_params->numStatsForDoc);
    tassert(6303807,
            "batch size parameters must be greater than or equal to zero",
            _batchedDeleteParams->targetStagedDocBytes >= 0 &&
                _batchedDeleteParams->targetBatchDocs >= 0 &&
                _batchedDeleteParams->targetBatchTimeMS >= Milliseconds(0));
}

BatchedDeleteStage::~BatchedDeleteStage() {}

bool BatchedDeleteStage::isEOF() const {
    return _stagedDeletesBuffer.empty() && _passStagingComplete;
}

std::unique_ptr<PlanStageStats> BatchedDeleteStage::getStats() {
    _commonStats.isEOF = isEOF();
    std::unique_ptr<PlanStageStats> stats =
        std::make_unique<PlanStageStats>(_commonStats, stageType());
    stats->specific = std::make_unique<BatchedDeleteStats>(_specificStats);
    stats->children.emplace_back(child()->getStats());
    return stats;
}

const SpecificStats* BatchedDeleteStage::getSpecificStats() const {
    return &_specificStats;
}

PlanStage::StageState BatchedDeleteStage::doWork(WorkingSetID* out) {
    WorkingSetID idToReturn = WorkingSet::INVALID_ID;
    PlanStage::StageState planStageState = PlanStage::NEED_TIME;

    if (!_commitStagedDeletes && !_passStagingComplete) {
        // It's okay to stage more documents.
        planStageState = _doStaging(&idToReturn);

        _passStagingComplete = planStageState == PlanStage::IS_EOF;
        _commitStagedDeletes = _passStagingComplete || _batchTargetMet();
    }

    if (!_params->isExplain && _commitStagedDeletes) {
        // Overwriting 'planStageState' potentially means throwing away the result produced from
        // staging. We expect to commit deletes after a new document is staged and the batch targets
        // are met (planStageState = PlanStage::NEED_TIME), after there are no more documents to
        // stage (planStageState = PlanStage::IS_EOF), or when resuming to commit deletes in the
        // buffer before more can be staged (planStageState = PlanStage::NEED_TIME by default).
        //
        // Enforce that if staging occurred, the resulting 'planStageState' is only overwritten when
        // we should be committing deletes.
        tassert(6304300,
                "Fetched unexpected plan stage state before committing deletes",
                planStageState == PlanStage::NEED_TIME || planStageState == PlanStage::IS_EOF);

        _stagedDeletesWatermarkBytes = 0;
        planStageState = _deleteBatch(&idToReturn);

        if (!_passStagingComplete) {
            tassert(6621000, "Expected staging to be permitted", !_specificStats.passTargetMet);

            // Staging hasn't yielded PlanStage::IS_EOF.
            //
            // Indicate whether the operation should reach completion due to a pass target.
            _specificStats.passTargetMet = _passTargetMet();
        }

        _passStagingComplete = _passStagingComplete || _specificStats.passTargetMet;
        _commitStagedDeletes = _passStagingComplete || !_stagedDeletesBuffer.empty();
    }

    // We need to check if 'planStateStage' is 'NEED_YIELD' earlier than we check 'isEOF()'.
    // The reason is that 'isEOF()' returns true if we have completed staging and have an empty
    // buffer of staged deletes. However, even if 'isEOF()' returns true, the 'planStateStage' can
    // be 'NEED_YIELD' here if inside 'restoreState()' in '_deleteBatch()' a
    // 'StorageEngineException' was thrown and caught. In this case, the 'planStateStage' is
    // 'NEED_YIELD' and 'isEOF()' already returns true.
    if (planStageState == PlanStage::NEED_YIELD) {
        *out = idToReturn;
        return PlanStage::NEED_YIELD;
    }

    if (isEOF()) {
        invariant(planStageState != PlanStage::NEED_YIELD);
        return PlanStage::IS_EOF;
    }

    return planStageState;
}

PlanStage::StageState BatchedDeleteStage::_deleteBatch(WorkingSetID* out) {
    if (!_stagedDeletesBuffer.size()) {
        return PlanStage::NEED_TIME;
    }

    const auto saveRet = handlePlanStageYield(
        expCtx(),
        "BatchedDeleteStage saveState",
        [&] {
            child()->saveState();
            return PlanStage::NEED_TIME;
        },
        [&] {
            // yieldHandler
            _prepareToRetryDrainAfterYield(out, {});
        });
    if (saveRet != PlanStage::NEED_TIME) {
        return saveRet;
    }

    std::set<WorkingSetID> recordsToSkip;
    unsigned int docsDeleted = 0;
    unsigned int bytesDeleted = 0;
    unsigned int rBufferOffset = 0;
    long long timeInBatch = 0;

    try {
        const auto ret = handlePlanStageYield(
            expCtx(),
            "BatchedDeleteStage::_deleteBatch",
            [&] {
                timeInBatch =
                    _commitBatch(out, &recordsToSkip, &docsDeleted, &bytesDeleted, &rBufferOffset);
                return PlanStage::NEED_TIME;
            },
            [&] {
                // yieldHandler
                _prepareToRetryDrainAfterYield(out, recordsToSkip);
            });

        if (ret != PlanStage::NEED_TIME) {
            return ret;
        }
    } catch (const ExceptionFor<ErrorCodes::StaleConfig>& ex) {
        if (ShardVersion::isPlacementVersionIgnored(ex->getVersionReceived()) &&
            ex->getCriticalSectionSignal()) {
            // If the placement version is IGNORED and we encountered a critical section, then
            // yield, wait for critical section to finish and then we'll resume the write from the
            // point we had left. We do this to prevent large multi-writes from repeatedly failing
            // due to StaleConfig and exhausting the mongos retry attempts.
            planExecutorShardingState(opCtx()).criticalSectionFuture =
                ex->getCriticalSectionSignal();
            _prepareToRetryDrainAfterYield(out, recordsToSkip);
            return PlanStage::NEED_YIELD;
        }
        throw;
    }

    incrementSSSMetricNoOverflow(batchedDeletesSSS.docs, docsDeleted);
    incrementSSSMetricNoOverflow(batchedDeletesSSS.batches, 1);
    incrementSSSMetricNoOverflow(batchedDeletesSSS.timeInBatchMillis, timeInBatch);
    _specificStats.docsDeleted += docsDeleted;
    _specificStats.bytesDeleted += bytesDeleted;

    // Note: 'rBufferOffset' stores the 0-based index of the last successfully processed buffer
    // entry, which is why the total number of documents processed is 1 more.
    if (auto docsProcessed = rBufferOffset + 1; docsProcessed < _stagedDeletesBuffer.size()) {
        // targetBatchTimeMS was met. Remove staged deletes that have been evaluated
        // (executed or skipped because they no longer match the query) from the buffer. If any
        // staged deletes remain in the buffer, they will be retried in a subsequent batch.
        _stagedDeletesBuffer.removeLastN(docsProcessed);
    } else {
        // The individual deletes staged in the buffer are preserved until the batch is committed so
        // they can be retried in case of a write conflict.
        // No write conflict occurred, all staged deletes were successfully evaluated/executed, it
        // is safe to clear the buffer.
        _stagedDeletesBuffer.clear();
    }

    return _tryRestoreState(out);
}

long long BatchedDeleteStage::_commitBatch(WorkingSetID* out,
                                           std::set<WorkingSetID>* recordsToSkip,
                                           unsigned int* docsDeleted,
                                           unsigned int* bytesDeleted,
                                           unsigned int* rBufferOffset) {
    // Estimate the size of the oplog entry that would result from committing the batch,
    // to ensure we emit an oplog entry that's within the 16MB BSON limit.
    size_t applyOpsBytes = kApplyOpsNonArrayEntryPaddingBytes;

    Timer batchTimer(opCtx()->getServiceContext()->getTickSource());

    // Start a WUOW with 'groupOplogEntries' which groups a delete batch into a single timestamp
    // and oplog entry.
    WriteUnitOfWork wuow(opCtx(),
                         _stagedDeletesBuffer.size() > 1U ? WriteUnitOfWork::kGroupForTransaction
                                                          : WriteUnitOfWork::kDontGroup);
    // We iterate pending deletes in reverse order of staging to work around duplicate deletions
    // that can result when the same document gets staged twice. When the batch of documents to
    // delete comes from an index scan, it can contain duplicates in the rare case that a yield
    // during the index scan coincides with an update to a document that advances its position
    // in the index. This loop accounts for that possibility by refetching the document to
    // verify that it still exists and should be deleted, but only when the staged deletion is
    // from a prior snapshot, which leaves open the possibility that the second of two deletions
    // does not get verified because it is from the current snapshot. Iterating in reverse
    // ensures that, in these rare cases, the second time we encounter a document, it will always
    // be from a prior snapshot and checked. Avoiding duplicate deletions is important because
    // 'collection_level::deleteDocument()' expects its target to exist, and validation errors
    // can result when it does not.
    for (; *rBufferOffset < _stagedDeletesBuffer.size(); ++*rBufferOffset) {
        if (MONGO_unlikely(throwWriteConflictExceptionInBatchedDeleteStage.shouldFail())) {
            throwWriteConflictException(
                str::stream() << "Hit failpoint '"
                              << throwWriteConflictExceptionInBatchedDeleteStage.getName() << "'.");
        }

        auto workingSetMemberID =
            _stagedDeletesBuffer.at(_stagedDeletesBuffer.size() - 1 - *rBufferOffset);
        WorkingSetMember* member = _ws->get(workingSetMemberID);

        bool writeToOrphan = _params->fromMigrate;
        if (!_params->fromMigrate) {
            using write_stage_common::PreWriteFilter;
            const PreWriteFilter::Action action = [&]() {
                // The PlanExecutor YieldPolicy may change snapshots between calls to 'doWork()'.
                // Different documents may have different snapshots.
                const bool docStillMatches = ensureStillMatchesAndUpdateStats(
                    collectionPtr(), opCtx(), _ws, workingSetMemberID, _params->canonicalQuery);

                // Warning: if docStillMatches is false, the WSM's underlying Document/BSONObj is no
                // longer valid.
                if (!docStillMatches) {
                    return PreWriteFilter::Action::kSkip;
                }

                // Determine whether the document being deleted is owned by this shard, and the
                // action to undertake if it isn't.
                return _preWriteFilter.computeActionAndLogSpecialCases(
                    member->doc.value(), "batched delete"_sd, collectionPtr()->ns());
            }();

            // Skip the document, as it either no longer exists, or has been filtered by the
            // PreWriteFilter.
            if (action == PreWriteFilter::Action::kSkip) {
                recordsToSkip->insert(workingSetMemberID);
                continue;
            }
            writeToOrphan = action == PreWriteFilter::Action::kWriteAsFromMigrate;
        }

        auto retryableWrite = write_stage_common::isRetryableWrite(opCtx());
        Snapshotted<Document> memberDoc = member->doc;
        BSONObj bsonObjDoc = memberDoc.value().toBson();
        applyOpsBytes += kApplyOpsArrayEntryPaddingBytes;
        tassert(
            6515700, "Expected document to have an _id field present", bsonObjDoc.hasField("_id"));
        applyOpsBytes += bsonObjDoc.getField("_id").size();
        if (applyOpsBytes > BSONObjMaxUserSize && ((*rBufferOffset) > 0)) {
            // There's no room to fit this deletion in the current batch, as doing so
            // would exceed 16MB of oplog entry: put this deletion back into the staging
            // buffer and commit the batch. Very large _id fields may exceed this threshold. In that
            // case, put them in their own batch.
            (*rBufferOffset)--;
            wuow.commit();
            return batchTimer.millis();
        }
        tassert(10118000,
                "batch size may only exceed BSON cap for single, large documents",
                applyOpsBytes <= BSONObjMaxUserSize || ((*rBufferOffset) == 0));

        collection_internal::deleteDocument(
            opCtx(),
            collectionPtr(),
            Snapshotted(memberDoc.snapshotId(), bsonObjDoc),
            _params->stmtId,
            member->recordId,
            _params->opDebug,
            writeToOrphan,
            false,
            _params->returnDeleted ? collection_internal::StoreDeletedDoc::On
                                   : collection_internal::StoreDeletedDoc::Off,
            CheckRecordId::Off,
            retryableWrite ? collection_internal::RetryableWrite::kYes
                           : collection_internal::RetryableWrite::kNo);

        (*docsDeleted)++;
        (*bytesDeleted) += bsonObjDoc.objsize();

        batchedDeleteStageSleepAfterNDocuments.executeIf(
            [&](const BSONObj& data) {
                int sleepMs = data["sleepMs"].safeNumberInt();
                opCtx()->sleepFor(Milliseconds(sleepMs));
            },
            [&](const BSONObj& data) {
                // hangAfterApproxNDocs is roughly estimated as the number of deletes
                // committed + the number of documents deleted in the current unit of work.

                // Assume nDocs is positive.
                const auto fpNss = NamespaceStringUtil::parseFailPointData(data, "ns"_sd);
                return data.hasField("sleepMs") && !fpNss.isEmpty() &&
                    collectionPtr()->ns() == fpNss && data.hasField("nDocs") &&
                    _specificStats.docsDeleted + *docsDeleted >=
                    static_cast<unsigned int>(data.getIntField("nDocs"));
            });

        const Milliseconds elapsedMillis(batchTimer.millis());
        if (_batchedDeleteParams->targetBatchTimeMS != Milliseconds(0) &&
            elapsedMillis >= _batchedDeleteParams->targetBatchTimeMS) {
            // Met 'targetBatchTimeMS' after evaluating the staged delete at '*bufferOffset'.
            break;
        }
    }
    wuow.commit();
    return batchTimer.millis();
}

PlanStage::StageState BatchedDeleteStage::_doStaging(WorkingSetID* idToReturn) {
    auto status = child()->work(idToReturn);

    switch (status) {
        case PlanStage::ADVANCED: {
            _stageNewDelete(idToReturn);
            return PlanStage::NEED_TIME;
        }
        default:
            return status;
    }
}

void BatchedDeleteStage::_stageNewDelete(WorkingSetID* workingSetMemberID) {

    WorkingSetMember* member = _ws->get(*workingSetMemberID);

    ScopeGuard memberFreer([&] { _ws->free(*workingSetMemberID); });
    invariant(member->hasRecordId());

    // Deletes can't have projections. This means that covering analysis will always add
    // a fetch. We should always get fetched data, and never just key data.
    invariant(member->hasObj());

    if (_params->isExplain) {
        // Populate 'nWouldDelete' for 'executionStats'.
        _specificStats.docsDeleted += 1;
        return;
    }

    // Preserve the member until the delete is committed. Once a delete is staged in the
    // buffer, its resources are freed when it is removed from the buffer.
    memberFreer.dismiss();

    // Ensure that the BSONObj underlying the WSM associated with 'id' is owned because
    // saveState() is allowed to free the memory the BSONObj points to. The BSONObj will be
    // needed later when it is passed to Collection::deleteDocument(). Note that the call to
    // makeObjOwnedIfNeeded() will leave the WSM in the RID_AND_OBJ state in case we need to
    // retry deleting it.
    member->makeObjOwnedIfNeeded();

    _stagedDeletesBuffer.append(*workingSetMemberID);
    const auto memberMemFootprintBytes = member->getMemUsage();
    _stagedDeletesWatermarkBytes += memberMemFootprintBytes;
    _passTotalDocsStaged += 1;
    incrementSSSMetricNoOverflow(batchedDeletesSSS.stagedSizeBytes, memberMemFootprintBytes);
}

PlanStage::StageState BatchedDeleteStage::_tryRestoreState(WorkingSetID* out) {
    return handlePlanStageYield(
        expCtx(),
        "BatchedDeleteStage::_tryRestoreState",
        [&] {
            if (MONGO_unlikely(
                    batchedDeleteStageThrowTemporarilyUnavailableException.shouldFail())) {
                throwTemporarilyUnavailableException(
                    str::stream()
                    << "Hit failpoint '"
                    << batchedDeleteStageThrowTemporarilyUnavailableException.getName() << "'.");
            }
            child()->restoreState(&collectionPtr());
            return PlanStage::NEED_TIME;
        },
        [&] {
            // yieldHandler
            *out = WorkingSet::INVALID_ID;
        });
}

void BatchedDeleteStage::_prepareToRetryDrainAfterYield(
    WorkingSetID* out, const std::set<WorkingSetID>& recordsToSkip) {
    _stagedDeletesBuffer.erase(recordsToSkip);
    *out = WorkingSet::INVALID_ID;
}

bool BatchedDeleteStage::_batchTargetMet() {
    return (_batchedDeleteParams->targetBatchDocs &&
            _stagedDeletesBuffer.size() >=
                static_cast<size_t>(_batchedDeleteParams->targetBatchDocs)) ||
        (_batchedDeleteParams->targetStagedDocBytes &&
         _stagedDeletesWatermarkBytes >=
             static_cast<unsigned long long>(_batchedDeleteParams->targetStagedDocBytes));
}

bool BatchedDeleteStage::_passTargetMet() {
    return (_batchedDeleteParams->targetPassDocs &&
            _passTotalDocsStaged >= _batchedDeleteParams->targetPassDocs) ||
        (_batchedDeleteParams->targetPassTimeMS != Milliseconds(0) &&
         Milliseconds(_passTimer.millis()) >= _batchedDeleteParams->targetPassTimeMS);
}

}  // namespace mongo
