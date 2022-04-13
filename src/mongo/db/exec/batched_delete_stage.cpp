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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kWrite

#include "mongo/platform/basic.h"

#include "mongo/db/exec/batched_delete_stage.h"

#include "mongo/db/catalog/collection.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop.h"
#include "mongo/db/exec/scoped_timer.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/exec/write_stage_common.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/s/pm2423_feature_flags_gen.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

MONGO_FAIL_POINT_DEFINE(throwWriteConflictExceptionInBatchedDeleteStage);
MONGO_FAIL_POINT_DEFINE(batchedDeleteStageSleepAfterNDocuments);

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
    BatchedDeletesSSS()
        : ServerStatusSection("batchedDeletes"),
          batches(0),
          docs(0),
          stagedSizeBytes(0),
          timeMillis(0) {}

    bool includeByDefault() const override {
        return true;
    }

    BSONObj generateSection(OperationContext* opCtx, const BSONElement& configElem) const override {
        BSONObjBuilder bob;
        bob.appendNumber("batches", batches.loadRelaxed());
        bob.appendNumber("docs", docs.loadRelaxed());
        bob.appendNumber("stagedSizeBytes", stagedSizeBytes.loadRelaxed());
        bob.append("timeMillis", timeMillis.loadRelaxed());

        return bob.obj();
    }

    AtomicWord<long long> batches;
    AtomicWord<long long> docs;
    AtomicWord<long long> stagedSizeBytes;
    AtomicWord<long long> timeMillis;
} batchedDeletesSSS;

BatchedDeleteStage::BatchedDeleteStage(ExpressionContext* expCtx,
                                       std::unique_ptr<DeleteStageParams> params,
                                       std::unique_ptr<BatchedDeleteStageBatchParams> batchParams,
                                       WorkingSet* ws,
                                       const CollectionPtr& collection,
                                       PlanStage* child)
    : DeleteStage::DeleteStage(
          kStageType.rawData(), expCtx, std::move(params), ws, collection, child),
      _batchParams(std::move(batchParams)),
      _stagedDeletesBuffer(ws),
      _stagedDeletesWatermarkBytes(0),
      _drainRemainingBuffer(false) {
    tassert(6303800,
            "batched deletions only support multi-document deletions (multi: true)",
            _params->isMulti);
    tassert(6303801,
            "batched deletions do not support the 'fromMigrate' parameter",
            !_params->fromMigrate);
    tassert(6303802,
            "batched deletions do not support the 'returnDelete' parameter",
            !_params->returnDeleted);
    tassert(
        6303803, "batched deletions do not support the 'sort' parameter", _params->sort.isEmpty());
    tassert(6303804,
            "batched deletions do not support the 'removeSaver' parameter",
            _params->sort.isEmpty());
    tassert(6303805,
            "batched deletions do not support the 'numStatsForDoc' parameter",
            !_params->numStatsForDoc);
    tassert(6303807,
            "batch size parameters must be greater than or equal to zero",
            _batchParams->targetStagedDocBytes >= 0 && _batchParams->targetBatchDocs >= 0 &&
                _batchParams->targetBatchTimeMS >= Milliseconds(0));
}

BatchedDeleteStage::~BatchedDeleteStage() {}

PlanStage::StageState BatchedDeleteStage::_deleteBatch(WorkingSetID* out) {
    tassert(6389900, "Expected documents for batched deletion", _stagedDeletesBuffer.size() != 0);
    try {
        child()->saveState();
    } catch (const WriteConflictException&) {
        std::terminate();
    }


    std::set<WorkingSetID> recordsThatNoLongerMatch;
    Timer batchTimer(opCtx()->getServiceContext()->getTickSource());

    unsigned int docsDeleted = 0;
    unsigned int bufferOffset = 0;

    // Estimate the size of the oplog entry that would result from committing the batch,
    // to ensure we emit an oplog entry that's within the 16MB BSON limit.
    size_t applyOpsBytes = kApplyOpsNonArrayEntryPaddingBytes;

    try {
        // Start a WUOW with 'groupOplogEntries' which groups a delete batch into a single timestamp
        // and oplog entry.
        WriteUnitOfWork wuow(opCtx(), true /* groupOplogEntries */);
        for (; bufferOffset < _stagedDeletesBuffer.size(); ++bufferOffset) {
            if (MONGO_unlikely(throwWriteConflictExceptionInBatchedDeleteStage.shouldFail())) {
                throw WriteConflictException();
            }

            auto workingSetMemberId = _stagedDeletesBuffer.at(bufferOffset);

            // The PlanExecutor YieldPolicy may change snapshots between calls to 'doWork()'.
            // Different documents may have different snapshots.
            bool docStillMatches = write_stage_common::ensureStillMatches(
                collection(), opCtx(), _ws, workingSetMemberId, _params->canonicalQuery);

            WorkingSetMember* member = _ws->get(workingSetMemberId);
            if (docStillMatches) {
                Snapshotted<Document> memberDoc = member->doc;
                BSONObj bsonObjDoc = memberDoc.value().toBson();
                applyOpsBytes += kApplyOpsArrayEntryPaddingBytes;
                tassert(6515700,
                        "Expected document to have an _id field present",
                        bsonObjDoc.hasField("_id"));
                applyOpsBytes += bsonObjDoc.getField("_id").size();
                if (applyOpsBytes > BSONObjMaxUserSize) {
                    // There's no room to fit this deletion in the current batch, as doing so would
                    // exceed 16MB of oplog entry: put this deletion back into the staging buffer
                    // and commit the batch.
                    invariant(bufferOffset > 0);
                    bufferOffset--;
                    break;
                }

                collection()->deleteDocument(opCtx(),
                                             Snapshotted(memberDoc.snapshotId(), bsonObjDoc),
                                             _params->stmtId,
                                             member->recordId,
                                             _params->opDebug,
                                             _params->fromMigrate,
                                             false,
                                             _params->returnDeleted
                                                 ? Collection::StoreDeletedDoc::On
                                                 : Collection::StoreDeletedDoc::Off);

                docsDeleted++;

                batchedDeleteStageSleepAfterNDocuments.executeIf(
                    [&](const BSONObj& data) {
                        int sleepMs = data["sleepMs"].safeNumberInt();
                        opCtx()->sleepFor(Milliseconds(sleepMs));
                    },
                    [&](const BSONObj& data) {
                        // hangAfterApproxNDocs is roughly estimated as the number of deletes
                        // committed
                        // + the number of documents deleted in the current unit of work.
                        return data.hasField("sleepMs") && data.hasField("ns") &&
                            data.getStringField("ns") == collection()->ns().toString() &&
                            data.hasField("nDocs") &&
                            static_cast<int>(_specificStats.docsDeleted + docsDeleted) >=
                            data.getIntField("nDocs");
                    });
            } else {
                recordsThatNoLongerMatch.insert(workingSetMemberId);
            }

            const Milliseconds elapsedMillis(batchTimer.millis());
            if (_batchParams->targetBatchTimeMS != Milliseconds(0) &&
                elapsedMillis >= _batchParams->targetBatchTimeMS) {
                // Met 'targetBatchTimeMS' after evaluating the staged delete at 'bufferOffset'.
                break;
            }
        }

        wuow.commit();
    } catch (const WriteConflictException&) {
        return _prepareToRetryDrainAfterWCE(out, recordsThatNoLongerMatch);
    }

    incrementSSSMetricNoOverflow(batchedDeletesSSS.docs, docsDeleted);
    incrementSSSMetricNoOverflow(batchedDeletesSSS.batches, 1);
    incrementSSSMetricNoOverflow(batchedDeletesSSS.timeMillis, batchTimer.millis());
    _specificStats.docsDeleted += docsDeleted;

    if (bufferOffset < _stagedDeletesBuffer.size()) {
        // targetBatchTimeMS was met. Remove staged deletes that have been evaluated
        // (executed or skipped because they no longer match the query) from the buffer. If any
        // staged deletes remain in the buffer, they will be retried in a subsequent batch.
        _stagedDeletesBuffer.eraseUpToOffsetInclusive(bufferOffset);
    } else {
        // The individual deletes staged in the buffer are preserved until the batch is committed so
        // they can be retried in case of a write conflict.
        // No write conflict occurred, all staged deletes were successfully evaluated/executed, it
        // is safe to clear the buffer.
        _stagedDeletesBuffer.clear();
    }

    _signalIfDrainComplete();
    return _tryRestoreState(out);
}

PlanStage::StageState BatchedDeleteStage::doWork(WorkingSetID* out) {
    if (!_drainRemainingBuffer) {
        WorkingSetID id;
        auto status = child()->work(&id);

        switch (status) {
            case PlanStage::ADVANCED:
                break;

            case PlanStage::NEED_TIME:
                return status;

            case PlanStage::NEED_YIELD:
                *out = id;
                return status;

            case PlanStage::IS_EOF:
                if (!_stagedDeletesBuffer.empty()) {
                    // Drain the outstanding deletions.
                    auto ret = _deleteBatch(out);
                    if (ret != NEED_TIME || (ret == NEED_TIME && _drainRemainingBuffer)) {
                        // Only return NEED_TIME if there is more to drain in the buffer. Otherwise,
                        // there is no more to fetch and NEED_TIME signals all staged deletes have
                        // been sucessfully executed.
                        return ret;
                    }
                }
                return status;

            default:
                MONGO_UNREACHABLE;
        }

        WorkingSetMember* member = _ws->get(id);

        ScopeGuard memberFreer([&] { _ws->free(id); });
        invariant(member->hasRecordId());

        // Deletes can't have projections. This means that covering analysis will always add
        // a fetch. We should always get fetched data, and never just key data.
        invariant(member->hasObj());

        if (!_params->isExplain) {
            // Preserve the member until the delete is committed. Once a delete is staged in the
            // buffer, its resources are freed when it is removed from the buffer.
            memberFreer.dismiss();

            // Ensure that the BSONObj underlying the WSM associated with 'id' is owned because
            // saveState() is allowed to free the memory the BSONObj points to. The BSONObj will be
            // needed later when it is passed to Collection::deleteDocument(). Note that the call to
            // makeObjOwnedIfNeeded() will leave the WSM in the RID_AND_OBJ state in case we need to
            // retry deleting it.
            member->makeObjOwnedIfNeeded();
            _stagedDeletesBuffer.append(id);
            const auto memberMemFootprintBytes = member->getMemUsage();
            _stagedDeletesWatermarkBytes += memberMemFootprintBytes;
            incrementSSSMetricNoOverflow(batchedDeletesSSS.stagedSizeBytes,
                                         memberMemFootprintBytes);
        }
    }

    if (!_params->isExplain && (_drainRemainingBuffer || _batchTargetMet())) {
        _stagedDeletesWatermarkBytes = 0;
        return _deleteBatch(out);
    }

    return PlanStage::NEED_TIME;
}

PlanStage::StageState BatchedDeleteStage::_tryRestoreState(WorkingSetID* out) {
    try {
        child()->restoreState(&collection());
    } catch (const WriteConflictException&) {
        *out = WorkingSet::INVALID_ID;
        return NEED_YIELD;
    }
    return NEED_TIME;
}

PlanStage::StageState BatchedDeleteStage::_prepareToRetryDrainAfterWCE(
    WorkingSetID* out, const std::set<WorkingSetID>& recordsThatNoLongerMatch) {
    _stagedDeletesBuffer.erase(recordsThatNoLongerMatch);
    _signalIfDrainComplete();
    *out = WorkingSet::INVALID_ID;
    return NEED_YIELD;
}

void BatchedDeleteStage::_signalIfDrainComplete() {
    _drainRemainingBuffer = !_stagedDeletesBuffer.empty();
}

bool BatchedDeleteStage::_batchTargetMet() {
    tassert(6303900,
            "not expecting to be still draining staged deletions while evaluating whether to "
            "commit staged deletions",
            !_drainRemainingBuffer);
    return (_batchParams->targetBatchDocs &&
            _stagedDeletesBuffer.size() >=
                static_cast<unsigned long long>(_batchParams->targetBatchDocs)) ||
        (_batchParams->targetStagedDocBytes &&
         _stagedDeletesWatermarkBytes >=
             static_cast<unsigned long long>(_batchParams->targetStagedDocBytes));
}
}  // namespace mongo
