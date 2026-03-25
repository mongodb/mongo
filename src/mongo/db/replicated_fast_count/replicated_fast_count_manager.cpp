/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/replicated_fast_count/replicated_fast_count_manager.h"

#include "mongo/db/collection_crud/collection_write_path.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_delta_utils.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_metrics.h"
#include "mongo/db/shard_role/shard_catalog/clustered_collection_util.h"
#include "mongo/db/update/document_diff_calculator.h"
#include "mongo/db/update/update_oplog_entry_serialization.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

MONGO_FAIL_POINT_DEFINE(hangAfterReplicatedFastCountSnapshot);

namespace mongo {

namespace {

MONGO_FAIL_POINT_DEFINE(sleepAfterFlush);
MONGO_FAIL_POINT_DEFINE(failDuringFlush);

}  // namespace

static const ServiceContext::Decoration<ReplicatedFastCountManager> getReplicatedFastCountManager =
    ServiceContext::declareDecoration<ReplicatedFastCountManager>();

ReplicatedFastCountManager& ReplicatedFastCountManager::get(ServiceContext* svcCtx) {
    return getReplicatedFastCountManager(svcCtx);
}

void ReplicatedFastCountManager::initializeFastCountCommitFn() {
    setFastCountCommitFn([](OperationContext* opCtx,
                            const boost::container::flat_map<UUID, CollectionSizeCount>& changes,
                            boost::optional<Timestamp> commitTime) {
        getReplicatedFastCountManager(opCtx->getServiceContext()).commit(changes, commitTime);
    });
}

void ReplicatedFastCountManager::startup(OperationContext* opCtx) {
    massert(11905700,
            "ReplicatedFastCountManager background thread already running. It should only be "
            "started up once.",
            !_backgroundThread.joinable());

    massert(11718600,
            "Expected fastcount collection to exist on startup",
            replicated_fast_count::acquireSizeCountCollectionForRead(opCtx).has_value());

    LOGV2(12051100, "Starting up ReplicatedFastCountManager thread");

    ObservableMutexRegistry::get().add("ReplicatedFastCountManager::_metadataMutex",
                                       _metadataMutex);

    if (!_isUnderTest) {
        _isEnabled.store(true);
        _backgroundThread = stdx::thread(
            &ReplicatedFastCountManager::_startBackgroundThread, this, opCtx->getServiceContext());
    }

    _metrics.setIsRunning(true);
}

void ReplicatedFastCountManager::shutdown(OperationContext* opCtx) {
    LOGV2(11648800, "Shutting down ReplicatedFastCountManager");
    if (!_backgroundThread.joinable()) {
        LOGV2(12150400,
              "ReplicatedFastCountManager background thread is not running; skipping shutdown");
        return;
    }

    // Shutdown background thread.
    {
        stdx::lock_guard lock(_metadataMutex);
        _isEnabled.store(false);
    }
    _backgroundThreadReadyForFlush.notify_one();
    _backgroundThread.join();

    flushSync(opCtx);

    LOGV2(11751501, "ReplicatedFastCountManager stopped");

    _metrics.setIsRunning(false);
}

int ReplicatedFastCountManager::_hydrateMetadataFromDisk(
    OperationContext* opCtx, const CollectionOrViewAcquisition& acquisition) {
    int numRecordsScanned = 0;
    stdx::lock_guard lock(_metadataMutex);
    auto cursor = acquisition.getCollectionPtr()->getCursor(opCtx);
    while (auto record = cursor->next()) {
        Record& rec = *record;
        UUID uuid = _UUIDForKey(rec.id);
        BSONObj data = rec.data.releaseToBson();

        ++numRecordsScanned;

        auto& meta = _metadata[uuid];
        meta.sizeCount.count = data.getField(replicated_fast_count::kMetadataKey)
                                   .Obj()
                                   .getField(replicated_fast_count::kCountKey)
                                   .Long();
        meta.sizeCount.size = data.getField(replicated_fast_count::kMetadataKey)
                                  .Obj()
                                  .getField(replicated_fast_count::kSizeKey)
                                  .Long();
        meta.validAsOf = data.getField(replicated_fast_count::kValidAsOfKey).timestamp();
    }
    return numRecordsScanned;
}

void ReplicatedFastCountManager::initializeMetadata(OperationContext* opCtx) {

    auto acquisition = replicated_fast_count::acquireSizeCountCollectionForRead(opCtx);

    if (!acquisition.has_value()) {
        // This should only be the case on cold boot.
        LOGV2(11999600, "Internal fastcount collection not present during initialization.");
        return;
    }

    const auto startTime = Date_t::now();

    const int numRecordsScanned = _hydrateMetadataFromDisk(opCtx, *acquisition);

    LOGV2(11648801,
          "ReplicatedFastCountManager initialization complete",
          "numRecordsScanned"_attr = numRecordsScanned,
          "duration"_attr = Date_t::now() - startTime);
}

void ReplicatedFastCountManager::commit(
    const boost::container::flat_map<UUID, CollectionSizeCount>& changes,
    boost::optional<Timestamp> commitTime) {
    stdx::lock_guard lock(_metadataMutex);
    for (const auto& [uuid, metadata] : changes) {
        // Ignore changes that don't need to be flushed. Count and size can both be zero if two or
        // more UncommittedFastCountChange::record() calls between checkpoints for the same UUID
        // cancel each other out.
        if (metadata.count == 0 && metadata.size == 0) {
            continue;
        }

        auto& stored = _metadata[uuid];
        stored.sizeCount.count += metadata.count;
        stored.sizeCount.size += metadata.size;
        stored.dirty = true;
        if (commitTime) {
            stored.validAsOf = commitTime.get();
        }
        // TODO SERVER-120203: Re-enable this invariant once outstanding bugs are fixed.
        // invariant(stored.sizeCount.size >= 0 && stored.sizeCount.count >= 0,
        //           fmt::format("Expected fast count size and count to be non-negative, but saw
        //           size "
        //                       "{} and count {}",
        //                       stored.sizeCount.size,
        //                       stored.sizeCount.count));
    }
}

CollectionSizeCount ReplicatedFastCountManager::find(const UUID& uuid) const {
    stdx::lock_guard lock(_metadataMutex);
    auto it = _metadata.find(uuid);
    if (it != _metadata.end()) {
        return it->second.sizeCount;
    }
    return {};
}

void ReplicatedFastCountManager::flushAsync() {
    stdx::lock_guard lock(_metadataMutex);
    _flushRequested = true;
    _backgroundThreadReadyForFlush.notify_one();
}

void ReplicatedFastCountManager::flushSync(OperationContext* opCtx) {
    FastSizeCountMap dirtyMetadata;
    Timestamp allDurableTs;
    {
        stdx::unique_lock lock(_metadataMutex);
        dirtyMetadata = _getAndClearSnapshotOfDirtyMetadata(lock);
        allDurableTs = opCtx->getServiceContext()->getStorageEngine()->getAllDurableTimestamp();
    }

    if (MONGO_unlikely(hangAfterReplicatedFastCountSnapshot.shouldFail())) {
        hangAfterReplicatedFastCountSnapshot.pauseWhileSet();
    }

    _acquireAndFlush(opCtx, dirtyMetadata, allDurableTs);
}

void ReplicatedFastCountManager::disablePeriodicWrites_ForTest() {
    invariant(!_backgroundThread.joinable(),
              "Background thread started running before disabling periodic metadata writes");
    _isUnderTest = true;
}

bool ReplicatedFastCountManager::isRunning_ForTest() {
    return _backgroundThread.joinable();
}

void ReplicatedFastCountManager::_acquireAndFlush(OperationContext* opCtx,
                                                  const FastSizeCountMap& dirtyMetadata,
                                                  Timestamp validAsOfTs) {
    // TODO SERVER-121393: Revamp acquisition helpers so that we can easily acquire both internal
    // collections and not have to perform as many checks at the callsite.
    auto sizeCountStoreAcquisition =
        replicated_fast_count::acquireSizeCountCollectionForWrite(opCtx);
    massert(ErrorCodes::NamespaceNotFound,
            "Expected fastcount collection to exist",
            sizeCountStoreAcquisition);


    const CollectionPtr& fastCountColl = sizeCountStoreAcquisition->getCollectionPtr();
    invariant(fastCountColl,
              str::stream()
                  << "Expected to acquire fastcount store as a collection, not a view. isView : "
                  << sizeCountStoreAcquisition->isView());

    auto timestampsStoreAcquisition =
        replicated_fast_count::acquireTimestampCollectionForWrite(opCtx);
    massert(ErrorCodes::NamespaceNotFound,
            "Expected fastcount timestamps collection to exist",
            timestampsStoreAcquisition);

    const CollectionPtr& timestampsColl = timestampsStoreAcquisition->getCollectionPtr();
    invariant(timestampsColl,
              str::stream() << "Expected to acquire fastcount timestamps store as a collection, "
                               "not a view. isView : "
                            << timestampsStoreAcquisition->isView());

    try {
        _doFlush(opCtx, fastCountColl, timestampsColl, dirtyMetadata, validAsOfTs);
    } catch (const DBException& ex) {
        _metrics.incrementFlushFailureCount();
        LOGV2_WARNING(7397500,
                      "Failed to persist collection sizeCount metadata",
                      "error"_attr = ex.toStatus());
    }
}

ReplicatedFastCountManager::FastSizeCountMap
ReplicatedFastCountManager::_getAndClearSnapshotOfDirtyMetadata(WithLock metadataLock) {
    FastSizeCountMap dirtyMetadata;
    dirtyMetadata.reserve(_metadata.size());
    for (auto&& [metadataKey, metadataValue] : _metadata) {
        if (metadataValue.dirty) {
            dirtyMetadata[metadataKey] = metadataValue;
            metadataValue.dirty = false;
        }
    }

    return dirtyMetadata;
}

void ReplicatedFastCountManager::_doFlush(OperationContext* opCtx,
                                          const CollectionPtr& metadataStoreColl,
                                          const CollectionPtr& metadataTimestampsColl,
                                          const FastSizeCountMap& dirtyMetadata,
                                          const Timestamp& validAsOfTs) {

    if (MONGO_unlikely(failDuringFlush.shouldFail())) {
        uasserted(11550800, "Injected failure in _doFlush() for testing");
    }

    const Date_t startTime = Date_t::now();

    WriteUnitOfWork wuow(opCtx, WriteUnitOfWork::kGroupForPossiblyRetryableOperations);
    for (auto&& [metadataKey, metadataVal] : dirtyMetadata) {
        _writeSizeCountEntry(opCtx,
                             metadataStoreColl,
                             metadataKey,
                             metadataVal.sizeCount,
                             metadataVal.validAsOf,
                             _keyForUUID(metadataKey));
    }
    int32_t stripe = _getStripe();
    _writeTimestampEntry(opCtx, metadataTimestampsColl, stripe, validAsOfTs);
    wuow.commit();
    _metrics.addWriteTimeMsTotal((Date_t::now() - startTime).count());
}

void ReplicatedFastCountManager::_startBackgroundThread(ServiceContext* svcCtx) {
    ThreadClient tc(_threadName, svcCtx->getService());
    AuthorizationSession::get(cc())->grantInternalAuthorization();
    try {
        _flushPeriodicallyOnSignal();
    } catch (const DBException& ex) {
        LOGV2_WARNING(11648806,
                      "Failure in thread",
                      "threadName"_attr = _threadName,
                      "error"_attr = ex.toStatus());
    }

    LOGV2(11648804, "ReplicatedFastCountManager exited");
}

void ReplicatedFastCountManager::_flushPeriodicallyOnSignal() {
    while (_isEnabled.load()) {
        FastSizeCountMap dirtyMetadata;
        Timestamp allDurableTs;
        {
            stdx::unique_lock lock(_metadataMutex);
            _backgroundThreadReadyForFlush.wait(
                lock, [this] { return _flushRequested || !_isEnabled.load(); });
            _flushRequested = false;
            // If the condition variable was signalled during shutdown, we can exit early.
            if (!_isEnabled.load()) {
                break;
            }
            // Get snapshot of metadata while still holding mutex from condition variable signal.
            dirtyMetadata = _getAndClearSnapshotOfDirtyMetadata(lock);
            allDurableTs = cc().getServiceContext()->getStorageEngine()->getAllDurableTimestamp();
        }

        const Date_t flushStartTime = Date_t::now();
        try {
            auto opCtx = cc().makeOperationContext();
            _acquireAndFlush(opCtx.get(), dirtyMetadata, allDurableTs);
        } catch (const DBException& ex) {
            if (ex.code() == ErrorCodes::InterruptedDueToReplStateChange) {
                // Stepdown attempt interrupted us. We can continue here - if the stepdown did not
                // succeed we will run the next iteration, otherwise we will break out of the loop.
                LOGV2_DEBUG(11905701,
                            2,
                            "ReplicatedFastCountManager iteration interrupted due to "
                            "replication state change; will retry",
                            "error"_attr = ex.toStatus());
                continue;
            }
        }

        // Failpoint used in testing for:
        // 1. indicating a flush has completed
        // 2. (optionally) elongating the duration of a flush.
        sleepAfterFlush.execute([](const BSONObj& data) {
            if (auto elem = data["sleepMs"]; elem) {
                sleepmillis(elem.numberInt());
            }
        });

        _metrics.recordFlush(flushStartTime, dirtyMetadata.size());
    }
}

void ReplicatedFastCountManager::_writeTimestampEntry(OperationContext* opCtx,
                                                      const CollectionPtr& timestampColl,
                                                      const int32_t stripe,
                                                      const Timestamp& validAsOfTs) {
    Snapshotted<BSONObj> doc;
    const RecordId recordId = _keyForStripe(stripe);
    const bool exists = timestampColl->findDoc(opCtx, recordId, &doc);
    const BSONObj newDoc = _getTimestampDocForWrite(stripe, validAsOfTs);
    const BSONObj criteria = BSON("_id" << stripe);

    if (exists) {
        _metrics.incrementUpdateCount();
        _updateMetadata(opCtx, timestampColl, doc, newDoc, criteria, recordId);
    } else {
        _metrics.incrementInsertCount();
        _insertMetadata(opCtx, timestampColl, newDoc);
    }
}

void ReplicatedFastCountManager::_writeSizeCountEntry(OperationContext* opCtx,
                                                      const CollectionPtr& fastCountColl,
                                                      const UUID& uuid,
                                                      const CollectionSizeCount& sizeCount,
                                                      const Timestamp& validAsOfTS,
                                                      const RecordId& recordId) {
    Snapshotted<BSONObj> doc;
    const bool exists = fastCountColl->findDoc(opCtx, recordId, &doc);
    const BSONObj newDoc = _getDocForWrite(uuid, sizeCount, validAsOfTS);
    const BSONObj criteria = BSON("_id" << uuid);

    if (exists) {
        _metrics.incrementUpdateCount();
        _updateMetadata(opCtx, fastCountColl, doc, newDoc, criteria, recordId);
    } else {
        _metrics.incrementInsertCount();
        _insertMetadata(opCtx, fastCountColl, newDoc);
    }
}

void ReplicatedFastCountManager::_updateMetadata(OperationContext* opCtx,
                                                 const CollectionPtr& fastCountColl,
                                                 const Snapshotted<BSONObj>& doc,
                                                 const BSONObj& newDoc,
                                                 const BSONObj& criteria,
                                                 const RecordId& recordId) {
    // TODO SERVER-117886: Manually performing update without query system. This would be nice to
    // avoid extra dependencies but might be too tricky to get right.
    CollectionUpdateArgs args(doc.value());

    const auto diff = doc_diff::computeOplogDiff(doc.value(), newDoc, /*padding=*/0);
    invariant(
        diff.has_value(),
        fmt::format("Expected computed diff to be smaller than the post-image: pre={}, post={}",
                    doc.value().toString(),
                    newDoc.toString()));

    if (!diff->isEmpty()) {
        args.update = update_oplog_entry::makeDeltaOplogEntry(*diff);
        args.criteria = criteria;
        collection_internal::updateDocument(
            opCtx, fastCountColl, recordId, doc, newDoc, &args.update, nullptr, nullptr, &args);
    } else {
        // TODO(SERVER-122569): Delete this metric and the associated tests.
        _metrics.incrementEmptyUpdateCount();
        LOGV2(11648805,
              "ReplicatedFastCountManager empty update",
              "doc"_attr = doc.value(),
              "newDoc"_attr = newDoc,
              "criteria"_attr = criteria);
    }
}

void ReplicatedFastCountManager::_insertMetadata(OperationContext* opCtx,
                                                 const CollectionPtr& fastCountColl,
                                                 const BSONObj& newDoc) {
    // TODO SERVER-118529: Consider error handling more carefully here.
    massertStatusOK(collection_internal::insertDocument(opCtx,
                                                        fastCountColl,
                                                        InsertStatement(newDoc),
                                                        /*opDebug=*/nullptr));
}

BSONObj ReplicatedFastCountManager::_getDocForWrite(const UUID& uuid,
                                                    const CollectionSizeCount& sizeCount,
                                                    const Timestamp& validAsOfTS) const {
    return BSON("_id" << uuid << replicated_fast_count::kValidAsOfKey << validAsOfTS
                      << replicated_fast_count::kMetadataKey
                      << BSON(replicated_fast_count::kCountKey << sizeCount.count
                                                               << replicated_fast_count::kSizeKey
                                                               << sizeCount.size));
}

BSONObj ReplicatedFastCountManager::_getTimestampDocForWrite(int32_t stripe,
                                                             const Timestamp& validAsOfTs) const {
    return BSON("_id" << stripe << replicated_fast_count::kValidAsOfKey << validAsOfTs);
}

RecordId ReplicatedFastCountManager::_keyForUUID(const UUID& uuid) const {
    auto key =
        record_id_helpers::keyForDoc(BSON("_id" << uuid),
                                     clustered_util::makeDefaultClusteredIdIndex().getIndexSpec(),
                                     /*collator=*/nullptr);
    return key.getValue();
}

UUID ReplicatedFastCountManager::_UUIDForKey(const RecordId key) const {
    return UUID::parse(record_id_helpers::toBSONAs(key, "").firstElement()).getValue();
}

RecordId ReplicatedFastCountManager::_keyForStripe(int32_t stripe) const {
    auto key =
        record_id_helpers::keyForDoc(BSON("_id" << stripe),
                                     clustered_util::makeDefaultClusteredIdIndex().getIndexSpec(),
                                     /*collator=*/nullptr);
    return key.getValue();
}

int32_t ReplicatedFastCountManager::_getStripe() const {
    return 0;
}

}  // namespace mongo
