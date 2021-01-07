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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kIndex

#include "mongo/platform/basic.h"

#include "mongo/db/index/index_build_interceptor.h"

#include <vector>

#include "mongo/bson/bsonobj.h"
#include "mongo/db/catalog/index_timestamp_helper.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/index_build_interceptor_gen.h"
#include "mongo/db/multi_key_path_tracker.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/progress_meter.h"
#include "mongo/util/uuid.h"

namespace mongo {

MONGO_FAIL_POINT_DEFINE(hangDuringIndexBuildDrainYield);

IndexBuildInterceptor::IndexBuildInterceptor(OperationContext* opCtx, IndexCatalogEntry* entry)
    : _indexCatalogEntry(entry),
      _sideWritesTable(
          opCtx->getServiceContext()->getStorageEngine()->makeTemporaryRecordStore(opCtx)),
      _sideWritesCounter(std::make_shared<AtomicWord<long long>>()) {

    if (entry->descriptor()->unique()) {
        _duplicateKeyTracker = std::make_unique<DuplicateKeyTracker>(opCtx, entry);
    }
}

void IndexBuildInterceptor::deleteTemporaryTables(OperationContext* opCtx) {
    _sideWritesTable->deleteTemporaryTable(opCtx);
    if (_duplicateKeyTracker) {
        _duplicateKeyTracker->deleteTemporaryTable(opCtx);
    }
}

Status IndexBuildInterceptor::recordDuplicateKey(OperationContext* opCtx, const BSONObj& key) {
    invariant(_indexCatalogEntry->descriptor()->unique());
    return _duplicateKeyTracker->recordKey(opCtx, key);
}

Status IndexBuildInterceptor::checkDuplicateKeyConstraints(OperationContext* opCtx) const {
    if (!_duplicateKeyTracker) {
        return Status::OK();
    }
    return _duplicateKeyTracker->checkConstraints(opCtx);
}

bool IndexBuildInterceptor::areAllConstraintsChecked(OperationContext* opCtx) const {
    if (!_duplicateKeyTracker) {
        return true;
    }
    return _duplicateKeyTracker->areAllConstraintsChecked(opCtx);
}

const std::string& IndexBuildInterceptor::getSideWritesTableIdent() const {
    return _sideWritesTable->rs()->getIdent();
}

const std::string& IndexBuildInterceptor::getConstraintViolationsTableIdent() const {
    return _duplicateKeyTracker->getConstraintsTableIdent();
}


Status IndexBuildInterceptor::drainWritesIntoIndex(OperationContext* opCtx,
                                                   const InsertDeleteOptions& options,
                                                   RecoveryUnit::ReadSource readSource) {
    invariant(!opCtx->lockState()->inAWriteUnitOfWork());
    // Reading at a timestamp during hybrid index builds is not supported.
    invariant(readSource == RecoveryUnit::ReadSource::kUnset);

    // These are used for logging only.
    int64_t totalDeleted = 0;
    int64_t totalInserted = 0;
    Timer timer;

    const int64_t appliedAtStart = _numApplied;

    // Set up the progress meter. This will never be completely accurate, because more writes can be
    // read from the side writes table than are observed before draining.
    static const char* curopMessage = "Index Build: draining writes received during build";
    ProgressMeterHolder progress;
    {
        stdx::unique_lock<Client> lk(*opCtx->getClient());
        progress.set(CurOp::get(opCtx)->setProgress_inlock(curopMessage));
    }

    // Force the progress meter to log at the end of every batch. By default, the progress meter
    // only logs after a large number of calls to hit(), but since we use such large batch sizes,
    // progress would rarely be displayed.
    progress->reset(_sideWritesCounter->load() - appliedAtStart /* total */,
                    3 /* secondsBetween */,
                    1 /* checkInterval */);

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

    // In a single WriteUnitOfWork, scan the side table up to the batch or memory limit, apply the
    // keys to the index, and delete the side table records.
    // Returns true if the cursor has reached the end of the table, false if there are more records,
    // and an error Status otherwise.
    auto applySingleBatch = [&]() -> StatusWith<bool> {
        WriteUnitOfWork wuow(opCtx);

        int32_t batchSize = 0;
        int64_t batchSizeBytes = 0;

        auto cursor = _sideWritesTable->rs()->getCursor(opCtx);

        // We use an ordered container because the order of deletion for the records in the side
        // table matters.
        std::vector<RecordId> recordsAddedToIndex;

        auto record = cursor->next();
        while (record) {
            opCtx->checkForInterrupt();

            RecordId currentRecordId = record->id;
            BSONObj unownedDoc = record->data.toBson();

            // Don't apply this record if the total batch size in bytes would be too large.
            const int objSize = unownedDoc.objsize();
            if (batchSize > 0 && batchSizeBytes + objSize > kBatchMaxBytes) {
                break;
            }

            batchSize += 1;
            batchSizeBytes += objSize;

            if (auto status =
                    _applyWrite(opCtx, unownedDoc, options, &totalInserted, &totalDeleted);
                !status.isOK()) {
                return status;
            }

            // Save the record ids of the documents inserted into the index for deletion later.
            // We can't delete records while holding a positioned cursor.
            recordsAddedToIndex.push_back(currentRecordId);

            // Don't continue if the batch is full. Allow the transaction to commit.
            if (batchSize == kBatchMaxSize) {
                break;
            }

            record = cursor->next();
        }

        // Delete documents from the side table as soon as they have been inserted into the index.
        // This ensures that no key is ever inserted twice and no keys are skipped.
        for (const auto& recordId : recordsAddedToIndex) {
            _sideWritesTable->rs()->deleteRecord(opCtx, recordId);
        }

        if (batchSize == 0) {
            invariant(!record);
            return true;
        }

        wuow.commit();

        progress->hit(batchSize);
        _numApplied += batchSize;

        // Lock yielding will only happen if we are holding intent locks.
        _tryYield(opCtx);

        // Account for more writes coming in during a batch.
        progress->setTotalWhileRunning(_sideWritesCounter->loadRelaxed() - appliedAtStart);
        return false;
    };

    // Indicates that there are no more visible records in the side table.
    bool atEof = false;

    // Apply batches of side writes until the last record in the table is seen.
    while (!atEof) {
        auto swAtEof = writeConflictRetry(
            opCtx, "index build drain", _indexCatalogEntry->ns().ns(), applySingleBatch);
        if (!swAtEof.isOK()) {
            return swAtEof.getStatus();
        }
        atEof = swAtEof.getValue();
    }

    progress->finished();

    int logLevel = (_numApplied - appliedAtStart > 0) ? 0 : 1;
    LOG(logLevel) << "index build: drain applied " << (_numApplied - appliedAtStart)
                  << " side writes (inserted: " << totalInserted << ", deleted: " << totalDeleted
                  << ") for '" << _indexCatalogEntry->descriptor()->indexName() << "' in "
                  << timer.millis() << " ms";

    return Status::OK();
}

Status IndexBuildInterceptor::_applyWrite(OperationContext* opCtx,
                                          const BSONObj& operation,
                                          const InsertDeleteOptions& options,
                                          int64_t* const keysInserted,
                                          int64_t* const keysDeleted) {
    const BSONObj key = operation["key"].Obj();
    const RecordId opRecordId = RecordId(operation["recordId"].Long());
    const Op opType =
        (strcmp(operation.getStringField("op"), "i") == 0) ? Op::kInsert : Op::kDelete;
    const BSONObjSet keySet = SimpleBSONObjComparator::kInstance.makeBSONObjSet({key});

    auto accessMethod = _indexCatalogEntry->accessMethod();
    if (opType == Op::kInsert) {
        int64_t numInserted;
        auto status = accessMethod->insertKeysAndUpdateMultikeyPaths(
            opCtx,
            {keySet.begin(), keySet.end()},
            {},
            MultikeyPaths{},
            opRecordId,
            options,
            [=](const BSONObj& duplicateKey) {
                return options.getKeysMode == IndexAccessMethod::GetKeysMode::kEnforceConstraints
                    ? recordDuplicateKey(opCtx, duplicateKey)
                    : Status::OK();
            },
            &numInserted);
        if (!status.isOK()) {
            return status;
        }

        *keysInserted += numInserted;
        opCtx->recoveryUnit()->onRollback(
            [keysInserted, numInserted] { *keysInserted -= numInserted; });
    } else {
        invariant(opType == Op::kDelete);
        DEV invariant(strcmp(operation.getStringField("op"), "d") == 0);

        int64_t numDeleted;
        Status s = accessMethod->removeKeys(
            opCtx, {keySet.begin(), keySet.end()}, opRecordId, options, &numDeleted);
        if (!s.isOK()) {
            return s;
        }

        *keysDeleted += numDeleted;
        opCtx->recoveryUnit()->onRollback(
            [keysDeleted, numDeleted] { *keysDeleted -= numDeleted; });
    }
    return Status::OK();
}

void IndexBuildInterceptor::_tryYield(OperationContext* opCtx) {
    // Never yield while holding locks that prevent writes to the collection: only yield while
    // holding intent locks. This check considers all locks in the hierarchy that would cover this
    // mode.
    const NamespaceString nss(_indexCatalogEntry->ns());
    if (opCtx->lockState()->isCollectionLockedForMode(nss, MODE_S)) {
        return;
    }
    DEV {
        invariant(!opCtx->lockState()->isCollectionLockedForMode(nss, MODE_X));
        invariant(!opCtx->lockState()->isDbLockedForMode(nss.db(), MODE_X));
    }

    // Releasing locks means a new snapshot should be acquired when restored.
    opCtx->recoveryUnit()->abandonSnapshot();

    auto locker = opCtx->lockState();
    Locker::LockSnapshot snapshot;
    invariant(locker->saveLockStateAndUnlock(&snapshot));


    // Track the number of yields in CurOp.
    CurOp::get(opCtx)->yielded();

    MONGO_FAIL_POINT_BLOCK(hangDuringIndexBuildDrainYield, config) {
        StringData ns{config.getData().getStringField("namespace")};
        if (ns == _indexCatalogEntry->ns().ns()) {
            log() << "Hanging index build during drain yield";
            MONGO_FAIL_POINT_PAUSE_WHILE_SET(hangDuringIndexBuildDrainYield);
        }
    }

    locker->restoreLockState(opCtx, snapshot);
}

bool IndexBuildInterceptor::areAllWritesApplied(OperationContext* opCtx) const {
    invariant(_sideWritesTable);
    auto cursor = _sideWritesTable->rs()->getCursor(opCtx);
    auto record = cursor->next();
    // The table is empty only when all writes are applied.
    if (!record) {
        auto writesRecorded = _sideWritesCounter->load();
        if (writesRecorded != _numApplied) {
            const std::string message = str::stream()
                << "The number of side writes recorded does not match the number "
                   "applied, despite the table appearing empty. Writes recorded: "
                << writesRecorded << ", applied: " << _numApplied;
            log() << message;

            // If _numApplied is less than writesRecorded, this suggests that there are keys not
            // visible in our snapshot, so we return false. If _numApplied is greater than
            // writesRecorded, this suggests that we either double-counted or double-applied a key.
            // Double counting would suggest a bug in the code that tracks inserts, but these
            // counters are only used for progress reporting and invariants. Double-inserting could
            // be concerning, but indexes allow key overwrites, so we would never introduce an
            // inconsistency in this case; we would just overwrite a previous key. Thus, we return
            // true.
            return writesRecorded < _numApplied;
        }
        return true;
    }

    return false;
}

boost::optional<MultikeyPaths> IndexBuildInterceptor::getMultikeyPaths() const {
    stdx::unique_lock<Latch> lk(_multikeyPathMutex);
    return _multikeyPaths;
}

Status IndexBuildInterceptor::sideWrite(OperationContext* opCtx,
                                        const std::vector<BSONObj>& keys,
                                        const BSONObjSet& multikeyMetadataKeys,
                                        const MultikeyPaths& multikeyPaths,
                                        RecordId loc,
                                        Op op,
                                        int64_t* const numKeysOut) {
    invariant(opCtx->lockState()->inAWriteUnitOfWork());
    // Maintain parity with IndexAccessMethods handling of key counting. Only include
    // `multikeyMetadataKeys` when inserting.
    *numKeysOut = keys.size() + (op == Op::kInsert ? multikeyMetadataKeys.size() : 0);

    // Maintain parity with IndexAccessMethod's handling of whether keys could change the multikey
    // state on the index.
    bool isMultikey = _indexCatalogEntry->accessMethod()->shouldMarkIndexAsMultikey(
        keys, {multikeyMetadataKeys.begin(), multikeyMetadataKeys.end()}, multikeyPaths);

    // No need to take the multikeyPaths mutex if this would not change any multikey state.
    if (op == Op::kInsert && isMultikey) {
        // SERVER-39705: It's worth noting that a document may not generate any keys, but be
        // described as being multikey. This step must be done to maintain parity with `validate`s
        // expectations.
        stdx::unique_lock<Latch> lk(_multikeyPathMutex);
        if (_multikeyPaths) {
            MultikeyPathTracker::mergeMultikeyPaths(&_multikeyPaths.get(), multikeyPaths);
        } else {
            // `mergeMultikeyPaths` is sensitive to the two inputs having the same multikey
            // "shape". Initialize `_multikeyPaths` with the right shape from the first result.
            _multikeyPaths = multikeyPaths;
        }
    }

    if (*numKeysOut == 0) {
        return Status::OK();
    }

    std::vector<BSONObj> toInsert;
    for (const auto& key : keys) {
        // Documents inserted into this table must be consumed in insert-order.
        // Additionally, these writes should be timestamped with the same timestamps that the
        // other writes making up this operation are given. When index builds can cope with
        // replication rollbacks, side table writes associated with a CUD operation should
        // remain/rollback along with the corresponding oplog entry.
        toInsert.emplace_back(BSON("op" << (op == Op::kInsert ? "i" : "d") << "key" << key
                                        << "recordId" << loc.repr()));
    }

    if (op == Op::kInsert) {
        // Wildcard indexes write multikey path information, typically part of the catalog
        // document, to the index itself. Multikey information is never deleted, so we only need
        // to add this data on the insert path.
        for (const auto& key : multikeyMetadataKeys) {
            toInsert.emplace_back(BSON("op"
                                       << "i"
                                       << "key" << key << "recordId"
                                       << static_cast<int64_t>(
                                              RecordId::ReservedId::kWildcardMultikeyMetadataId)));
        }
    }

    _sideWritesCounter->fetchAndAdd(toInsert.size());
    // This insert may roll back, but not necessarily from inserting into this table. If other write
    // operations outside this table and in the same transaction are rolled back, this counter also
    // needs to be rolled back.
    opCtx->recoveryUnit()->onRollback([sharedCounter = _sideWritesCounter, size = toInsert.size()] {
        sharedCounter->fetchAndSubtract(size);
    });

    std::vector<Record> records;
    for (auto& doc : toInsert) {
        records.emplace_back(Record{RecordId(),  // The storage engine will assign its own RecordId
                                                 // when we pass one that is null.
                                    RecordData(doc.objdata(), doc.objsize())});
    }

    LOG(2) << "recording " << records.size() << " side write keys on index '"
           << _indexCatalogEntry->descriptor()->indexName() << "'";

    // By passing a vector of null timestamps, these inserts are not timestamped individually, but
    // rather with the timestamp of the owning operation.
    std::vector<Timestamp> timestamps(records.size());
    return _sideWritesTable->rs()->insertRecords(opCtx, &records, timestamps);
}

}  // namespace mongo
