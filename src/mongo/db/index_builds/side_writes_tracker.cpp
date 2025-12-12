/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/index_builds/side_writes_tracker.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/client.h"
#include "mongo/db/collection_crud/container_write.h"
#include "mongo/db/curop.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index_builds/index_build_interceptor_gen.h"
#include "mongo/db/index_builds/side_writes_tracker.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/lock_manager/exception_util.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/storage/temporary_record_store.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/progress_meter.h"
#include "mongo/util/timer.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kIndex

namespace mongo {

MONGO_FAIL_POINT_DEFINE(hangDuringIndexBuildDrainYield);
MONGO_FAIL_POINT_DEFINE(hangDuringIndexBuildDrainYieldSecond);
MONGO_FAIL_POINT_DEFINE(hangIndexBuildDuringDrainWritesPhase);
MONGO_FAIL_POINT_DEFINE(hangIndexBuildDuringDrainWritesPhaseSecond);


Status SideWritesTracker::bufferSideWrite(OperationContext* opCtx,
                                          const CollectionPtr& coll,
                                          const IndexCatalogEntry* indexCatalogEntry,
                                          const std::vector<BSONObj>& toInsert) {
    _counter->fetchAndAdd(toInsert.size());
    // This insert may roll back, but not necessarily from inserting into this table. If other
    // write operations outside this table and in the same transaction are rolled back, this
    // counter also needs to be rolled back.
    shard_role_details::getRecoveryUnit(opCtx)->onRollback(
        [sharedCounter = _counter, size = toInsert.size()](OperationContext*) {
            sharedCounter->fetchAndSubtract(size);
        });

    // Reserve the record ids which will be used for either the table insert or the container write.
    std::vector<RecordId> rids;
    _table->rs()->reserveRecordIds(
        opCtx, *shard_role_details::getRecoveryUnit(opCtx), &rids, toInsert.size());

    // TODO(SERVER-110289): Use utility function instead of checking fcvSnapshot.
    auto fcvSnapshot = serverGlobalParams.featureCompatibility.acquireFCVSnapshot();
    bool primaryDrivenFeatureFlagEnabled = fcvSnapshot.isVersionInitialized() &&
        feature_flags::gFeatureFlagPrimaryDrivenIndexBuilds.isEnabled(
            VersionContext::getDecoration(opCtx), fcvSnapshot);

    LOGV2_DEBUG(20691,
                2,
                "Recording side write keys on index",
                "numRecords"_attr = toInsert.size(),
                "index"_attr = indexCatalogEntry->descriptor()->indexName());

    if (primaryDrivenFeatureFlagEnabled) {
        invariant(_table->rs()->keyFormat() == KeyFormat::Long);
        IntegerKeyedContainer& container =
            std::get<std::reference_wrapper<IntegerKeyedContainer>>(_table->rs()->getContainer())
                .get();

        for (size_t i = 0; i < toInsert.size(); ++i) {
            auto& doc = toInsert[i];
            const auto& rid = rids[i];
            auto status =
                container_write::insert(opCtx,
                                        *shard_role_details::getRecoveryUnit(opCtx),
                                        coll,
                                        container,
                                        rid.getLong(),
                                        std::span<const char>(doc.objdata(), doc.objsize()));
            if (!status.isOK())
                return status;
        }
        return Status::OK();
    }

    std::vector<Record> records;
    records.reserve(toInsert.size());
    for (size_t i = 0; i < toInsert.size(); ++i) {
        auto& doc = toInsert[i];
        const auto& rid = rids[i];
        records.emplace_back(Record{rid, RecordData(doc.objdata(), doc.objsize())});
    }

    // By passing a vector of null timestamps, these inserts are not timestamped individually,
    // but rather with the timestamp of the owning operation.
    std::vector<Timestamp> timestamps(records.size());

    return _table->rs()->insertRecords(
        opCtx, *shard_role_details::getRecoveryUnit(opCtx), &records, timestamps);
}

void SideWritesTracker::keepTemporaryTable() {
    _table->keep();
}

std::string SideWritesTracker::getTableIdent() const {
    return std::string{_table->rs()->getIdent()};
}

void SideWritesTracker::_checkDrainPhaseFailPoint(OperationContext* opCtx,
                                                  const IndexCatalogEntry* indexCatalogEntry,
                                                  FailPoint* fp,
                                                  long long iteration) const {
    fp->executeIf(
        [=, this, indexName = indexCatalogEntry->descriptor()->indexName()](const BSONObj& data) {
            LOGV2(4841800,
                  "Hanging index build during drain writes phase",
                  "iteration"_attr = iteration,
                  "index"_attr = indexName);

            fp->pauseWhileSet(opCtx);
        },
        [iteration, indexName = indexCatalogEntry->descriptor()->indexName()](const BSONObj& data) {
            auto indexNames = data.getObjectField("indexNames");
            return iteration == data["iteration"].numberLong() &&
                std::any_of(indexNames.begin(), indexNames.end(), [&indexName](const auto& elem) {
                       return indexName == elem.String();
                   });
        });
}


void SideWritesTracker::_yield(OperationContext* opCtx,
                               const IndexCatalogEntry* indexCatalogEntry,
                               const Yieldable* yieldable) {
    // Releasing locks means a new snapshot should be acquired when restored.
    shard_role_details::getRecoveryUnit(opCtx)->abandonSnapshot();
    yieldable->yield();

    auto locker = shard_role_details::getLocker(opCtx);
    Locker::LockSnapshot snapshot;
    locker->saveLockStateAndUnlock(&snapshot);


    // Track the number of yields in CurOp.
    CurOp::get(opCtx)->yielded();

    auto failPointHang = [opCtx, indexCatalogEntry](FailPoint* fp) {
        fp->executeIf(
            [fp](auto&&) {
                LOGV2(20690, "Hanging index build during drain yield");
                fp->pauseWhileSet();
            },
            [opCtx, indexCatalogEntry](auto&& config) {
                return NamespaceStringUtil::parseFailPointData(config, "namespace") ==
                    indexCatalogEntry->getNSSFromCatalog(opCtx);
            });
    };
    failPointHang(&hangDuringIndexBuildDrainYield);
    failPointHang(&hangDuringIndexBuildDrainYieldSecond);

    locker->restoreLockState(opCtx, snapshot);
    yieldable->restore();
}

Status SideWritesTracker::drainWritesIntoIndex(
    OperationContext* opCtx,
    const CollectionPtr& coll,
    const IndexCatalogEntry* indexCatalogEntry,
    const InsertDeleteOptions& options,
    const IndexAccessMethod::KeyHandlerFn& onDuplicateKeyFn,
    DrainYieldPolicy drainYieldPolicy) {
    invariant(!shard_role_details::getLocker(opCtx)->inAWriteUnitOfWork());

    // These are used for logging only.
    int64_t totalDeleted = 0;
    int64_t totalInserted = 0;
    Timer timer;

    const int64_t appliedAtStart = _numApplied;

    // Set up the progress meter. This will never be completely accurate, because more writes
    // can be read from the side writes table than are observed before draining.
    static const char* curopMessage = "Index Build: draining writes received during build";
    ProgressMeterHolder progress;
    {
        stdx::unique_lock<Client> lk(*opCtx->getClient());
        progress.set(lk, CurOp::get(opCtx)->setProgress(lk, curopMessage), opCtx);
    }

    {
        stdx::unique_lock<Client> lk(*opCtx->getClient());
        // Force the progress meter to log at the end of every batch. By default, the progress
        // meter only logs after a large number of calls to hit(), but since we use such large
        // batch sizes, progress would rarely be displayed.
        progress.get(lk)->reset(
            count() - appliedAtStart /* total */, 3 /* secondsBetween */, 1 /* checkInterval */);
    }

    // Apply operations in batches per WriteUnitOfWork. The batch size limit allows the drain to
    // yield at a frequent interval, releasing locks and storage engine resources.
    const int32_t kBatchMaxSize = maxIndexBuildDrainBatchSize.load();

    // The batch byte limit restricts the total size of the write transaction, which relieves
    // pressure on the storage engine cache. This size maximum is enforced by the IDL. It should
    // never exceed the size limit of a 32-bit signed integer for overflow reasons.
    const int32_t kBatchMaxMB = maxIndexBuildDrainMemoryUsageMegabytes.load();
    const int32_t kMB = 1024 * 1024;
    invariant(kBatchMaxMB <= std::numeric_limits<int32_t>::max() / kMB);
    const int32_t kBatchMaxBytes = kBatchMaxMB * kMB;

    // In a single WriteUnitOfWork, scan the side table up to the batch or memory limit, apply
    // the keys to the index, and delete the side table records. Returns true if the cursor has
    // reached the end of the table, false if there are more records, and an error Status
    // otherwise.
    auto applySingleBatch = [&]() -> StatusWith<bool> {
        // This write is performed without a durable/commit timestamp. This transaction trips
        // the ordered assertion for the side-table documents which are inserted with a
        // timestamp and, in here, being deleted without a timestamp. Because the data being
        // read is majority committed, there's no risk of needing to roll back the writes done
        // by this "drain".
        //
        // Note that index builds will only "resume" once. A second resume results in the index
        // build starting from scratch. A "resumed" index build does not use a majority read
        // concern. And thus will observe data that can be rolled back via replication.
        shard_role_details::getRecoveryUnit(opCtx)->allowOneUntimestampedWrite();

        // TODO(SERVER-110289): Use utility function instead of checking fcvSnapshot.
        auto fcvSnapshot = serverGlobalParams.featureCompatibility.acquireFCVSnapshot();
        bool primaryDrivenFeatureFlagEnabled = fcvSnapshot.isVersionInitialized() &&
            feature_flags::gFeatureFlagPrimaryDrivenIndexBuilds.isEnabled(
                VersionContext::getDecoration(opCtx), fcvSnapshot);
        WriteUnitOfWork wuow(opCtx,
                             primaryDrivenFeatureFlagEnabled
                                 ? WriteUnitOfWork::kGroupForPossiblyRetryableOperations
                                 : WriteUnitOfWork::kDontGroup);

        int32_t batchSize = 0;
        int64_t batchSizeBytes = 0;

        auto cursor = _table->rs()->getCursor(opCtx, *shard_role_details::getRecoveryUnit(opCtx));

        // We use an ordered container because the order of deletion for the records in the side
        // table matters.
        std::vector<RecordId> recordsAddedToIndex;

        auto record = cursor->next();
        while (record) {
            opCtx->checkForInterrupt();

            auto& currentRecordId = record->id;
            BSONObj unownedDoc = record->data.toBson();

            // Don't apply this record if the total batch size in bytes would be too large.
            const int objSize = unownedDoc.objsize();
            if (batchSize > 0 && batchSizeBytes + objSize > kBatchMaxBytes) {
                break;
            }

            const long long iteration = _numApplied + batchSize;
            _checkDrainPhaseFailPoint(
                opCtx, indexCatalogEntry, &hangIndexBuildDuringDrainWritesPhase, iteration);
            _checkDrainPhaseFailPoint(
                opCtx, indexCatalogEntry, &hangIndexBuildDuringDrainWritesPhaseSecond, iteration);

            batchSize += 1;
            batchSizeBytes += objSize;

            // Wrap the re-usable reference in a movable instance.
            auto onDuplicateKeyUniqueFn = [&](const CollectionPtr& coll,
                                              const key_string::View& duplicateKey) {
                return onDuplicateKeyFn(coll, duplicateKey);
            };

            if (auto status = indexCatalogEntry->accessMethod()->applyIndexBuildSideWrite(
                    opCtx,
                    coll,
                    indexCatalogEntry,
                    unownedDoc,
                    options,
                    std::move(onDuplicateKeyUniqueFn),
                    &totalInserted,
                    &totalDeleted);
                !status.isOK()) {
                return status;
            }

            // Save the record ids of the documents inserted into the index for deletion
            // later. We can't delete records while holding a positioned cursor.
            recordsAddedToIndex.emplace_back(std::move(currentRecordId));

            // Don't continue if the batch is full. Allow the transaction to commit.
            if (batchSize == kBatchMaxSize) {
                break;
            }

            record = cursor->next();
        }

        // Delete documents from the side table as soon as they have been inserted into the
        // index. This ensures that no key is ever inserted twice and no keys are skipped.
        for (const auto& recordId : recordsAddedToIndex) {
            if (primaryDrivenFeatureFlagEnabled) {
                IntegerKeyedContainer& container =
                    std::get<std::reference_wrapper<IntegerKeyedContainer>>(
                        _table->rs()->getContainer())
                        .get();
                auto status = container_write::remove(opCtx,
                                                      *shard_role_details::getRecoveryUnit(opCtx),
                                                      coll,
                                                      container,
                                                      recordId.getLong());
                if (!status.isOK()) {
                    return status;
                }
            } else {
                _table->rs()->deleteRecord(
                    opCtx, *shard_role_details::getRecoveryUnit(opCtx), recordId);
            }
        }

        if (batchSize == 0) {
            invariant(!record);
            return true;
        }

        wuow.commit();

        {
            stdx::unique_lock<Client> lk(*opCtx->getClient());
            progress.get(lk)->hit(batchSize);
        }
        _numApplied += batchSize;

        // Lock yielding will be directed by the yield policy provided.
        // We will typically yield locks during the draining phase if we are holding intent
        // locks.
        if (DrainYieldPolicy::kYield == drainYieldPolicy) {
            const std::string indexIdent = indexCatalogEntry->getIdent();
            _yield(opCtx, indexCatalogEntry, &coll);

            // After yielding, the latest instance of the collection is fetched and can be
            // different from the collection instance prior to yielding. For this reason we need
            // to refresh the index entry pointer.
            indexCatalogEntry = coll->getIndexCatalog()->findIndexByIdent(
                opCtx, indexIdent, IndexCatalog::InclusionPolicy::kUnfinished);
        }

        {
            stdx::unique_lock<Client> lk(*opCtx->getClient());
            // Account for more writes coming in during a batch.
            progress.get(lk)->setTotalWhileRunning(_counter->loadRelaxed() - appliedAtStart);
        }
        return false;
    };

    // Indicates that there are no more visible records in the side table.
    bool atEof = false;

    // Apply batches of side writes until the last record in the table is seen.
    while (!atEof) {
        auto swAtEof = writeConflictRetry(opCtx, "index build drain", coll->ns(), applySingleBatch);
        if (!swAtEof.isOK()) {
            return swAtEof.getStatus();
        }
        atEof = swAtEof.getValue();
    }

    {
        stdx::unique_lock<Client> lk(*opCtx->getClient());
        progress.get(lk)->finished();
    }

    int logLevel = (_numApplied - appliedAtStart > 0) ? 0 : 1;
    LOGV2_DEBUG(20689,
                logLevel,
                "Index build: drained side writes",
                "index"_attr = indexCatalogEntry->descriptor()->indexName(),
                "collectionUUID"_attr = coll->uuid(),
                logAttrs(coll->ns()),
                "numApplied"_attr = (_numApplied - appliedAtStart),
                "totalInserted"_attr = totalInserted,
                "totalDeleted"_attr = totalDeleted,
                "durationMillis"_attr = timer.millis());

    return Status::OK();
}


bool SideWritesTracker::checkAllWritesApplied(OperationContext* opCtx, bool fatal) const {
    invariant(_table);

    // The table is empty only when all writes are applied.
    auto cursor = _table->rs()->getCursor(opCtx, *shard_role_details::getRecoveryUnit(opCtx));
    auto record = cursor->next();
    if (fatal) {
        invariant(
            !record,
            str::stream() << "Expected all side writes to be drained but found record with id "
                          << record->id << " and data " << record->data.toBson());
    } else if (record) {
        return false;
    }

    return true;
}

}  // namespace mongo
