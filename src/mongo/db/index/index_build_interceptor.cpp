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


#include "mongo/platform/basic.h"

#include "mongo/db/index/index_build_interceptor.h"

#include <vector>

#include "mongo/bson/bsonobj.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/index_build_interceptor_gen.h"
#include "mongo/db/multi_key_path_tracker.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/progress_meter.h"
#include "mongo/util/uuid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kIndex


namespace mongo {

MONGO_FAIL_POINT_DEFINE(hangDuringIndexBuildDrainYield);
MONGO_FAIL_POINT_DEFINE(hangDuringIndexBuildDrainYieldSecond);
MONGO_FAIL_POINT_DEFINE(hangIndexBuildDuringDrainWritesPhase);
MONGO_FAIL_POINT_DEFINE(hangIndexBuildDuringDrainWritesPhaseSecond);

IndexBuildInterceptor::IndexBuildInterceptor(OperationContext* opCtx, IndexCatalogEntry* entry)
    : _indexCatalogEntry(entry),
      _sideWritesTable(opCtx->getServiceContext()->getStorageEngine()->makeTemporaryRecordStore(
          opCtx, KeyFormat::Long)),
      _skippedRecordTracker(opCtx, entry, boost::none) {

    if (entry->descriptor()->unique()) {
        _duplicateKeyTracker = std::make_unique<DuplicateKeyTracker>(opCtx, entry);
    }
}

IndexBuildInterceptor::IndexBuildInterceptor(OperationContext* opCtx,
                                             IndexCatalogEntry* entry,
                                             StringData sideWritesIdent,
                                             boost::optional<StringData> duplicateKeyTrackerIdent,
                                             boost::optional<StringData> skippedRecordTrackerIdent)
    : _indexCatalogEntry(entry),
      _sideWritesTable(
          opCtx->getServiceContext()->getStorageEngine()->makeTemporaryRecordStoreFromExistingIdent(
              opCtx, sideWritesIdent)),
      _skippedRecordTracker(opCtx, entry, skippedRecordTrackerIdent),
      _skipNumAppliedCheck(true) {

    auto dupKeyTrackerIdentExists = duplicateKeyTrackerIdent ? true : false;
    uassert(ErrorCodes::BadValue,
            str::stream() << "Resume info must contain the duplicate key tracker ident ["
                          << duplicateKeyTrackerIdent
                          << "] if and only if the index is unique: " << entry->descriptor(),
            entry->descriptor()->unique() == dupKeyTrackerIdentExists);
    if (duplicateKeyTrackerIdent) {
        _duplicateKeyTracker =
            std::make_unique<DuplicateKeyTracker>(opCtx, entry, duplicateKeyTrackerIdent.get());
    }
}

void IndexBuildInterceptor::keepTemporaryTables() {
    _sideWritesTable->keep();
    if (_duplicateKeyTracker) {
        _duplicateKeyTracker->keepTemporaryTable();
    }
    _skippedRecordTracker.keepTemporaryTable();
}

Status IndexBuildInterceptor::recordDuplicateKey(OperationContext* opCtx,
                                                 const KeyString::Value& key) const {
    invariant(_indexCatalogEntry->descriptor()->unique());
    return _duplicateKeyTracker->recordKey(opCtx, key);
}

Status IndexBuildInterceptor::checkDuplicateKeyConstraints(OperationContext* opCtx) const {
    if (!_duplicateKeyTracker) {
        return Status::OK();
    }
    return _duplicateKeyTracker->checkConstraints(opCtx);
}

Status IndexBuildInterceptor::drainWritesIntoIndex(OperationContext* opCtx,
                                                   const CollectionPtr& coll,
                                                   const InsertDeleteOptions& options,
                                                   TrackDuplicates trackDuplicates,
                                                   DrainYieldPolicy drainYieldPolicy) {
    invariant(!opCtx->lockState()->inAWriteUnitOfWork());

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
        // This write is performed without a durable/commit timestamp. This transaction trips the
        // ordered assertion for the side-table documents which are inserted with a timestamp and,
        // in here, being deleted without a timestamp. Because the data being read is majority
        // committed, there's no risk of needing to roll back the writes done by this "drain".
        //
        // Note that index builds will only "resume" once. A second resume results in the index
        // build starting from scratch. A "resumed" index build does not use a majority read
        // concern. And thus will observe data that can be rolled back via replication.
        opCtx->recoveryUnit()->allowUntimestampedWrite();
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

            auto& currentRecordId = record->id;
            BSONObj unownedDoc = record->data.toBson();

            // Don't apply this record if the total batch size in bytes would be too large.
            const int objSize = unownedDoc.objsize();
            if (batchSize > 0 && batchSizeBytes + objSize > kBatchMaxBytes) {
                break;
            }

            const long long iteration = _numApplied + batchSize;
            _checkDrainPhaseFailPoint(opCtx, &hangIndexBuildDuringDrainWritesPhase, iteration);
            _checkDrainPhaseFailPoint(
                opCtx, &hangIndexBuildDuringDrainWritesPhaseSecond, iteration);

            batchSize += 1;
            batchSizeBytes += objSize;

            if (auto status = _applyWrite(opCtx,
                                          coll,
                                          unownedDoc,
                                          options,
                                          trackDuplicates,
                                          &totalInserted,
                                          &totalDeleted);
                !status.isOK()) {
                return status;
            }

            // Save the record ids of the documents inserted into the index for deletion later.
            // We can't delete records while holding a positioned cursor.
            recordsAddedToIndex.emplace_back(std::move(currentRecordId));

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

        // Lock yielding will be directed by the yield policy provided.
        // We will typically yield locks during the draining phase if we are holding intent locks.
        if (DrainYieldPolicy::kYield == drainYieldPolicy) {
            _yield(opCtx, &coll);
        }

        // Account for more writes coming in during a batch.
        progress->setTotalWhileRunning(_sideWritesCounter->loadRelaxed() - appliedAtStart);
        return false;
    };

    // Indicates that there are no more visible records in the side table.
    bool atEof = false;

    // Apply batches of side writes until the last record in the table is seen.
    while (!atEof) {
        auto swAtEof =
            writeConflictRetry(opCtx, "index build drain", coll->ns().ns(), applySingleBatch);
        if (!swAtEof.isOK()) {
            return swAtEof.getStatus();
        }
        atEof = swAtEof.getValue();
    }

    progress->finished();

    int logLevel = (_numApplied - appliedAtStart > 0) ? 0 : 1;
    LOGV2_DEBUG(20689,
                logLevel,
                "Index build: drained side writes",
                "index"_attr = _indexCatalogEntry->descriptor()->indexName(),
                "collectionUUID"_attr = coll->uuid(),
                logAttrs(coll->ns()),
                "numApplied"_attr = (_numApplied - appliedAtStart),
                "totalInserted"_attr = totalInserted,
                "totalDeleted"_attr = totalDeleted,
                "durationMillis"_attr = timer.millis());

    return Status::OK();
}

Status IndexBuildInterceptor::_applyWrite(OperationContext* opCtx,
                                          const CollectionPtr& coll,
                                          const BSONObj& operation,
                                          const InsertDeleteOptions& options,
                                          TrackDuplicates trackDups,
                                          int64_t* const keysInserted,
                                          int64_t* const keysDeleted) {
    // Deserialize the encoded KeyString::Value.
    int keyLen;
    const char* binKey = operation["key"].binData(keyLen);
    BufReader reader(binKey, keyLen);
    auto accessMethod = _indexCatalogEntry->accessMethod()->asSortedData();
    const KeyString::Value keyString = KeyString::Value::deserialize(
        reader, accessMethod->getSortedDataInterface()->getKeyStringVersion());

    const Op opType = operation.getStringField("op") == "i"_sd ? Op::kInsert : Op::kDelete;

    const KeyStringSet keySet{keyString};
    if (opType == Op::kInsert) {
        int64_t numInserted;
        auto status = accessMethod->insertKeysAndUpdateMultikeyPaths(
            opCtx,
            coll,
            {keySet.begin(), keySet.end()},
            {},
            MultikeyPaths{},
            options,
            [=](const KeyString::Value& duplicateKey) {
                return trackDups == TrackDuplicates::kTrack
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
        if (kDebugBuild)
            invariant(operation.getStringField("op") == "d"_sd);

        int64_t numDeleted;
        Status s =
            accessMethod->removeKeys(opCtx, {keySet.begin(), keySet.end()}, options, &numDeleted);
        if (!s.isOK()) {
            return s;
        }

        *keysDeleted += numDeleted;
        opCtx->recoveryUnit()->onRollback(
            [keysDeleted, numDeleted] { *keysDeleted -= numDeleted; });
    }
    return Status::OK();
}

void IndexBuildInterceptor::_yield(OperationContext* opCtx, const Yieldable* yieldable) {
    // Releasing locks means a new snapshot should be acquired when restored.
    opCtx->recoveryUnit()->abandonSnapshot();
    yieldable->yield();

    auto locker = opCtx->lockState();
    Locker::LockSnapshot snapshot;
    invariant(locker->saveLockStateAndUnlock(&snapshot));


    // Track the number of yields in CurOp.
    CurOp::get(opCtx)->yielded();

    auto failPointHang = [opCtx, indexCatalogEntry = _indexCatalogEntry](FailPoint* fp) {
        fp->executeIf(
            [fp](auto&&) {
                LOGV2(20690, "Hanging index build during drain yield");
                fp->pauseWhileSet();
            },
            [opCtx, indexCatalogEntry](auto&& config) {
                return config.getStringField("namespace") ==
                    indexCatalogEntry->getNSSFromCatalog(opCtx).ns();
            });
    };
    failPointHang(&hangDuringIndexBuildDrainYield);
    failPointHang(&hangDuringIndexBuildDrainYieldSecond);

    locker->restoreLockState(opCtx, snapshot);
    yieldable->restore();
}

bool IndexBuildInterceptor::areAllWritesApplied(OperationContext* opCtx) const {
    return _checkAllWritesApplied(opCtx, false);
}

void IndexBuildInterceptor::invariantAllWritesApplied(OperationContext* opCtx) const {
    _checkAllWritesApplied(opCtx, true);
}

bool IndexBuildInterceptor::_checkAllWritesApplied(OperationContext* opCtx, bool fatal) const {
    invariant(_sideWritesTable);

    // The table is empty only when all writes are applied.
    auto cursor = _sideWritesTable->rs()->getCursor(opCtx);
    auto record = cursor->next();
    if (fatal) {
        invariant(
            !record,
            str::stream() << "Expected all side writes to be drained but found record with id "
                          << record->id << " and data " << record->data.toBson());
    } else if (record) {
        return false;
    }

    if (_skipNumAppliedCheck) {
        return true;
    }

    auto writesRecorded = _sideWritesCounter->load();
    if (writesRecorded != _numApplied) {
        dassert(writesRecorded == _numApplied,
                (str::stream() << "The number of side writes recorded does not match the number "
                                  "applied, despite the table appearing empty. Writes recorded: "
                               << writesRecorded << ", applied: " << _numApplied));
        LOGV2_WARNING(20692,
                      "The number of side writes recorded does not match the number applied, "
                      "despite the table appearing empty",
                      "writesRecorded"_attr = writesRecorded,
                      "applied"_attr = _numApplied);
    }

    return true;
}

boost::optional<MultikeyPaths> IndexBuildInterceptor::getMultikeyPaths() const {
    stdx::unique_lock<Latch> lk(_multikeyPathMutex);
    return _multikeyPaths;
}

Status IndexBuildInterceptor::sideWrite(OperationContext* opCtx,
                                        const KeyStringSet& keys,
                                        const KeyStringSet& multikeyMetadataKeys,
                                        const MultikeyPaths& multikeyPaths,
                                        Op op,
                                        int64_t* const numKeysOut) {
    invariant(opCtx->lockState()->inAWriteUnitOfWork());

    // Maintain parity with IndexAccessMethods handling of key counting. Only include
    // `multikeyMetadataKeys` when inserting.
    *numKeysOut = keys.size() + (op == Op::kInsert ? multikeyMetadataKeys.size() : 0);

    // Maintain parity with IndexAccessMethod's handling of whether keys could change the multikey
    // state on the index.
    bool isMultikey = _indexCatalogEntry->accessMethod()->asSortedData()->shouldMarkIndexAsMultikey(
        keys.size(), multikeyMetadataKeys, multikeyPaths);

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
        BSONBinData binData(builder.buf(), builder.len(), BinDataGeneral);
        toInsert.emplace_back(BSON("op" << (op == Op::kInsert ? "i" : "d") << "key" << binData));
    }

    if (op == Op::kInsert) {
        // Wildcard indexes write multikey path information, typically part of the catalog
        // document, to the index itself. Multikey information is never deleted, so we only need
        // to add this data on the insert path.
        for (const auto& keyString : multikeyMetadataKeys) {
            builder.reset();
            keyString.serialize(builder);
            BSONBinData binData(builder.buf(), builder.len(), BinDataGeneral);
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

    LOGV2_DEBUG(20691,
                2,
                "recording {records_size} side write keys on index "
                "'{indexCatalogEntry_descriptor_indexName}'",
                "records_size"_attr = records.size(),
                "indexCatalogEntry_descriptor_indexName"_attr =
                    _indexCatalogEntry->descriptor()->indexName());

    // By passing a vector of null timestamps, these inserts are not timestamped individually, but
    // rather with the timestamp of the owning operation.
    std::vector<Timestamp> timestamps(records.size());
    return _sideWritesTable->rs()->insertRecords(opCtx, &records, timestamps);
}

Status IndexBuildInterceptor::retrySkippedRecords(OperationContext* opCtx,
                                                  const CollectionPtr& collection) {
    return _skippedRecordTracker.retrySkippedRecords(opCtx, collection);
}

void IndexBuildInterceptor::_checkDrainPhaseFailPoint(OperationContext* opCtx,
                                                      FailPoint* fp,
                                                      long long iteration) const {
    fp->executeIf(
        [=](const BSONObj& data) {
            LOGV2(4841800,
                  "Hanging index build during drain writes phase",
                  "iteration"_attr = iteration,
                  "index"_attr = _indexCatalogEntry->descriptor()->indexName());

            fp->pauseWhileSet(opCtx);
        },
        [iteration,
         &indexName = _indexCatalogEntry->descriptor()->indexName()](const BSONObj& data) {
            auto indexNames = data.getObjectField("indexNames");
            return iteration == data["iteration"].numberLong() &&
                std::any_of(indexNames.begin(), indexNames.end(), [&indexName](const auto& elem) {
                       return indexName == elem.String();
                   });
        });
}

}  // namespace mongo
