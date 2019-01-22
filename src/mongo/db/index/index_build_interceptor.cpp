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

    // Callers may request to read at a specific timestamp so that no drained writes are timestamped
    // earlier than their original write timestamp. Also ensure that leaving this function resets
    // the ReadSource to its original value.
    auto resetReadSourceGuard =
        makeGuard([ opCtx, prevReadSource = opCtx->recoveryUnit()->getTimestampReadSource() ] {
            opCtx->recoveryUnit()->abandonSnapshot();
            opCtx->recoveryUnit()->setTimestampReadSource(prevReadSource);
        });

    if (readSource != RecoveryUnit::ReadSource::kUnset) {
        opCtx->recoveryUnit()->abandonSnapshot();
        opCtx->recoveryUnit()->setTimestampReadSource(readSource);
    } else {
        resetReadSourceGuard.dismiss();
    }

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
    // only logs after a large number of calls to hit(), but since we batch inserts by up to
    // 1000 records, progress would rarely be displayed.
    progress->reset(_sideWritesCounter.load() - appliedAtStart /* total */,
                    3 /* secondsBetween */,
                    1 /* checkInterval */);

    // Buffer operations into batches to insert per WriteUnitOfWork. Impose an upper limit on the
    // number of documents and the total size of the batch.
    const int32_t kBatchMaxSize = 1000;
    const int64_t kBatchMaxBytes = BSONObjMaxInternalSize;

    int64_t batchSizeBytes = 0;

    std::vector<SideWriteRecord> batch;
    batch.reserve(kBatchMaxSize);

    // Hold on to documents that would exceed the per-batch memory limit. Always insert this first
    // into the next batch.
    boost::optional<SideWriteRecord> stashed;

    auto cursor = _sideWritesTable->rs()->getCursor(opCtx);

    bool atEof = false;
    while (!atEof) {
        opCtx->checkForInterrupt();

        // Stashed records should be inserted into a batch first.
        if (stashed) {
            invariant(batch.empty());
            batch.push_back(std::move(stashed.get()));
            stashed.reset();
        }

        auto record = cursor->next();

        if (record) {
            RecordId currentRecordId = record->id;
            BSONObj docOut = record->data.toBson().getOwned();

            // If the total batch size in bytes would be too large, stash this document and let the
            // current batch insert.
            int objSize = docOut.objsize();
            if (batchSizeBytes + objSize > kBatchMaxBytes) {
                invariant(!stashed);

                // Stash this document to be inserted in the next batch.
                stashed.emplace(currentRecordId, std::move(docOut));
            } else {
                batchSizeBytes += objSize;
                batch.emplace_back(currentRecordId, std::move(docOut));

                // Continue if there is more room in the batch.
                if (batch.size() < kBatchMaxSize) {
                    continue;
                }
            }
        } else {
            atEof = true;
            if (batch.empty())
                break;
        }

        invariant(!batch.empty());

        cursor->save();

        // If we are here, either we have reached the end of the table or the batch is full, so
        // insert everything in one WriteUnitOfWork, and delete each inserted document from the side
        // writes table.
        auto status = writeConflictRetry(opCtx, "index build drain", _indexCatalogEntry->ns(), [&] {
            WriteUnitOfWork wuow(opCtx);
            for (auto& operation : batch) {
                auto status =
                    _applyWrite(opCtx, operation.second, options, &totalInserted, &totalDeleted);
                if (!status.isOK()) {
                    return status;
                }

                // Delete the document from the table as soon as it has been inserted into the
                // index. This ensures that no key is ever inserted twice and no keys are skipped.
                _sideWritesTable->rs()->deleteRecord(opCtx, operation.first);
            }

            // For rollback to work correctly, these writes need to be timestamped. The actual time
            // is not important, as long as it not older than the most recent visible side write.
            IndexTimestampHelper::setGhostCommitTimestampForWrite(
                opCtx, NamespaceString(_indexCatalogEntry->ns()));

            wuow.commit();
            return Status::OK();
        });
        if (!status.isOK()) {
            return status;
        }

        progress->hit(batch.size());

        // Lock yielding will only happen if we are holding intent locks.
        _tryYield(opCtx);
        cursor->restore();

        // Account for more writes coming in during a batch.
        progress->setTotalWhileRunning(_sideWritesCounter.loadRelaxed() - appliedAtStart);

        _numApplied += batch.size();
        batch.clear();
        batchSizeBytes = 0;
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
    if (opCtx->lockState()->isCollectionLockedForMode(_indexCatalogEntry->ns(), MODE_S)) {
        return;
    }
    DEV {
        const NamespaceString nss(_indexCatalogEntry->ns());
        invariant(!opCtx->lockState()->isCollectionLockedForMode(nss.ns(), MODE_X));
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
        if (ns == _indexCatalogEntry->ns()) {
            log() << "Hanging index build during drain yield";
            MONGO_FAIL_POINT_PAUSE_WHILE_SET(hangDuringIndexBuildDrainYield);
        }
    }

    locker->restoreLockState(opCtx, snapshot);
}

bool IndexBuildInterceptor::areAllWritesApplied(OperationContext* opCtx) const {
    invariant(_sideWritesTable);
    auto cursor = _sideWritesTable->rs()->getCursor(opCtx, false /* forward */);
    auto record = cursor->next();

    // The table is empty only when all writes are applied.
    if (!record)
        return true;

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
