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

bool IndexBuildInterceptor::typeCanFastpathMultikeyUpdates(IndexType indexType) {
    // Ensure no new indexes are added without considering whether they use the multikeyPaths
    // vector.
    invariant(indexType == INDEX_BTREE || indexType == INDEX_2D || indexType == INDEX_HAYSTACK ||
              indexType == INDEX_2DSPHERE || indexType == INDEX_TEXT || indexType == INDEX_HASHED ||
              indexType == INDEX_WILDCARD);
    // Only BTREE indexes are guaranteed to use the multikeyPaths vector. Other index types either
    // do not track path-level multikey information or have "special" handling of multikey
    // information.
    return (indexType == INDEX_BTREE);
}

IndexBuildInterceptor::IndexBuildInterceptor(OperationContext* opCtx, IndexCatalogEntry* entry)
    : _indexCatalogEntry(entry),
      _sideWritesTable(
          opCtx->getServiceContext()->getStorageEngine()->makeTemporaryRecordStore(opCtx)),
      _sideWritesCounter(std::make_shared<AtomicWord<long long>>()) {

    if (entry->descriptor()->unique()) {
        _duplicateKeyTracker = std::make_unique<DuplicateKeyTracker>(opCtx, entry);
    }
    // `mergeMultikeyPaths` is sensitive to the two inputs having the same multikey
    // "shape". Initialize `_multikeyPaths` with the right shape from the IndexCatalogEntry.
    auto indexType = entry->descriptor()->getIndexType();
    if (typeCanFastpathMultikeyUpdates(indexType)) {
        auto numFields = entry->descriptor()->getNumFields();
        _multikeyPaths = MultikeyPaths{};
        auto it = _multikeyPaths->begin();
        _multikeyPaths->insert(it, numFields, {});
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

    // Indicates that there are no more visible records in the side table.
    bool atEof = false;

    // In a single WriteUnitOfWork, scan the side table up to the batch or memory limit, apply the
    // keys to the index, and delete the side table records.
    auto applySingleBatch = [&] {
        WriteUnitOfWork wuow(opCtx);

        int32_t batchSize = 0;
        int64_t batchSizeBytes = 0;

        auto cursor = _sideWritesTable->rs()->getCursor(opCtx);

        // We use an ordered container because the order of deletion for the records in the side
        // table matters.
        std::vector<RecordId> recordsAddedToIndex;

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

            // Save the record ids of the documents inserted into the index for deletion later.
            // We can't delete records while holding a positioned cursor.
            recordsAddedToIndex.push_back(currentRecordId);

            // Don't continue if the batch is full. Allow the transaction to commit.
            if (batchSize == kBatchMaxSize) {
                break;
            }
        }

        // Delete documents from the side table as soon as they have been inserted into the index.
        // This ensures that no key is ever inserted twice and no keys are skipped.
        for (const auto& recordId : recordsAddedToIndex) {
            _sideWritesTable->rs()->deleteRecord(opCtx, recordId);
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
        progress->setTotalWhileRunning(_sideWritesCounter->loadRelaxed() - appliedAtStart);
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
    // Deserialize the encoded KeyString::Value.
    int keyLen;
    const char* binKey = operation["key"].binData(keyLen);
    BufReader reader(binKey, keyLen);
    const KeyString::Value keyString = KeyString::Value::deserialize(
        reader,
        _indexCatalogEntry->accessMethod()->getSortedDataInterface()->getKeyStringVersion());

    const Op opType =
        (strcmp(operation.getStringField("op"), "i") == 0) ? Op::kInsert : Op::kDelete;

    const KeyStringSet keySet{keyString};
    const RecordId opRecordId =
        KeyString::decodeRecordIdAtEnd(keyString.getBuffer(), keyString.getSize());

    auto accessMethod = _indexCatalogEntry->accessMethod();
    if (opType == Op::kInsert) {
        InsertResult result;
        auto status = accessMethod->insertKeys(opCtx,
                                               {keySet.begin(), keySet.end()},
                                               {},
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
        if (kDebugBuild)
            invariant(strcmp(operation.getStringField("op"), "d") == 0);

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
    if (kDebugBuild) {
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

    hangDuringIndexBuildDrainYield.executeIf(
        [&](auto&&) {
            log() << "Hanging index build during drain yield";
            hangDuringIndexBuildDrainYield.pauseWhileSet();
        },
        [&](auto&& config) {
            return config.getStringField("namespace") == _indexCatalogEntry->ns().ns();
        });

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

            dassert(writesRecorded == _numApplied, message);
            warning() << message;
        }
        return true;
    }

    return false;
}

boost::optional<MultikeyPaths> IndexBuildInterceptor::getMultikeyPaths() const {
    stdx::unique_lock<stdx::mutex> lk(_multikeyPathMutex);
    return _multikeyPaths;
}

Status IndexBuildInterceptor::sideWrite(OperationContext* opCtx,
                                        const std::vector<KeyString::Value>& keys,
                                        const KeyStringSet& multikeyMetadataKeys,
                                        const MultikeyPaths& multikeyPaths,
                                        RecordId loc,
                                        Op op,
                                        int64_t* const numKeysOut) {
    invariant(opCtx->lockState()->inAWriteUnitOfWork());
    // Maintain parity with IndexAccessMethods handling of key counting. Only include
    // `multikeyMetadataKeys` when inserting.
    *numKeysOut = keys.size() + (op == Op::kInsert ? multikeyMetadataKeys.size() : 0);

    auto indexType = _indexCatalogEntry->descriptor()->getIndexType();

    // No need to take the multikeyPaths mutex if this is a trivial multikey update.
    bool canBypassMultikeyMutex = typeCanFastpathMultikeyUpdates(indexType) &&
        MultikeyPathTracker::isMultikeyPathsTrivial(multikeyPaths);

    if (op == Op::kInsert && !canBypassMultikeyMutex) {
        // SERVER-39705: It's worth noting that a document may not generate any keys, but be
        // described as being multikey. This step must be done to maintain parity with `validate`s
        // expectations.
        stdx::unique_lock<stdx::mutex> lk(_multikeyPathMutex);
        if (_multikeyPaths) {
            MultikeyPathTracker::mergeMultikeyPaths(&_multikeyPaths.get(), multikeyPaths);
        } else {
            // All indexes that support pre-initialization of _multikeyPaths during
            // IndexBuildInterceptor construction time should have been initialized already.
            invariant(!typeCanFastpathMultikeyUpdates(indexType));

            // `mergeMultikeyPaths` is sensitive to the two inputs having the same multikey
            // "shape". Initialize `_multikeyPaths` with the right shape from the first result.
            _multikeyPaths = multikeyPaths;
        }
    }

    if (*numKeysOut == 0) {
        return Status::OK();
    }

    // Reuse the same builder to avoid an allocation per key.
    BufBuilder builder;
    std::vector<BSONObj> toInsert;
    for (const auto& keyString : keys) {
        // Documents inserted into this table must be consumed in insert-order.
        // Additionally, these writes should be timestamped with the same timestamps that the
        // other writes making up this operation are given. When index builds can cope with
        // replication rollbacks, side table writes associated with a CUD operation should
        // remain/rollback along with the corresponding oplog entry.

        // Serialize the KeyString::Value into a binary format for storage. Since the
        // KeyString::Value also contains TypeBits information, it is not sufficient to just read
        // from getBuffer().
        builder.reset();
        keyString.serialize(builder);
        BSONBinData binData(builder.buf(), builder.getSize(), BinDataGeneral);
        toInsert.emplace_back(BSON("op" << (op == Op::kInsert ? "i" : "d") << "key" << binData));
    }

    if (op == Op::kInsert) {
        // Wildcard indexes write multikey path information, typically part of the catalog
        // document, to the index itself. Multikey information is never deleted, so we only need
        // to add this data on the insert path.
        for (const auto& keyString : multikeyMetadataKeys) {
            builder.reset();
            keyString.serialize(builder);
            BSONBinData binData(builder.buf(), builder.getSize(), BinDataGeneral);
            toInsert.emplace_back(BSON("op"
                                       << "i"
                                       << "key" << binData));
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
