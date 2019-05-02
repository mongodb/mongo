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
          opCtx->getServiceContext()->getStorageEngine()->makeTemporaryRecordStore(opCtx)) {

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

Status IndexBuildInterceptor::recordDuplicateKeys(OperationContext* opCtx,
                                                  const std::vector<BSONObj>& keys) {
    invariant(_indexCatalogEntry->descriptor()->unique());
    return _duplicateKeyTracker->recordKeys(opCtx, keys);
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
    progress->reset(_sideWritesCounter.load() - appliedAtStart /* total */,
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

    // Indicates that there are no more visible records in the side table.
    bool atEof = false;

    // In a single WriteUnitOfWork, scan the side table up to the batch or memory limit, apply the
    // keys to the index, and delete the side table records.
    auto applySingleBatch = [&] {
        WriteUnitOfWork wuow(opCtx);

        int32_t batchSize = 0;
        int64_t batchSizeBytes = 0;

        auto cursor = _sideWritesTable->rs()->getCursor(opCtx);

        while (!atEof) {
            opCtx->checkForInterrupt();

            auto record = cursor->next();
            if (!record) {
                atEof = true;
                break;
            }

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

            // Delete the document from the table as soon as it has been inserted into the index.
            // This ensures that no key is ever inserted twice and no keys are skipped.
            _sideWritesTable->rs()->deleteRecord(opCtx, currentRecordId);

            // Don't continue if the batch is full. Allow the transaction to commit.
            if (batchSize == kBatchMaxSize) {
                break;
            }
        }
        if (batchSize == 0) {
            invariant(atEof);
            return Status::OK();
        }

        wuow.commit();

        progress->hit(batchSize);
        _numApplied += batchSize;

        // Lock yielding will only happen if we are holding intent locks.
        _tryYield(opCtx);

        // Account for more writes coming in during a batch.
        progress->setTotalWhileRunning(_sideWritesCounter.loadRelaxed() - appliedAtStart);
        return Status::OK();
    };

    // Apply batches of side writes until the last record in the table is seen.
    while (!atEof) {
        if (auto status = writeConflictRetry(
                opCtx, "index build drain", _indexCatalogEntry->ns().ns(), applySingleBatch);
            !status.isOK()) {
            return status;
        }
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
        InsertResult result;
        auto status = accessMethod->insertKeys(opCtx,
                                               keySet,
                                               SimpleBSONObjComparator::kInstance.makeBSONObjSet(),
                                               MultikeyPaths{},
                                               opRecordId,
                                               options,
                                               &result);
        if (!status.isOK()) {
            return status;
        }

        if (result.dupsInserted.size() &&
            options.getKeysMode == IndexAccessMethod::GetKeysMode::kEnforceConstraints) {
            status = recordDuplicateKeys(opCtx, result.dupsInserted);
            if (!status.isOK()) {
                return status;
            }
        }

        int64_t numInserted = result.numInserted;
        *keysInserted += numInserted;
        opCtx->recoveryUnit()->onRollback(
            [keysInserted, numInserted] { *keysInserted -= numInserted; });
    } else {
        invariant(opType == Op::kDelete);
        DEV invariant(strcmp(operation.getStringField("op"), "d") == 0);

        int64_t numDeleted;
        Status s = accessMethod->removeKeys(opCtx, keySet, opRecordId, options, &numDeleted);
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
        auto writesRecorded = _sideWritesCounter.load();
        invariant(writesRecorded == _numApplied,
                  str::stream() << "The number of side writes recorded does not match the number "
                                   "applied, despite the table appearing empty. Writes recorded: "
                                << writesRecorded
                                << ", applied: "
                                << _numApplied);
        return true;
    }

    return false;
}

boost::optional<MultikeyPaths> IndexBuildInterceptor::getMultikeyPaths() const {
    stdx::unique_lock<stdx::mutex> lk(_multikeyPathMutex);
    return _multikeyPaths;
}

Status IndexBuildInterceptor::sideWrite(OperationContext* opCtx,
                                        IndexAccessMethod* indexAccessMethod,
                                        const BSONObj* obj,
                                        const InsertDeleteOptions& options,
                                        RecordId loc,
                                        Op op,
                                        int64_t* const numKeysOut) {
    invariant(opCtx->lockState()->inAWriteUnitOfWork());

    *numKeysOut = 0;
    BSONObjSet keys = SimpleBSONObjComparator::kInstance.makeBSONObjSet();
    BSONObjSet multikeyMetadataKeys = SimpleBSONObjComparator::kInstance.makeBSONObjSet();
    MultikeyPaths multikeyPaths;

    // Override key constraints when generating keys for removal. This is the same behavior as
    // IndexAccessMethod::remove and only applies to keys that do not apply to a partial filter
    // expression.
    const auto getKeysMode = op == Op::kInsert
        ? options.getKeysMode
        : IndexAccessMethod::GetKeysMode::kRelaxConstraintsUnfiltered;
    indexAccessMethod->getKeys(*obj, getKeysMode, &keys, &multikeyMetadataKeys, &multikeyPaths);

    // Maintain parity with IndexAccessMethods handling of key counting. Only include
    // `multikeyMetadataKeys` when inserting.
    *numKeysOut = keys.size() + (op == Op::kInsert ? multikeyMetadataKeys.size() : 0);

    if (*numKeysOut == 0) {
        return Status::OK();
    }

    {
        stdx::unique_lock<stdx::mutex> lk(_multikeyPathMutex);
        if (_multikeyPaths) {
            MultikeyPathTracker::mergeMultikeyPaths(&_multikeyPaths.get(), multikeyPaths);
        } else {
            // `mergeMultikeyPaths` is sensitive to the two inputs having the same multikey
            // "shape". Initialize `_multikeyPaths` with the right shape from the first result.
            _multikeyPaths = multikeyPaths;
        }
    }

    std::vector<BSONObj> toInsert;
    for (const auto& key : keys) {
        // Documents inserted into this table must be consumed in insert-order.
        // Additionally, these writes should be timestamped with the same timestamps that the
        // other writes making up this operation are given. When index builds can cope with
        // replication rollbacks, side table writes associated with a CUD operation should
        // remain/rollback along with the corresponding oplog entry.
        toInsert.emplace_back(BSON(
            "op" << (op == Op::kInsert ? "i" : "d") << "key" << key << "recordId" << loc.repr()));
    }

    if (op == Op::kInsert) {
        // Wildcard indexes write multikey path information, typically part of the catalog
        // document, to the index itself. Multikey information is never deleted, so we only need
        // to add this data on the insert path.
        for (const auto& key : multikeyMetadataKeys) {
            toInsert.emplace_back(BSON("op"
                                       << "i"
                                       << "key"
                                       << key
                                       << "recordId"
                                       << static_cast<int64_t>(
                                              RecordId::ReservedId::kWildcardMultikeyMetadataId)));
        }
    }

    _sideWritesCounter.fetchAndAdd(toInsert.size());
    // This insert may roll back, but not necessarily from inserting into this table. If other write
    // operations outside this table and in the same transaction are rolled back, this counter also
    // needs to be rolled back.
    opCtx->recoveryUnit()->onRollback(
        [ this, size = toInsert.size() ] { _sideWritesCounter.fetchAndSubtract(size); });

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
