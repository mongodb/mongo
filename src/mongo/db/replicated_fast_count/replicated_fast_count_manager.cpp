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
#include "mongo/db/commands/server_status/server_status.h"
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

/**
 * Aggregate metrics for the replicated fast count collection reported by server status.
 */
class ReplicatedFastCountSSS : public ServerStatusSection {
public:
    using ServerStatusSection::ServerStatusSection;

    bool includeByDefault() const final {
        return true;
    }

    BSONObj generateSection(OperationContext* opCtx, const BSONElement& configElement) const final {
        BSONObjBuilder replicatedFastCount;

        replicatedFastCount.append("isRunning", isRunning.loadRelaxed());

        replicatedFastCount.append("flushSuccessCount", flushSuccessCount.loadRelaxed());
        replicatedFastCount.append("flushFailureCount", flushFailureCount.loadRelaxed());
        replicatedFastCount.append(
            "flushTimeMsMin", flushTimeMsMax.loadRelaxed() > 0 ? flushTimeMsMin.loadRelaxed() : 0);
        replicatedFastCount.append("flushTimeMsMax", flushTimeMsMax.loadRelaxed());
        replicatedFastCount.append("flushTimeMsTotal", flushTimeMsTotal.loadRelaxed());
        replicatedFastCount.append(
            "flushedDocsMin", flushedDocsMax.loadRelaxed() > 0 ? flushedDocsMin.loadRelaxed() : 0);
        replicatedFastCount.append("flushedDocsMax", flushedDocsMax.loadRelaxed());
        const auto successCount = flushSuccessCount.loadRelaxed();
        replicatedFastCount.append(
            "flushedDocsAvg",
            successCount > 0 ? static_cast<double>(flushedDocsTotal.loadRelaxed()) / successCount
                             : 0);

        replicatedFastCount.append("emptyUpdateCount", emptyUpdateCount.loadRelaxed());

        replicatedFastCount.append("insertCount", insertCount.loadRelaxed());
        replicatedFastCount.append("updateCount", updateCount.loadRelaxed());
        replicatedFastCount.append("writeTimeMsTotal", writeTimeMsTotal.loadRelaxed());

        return replicatedFastCount.obj();
    }

    /**
     * Records metrics for a successful flush.
     */
    void recordFlush(Date_t startTime, size_t batchSize) {
        const int64_t elapsedTime = (Date_t::now() - startTime).count();
        flushSuccessCount.addAndFetch(1);
        flushTimeMsMin.storeRelaxed(std::min(flushTimeMsMin.loadRelaxed(), elapsedTime));
        flushTimeMsMax.storeRelaxed(std::max(flushTimeMsMax.loadRelaxed(), elapsedTime));
        flushTimeMsTotal.addAndFetch(elapsedTime);
        flushedDocsMin.storeRelaxed(
            std::min(flushedDocsMin.loadRelaxed(), static_cast<int64_t>(batchSize)));
        flushedDocsMax.storeRelaxed(
            std::max(flushedDocsMax.loadRelaxed(), static_cast<int64_t>(batchSize)));
        flushedDocsTotal.addAndFetch(batchSize);
    }

    // Boolean flag indicating whether or not the fast count background thread is currently running.
    Atomic<bool> isRunning{false};

    // Flushes persist fast count information to the oplog and occur during checkpointing,
    // shutdown, step down, etc. The total number of flush attempts = flushSuccessCount +
    // flushFailureCount.
    Atomic<int64_t> flushSuccessCount{0};
    Atomic<int64_t> flushFailureCount{0};
    Atomic<int64_t> flushTimeMsMin{std::numeric_limits<int64_t>::max()};
    Atomic<int64_t> flushTimeMsMax{0};
    Atomic<int64_t> flushTimeMsTotal{0};
    // Aggregate metrics for the min/max number of documents inserted or updated during one flush.
    Atomic<int64_t> flushedDocsMin{std::numeric_limits<int>::max()};
    Atomic<int64_t> flushedDocsMax{0};
    // The total number of documents written during flushes. Used to compute the average flush size.
    Atomic<int64_t> flushedDocsTotal{0};

    // The number of times an empty diff is found when writing an update to the replicated fast
    // count collection.
    Atomic<int64_t> emptyUpdateCount{0};

    // The number of inserts into a new record for storing size and count data.
    Atomic<int64_t> insertCount{0};
    // The number of update to an existing record storing size and count data.
    Atomic<int64_t> updateCount{0};
    // The total time spent writing metadata to the replicated fast count collection.
    // writeTimeMsTotal / flushTimeMsTotal = the proportion of iteration time writing dirty
    // metadata.
    Atomic<int64_t> writeTimeMsTotal{0};
};

ReplicatedFastCountSSS& replicatedFastCountSSS =
    *ServerStatusSectionBuilder<ReplicatedFastCountSSS>("replicatedFastCount").forShard();

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
            _acquireFastCountCollectionForRead(opCtx).has_value());

    LOGV2(12051100, "Starting up ReplicatedFastCountManager thread");

    if (!_isUnderTest) {
        _isEnabled.store(true);
        _backgroundThread = stdx::thread(
            &ReplicatedFastCountManager::_startBackgroundThread, this, opCtx->getServiceContext());
    }

    replicatedFastCountSSS.isRunning.store(true);
}

void ReplicatedFastCountManager::shutdown(OperationContext* opCtx) {
    LOGV2(11648800, "Shutting down ReplicatedFastCountManager");
    if (!_backgroundThread.joinable()) {
        LOGV2(12150400,
              "ReplicatedFastCountManager background thread is not running; skipping shutdown");
        return;
    }

    // Shutdown background thread.
    _isEnabled.store(false);
    _backgroundThreadReadyForFlush.notify_one();
    _backgroundThread.join();

    flushSync(opCtx);

    LOGV2(11751501, "ReplicatedFastCountManager stopped");

    replicatedFastCountSSS.isRunning.store(false);
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
        meta.sizeCount.count = data.getField(kMetaDataKey).Obj().getField(kCountKey).Long();
        meta.sizeCount.size = data.getField(kMetaDataKey).Obj().getField(kSizeKey).Long();
        meta.validAsOf = data.getField(kValidAsOfKey).timestamp();
    }
    return numRecordsScanned;
}

void ReplicatedFastCountManager::initializeMetadata(OperationContext* opCtx) {

    auto acquisition = _acquireFastCountCollectionForRead(opCtx);

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
    {
        stdx::unique_lock lock(_metadataMutex);
        dirtyMetadata = _getAndClearSnapshotOfDirtyMetadata(lock);
    }

    if (MONGO_unlikely(hangAfterReplicatedFastCountSnapshot.shouldFail())) {
        hangAfterReplicatedFastCountSnapshot.pauseWhileSet();
    }

    _acquireAndFlush(opCtx, dirtyMetadata);
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
                                                  const FastSizeCountMap& dirtyMetadata) {
    auto acquisition = _acquireFastCountCollectionForWrite(opCtx);
    massert(ErrorCodes::NamespaceNotFound, "Expected fastcount collection to exist", acquisition);

    const CollectionPtr& fastCountColl = acquisition->getCollectionPtr();
    invariant(fastCountColl,
              str::stream()
                  << "Expected to acquire fastcount store as a collection, not a view. isView : "
                  << acquisition->isView());

    try {
        _doFlush(opCtx, fastCountColl, dirtyMetadata);
    } catch (const DBException& ex) {
        replicatedFastCountSSS.flushFailureCount.addAndFetch(1);
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
                                          const CollectionPtr& coll,
                                          const FastSizeCountMap& dirtyMetadata) {
    if (MONGO_unlikely(failDuringFlush.shouldFail())) {
        uasserted(11550800, "Injected failure in _doFlush() for testing");
    }

    const Date_t startTime = Date_t::now();
    WriteUnitOfWork wuow(opCtx, WriteUnitOfWork::kGroupForPossiblyRetryableOperations);
    for (auto&& [metadataKey, metadataVal] : dirtyMetadata) {
        _writeOneMetadata(opCtx,
                          coll,
                          metadataKey,
                          metadataVal.sizeCount,
                          metadataVal.validAsOf,
                          _keyForUUID(metadataKey));
    }
    wuow.commit();
    replicatedFastCountSSS.writeTimeMsTotal.addAndFetch((Date_t::now() - startTime).count());
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
        }

        const Date_t flushStartTime = Date_t::now();
        try {
            auto opCtx = cc().makeOperationContext();
            _acquireAndFlush(opCtx.get(), dirtyMetadata);
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

        replicatedFastCountSSS.recordFlush(flushStartTime, dirtyMetadata.size());
    }
}

void ReplicatedFastCountManager::_writeOneMetadata(OperationContext* opCtx,
                                                   const CollectionPtr& fastCountColl,
                                                   const UUID& uuid,
                                                   const CollectionSizeCount& sizeCount,
                                                   const Timestamp& validAsOfTS,
                                                   const RecordId& recordId) {
    Snapshotted<BSONObj> doc;
    bool exists = fastCountColl->findDoc(opCtx, recordId, &doc);

    if (exists) {
        replicatedFastCountSSS.updateCount.addAndFetch(1);
        _updateOneMetadata(opCtx, fastCountColl, doc, uuid, sizeCount, validAsOfTS, recordId);
    } else {
        replicatedFastCountSSS.insertCount.addAndFetch(1);
        _insertOneMetadata(opCtx, fastCountColl, uuid, sizeCount, validAsOfTS);
    }
}

void ReplicatedFastCountManager::_updateOneMetadata(OperationContext* opCtx,
                                                    const CollectionPtr& fastCountColl,
                                                    const Snapshotted<BSONObj>& doc,
                                                    const UUID& uuid,
                                                    const CollectionSizeCount& sizeCount,
                                                    const Timestamp& validAsOfTS,
                                                    const RecordId& recordId) {
    // TODO SERVER-117886: Manually performing update without query system. This would be nice to
    // avoid extra dependencies but might be too tricky to get right.
    CollectionUpdateArgs args(doc.value());
    // TODO SERVER-117654: When we also store timestamp we should be able to recover/combine data
    // from old doc to keep this accurate.
    const BSONObj newDoc = _getDocForWrite(uuid, sizeCount, validAsOfTS);

    const auto diff = doc_diff::computeOplogDiff(doc.value(), newDoc, /*padding=*/0);
    invariant(
        diff.has_value(),
        fmt::format("Expected computed diff to be smaller than the post-image: pre={}, post={}",
                    doc.value().toString(),
                    newDoc.toString()));

    if (!diff->isEmpty()) {
        args.update = update_oplog_entry::makeDeltaOplogEntry(*diff);
        args.criteria = BSON("_id" << uuid);
        collection_internal::updateDocument(
            opCtx, fastCountColl, recordId, doc, newDoc, &args.update, nullptr, nullptr, &args);
    } else {
        replicatedFastCountSSS.emptyUpdateCount.addAndFetch(1);
        LOGV2(11648805, "ReplicatedFastCountManager empty update", "uuid"_attr = uuid);
    }
}

void ReplicatedFastCountManager::_insertOneMetadata(OperationContext* opCtx,
                                                    const CollectionPtr& fastCountColl,
                                                    const UUID& uuid,
                                                    const CollectionSizeCount& sizeCount,
                                                    const Timestamp& validAsOfTS) {
    // TODO SERVER-118529: Consider error handling more carefully here.
    massertStatusOK(collection_internal::insertDocument(
        opCtx,
        fastCountColl,
        InsertStatement(_getDocForWrite(uuid, sizeCount, validAsOfTS)),
        /*opDebug=*/nullptr));
}

boost::optional<CollectionOrViewAcquisition>
ReplicatedFastCountManager::_acquireFastCountCollectionForWrite(OperationContext* opCtx) {
    CollectionOrViewAcquisition acquisition = acquireCollectionOrView(
        opCtx,
        CollectionOrViewAcquisitionRequest::fromOpCtx(
            opCtx,
            NamespaceString::makeGlobalConfigCollection(NamespaceString::kReplicatedFastCountStore),
            AcquisitionPrerequisites::OperationType::kWrite),
        LockMode::MODE_IX);

    if (acquisition.getCollectionPtr()) {
        return acquisition;
    }

    return boost::none;
}

boost::optional<CollectionOrViewAcquisition>
ReplicatedFastCountManager::_acquireFastCountCollectionForRead(OperationContext* opCtx) {
    CollectionOrViewAcquisition acquisition = acquireCollectionOrView(
        opCtx,
        CollectionOrViewAcquisitionRequest::fromOpCtx(
            opCtx,
            NamespaceString::makeGlobalConfigCollection(NamespaceString::kReplicatedFastCountStore),
            AcquisitionPrerequisites::OperationType::kRead),
        LockMode::MODE_IS);

    if (acquisition.getCollectionPtr()) {
        return acquisition;
    }

    return boost::none;
}

BSONObj ReplicatedFastCountManager::_getDocForWrite(const UUID& uuid,
                                                    const CollectionSizeCount& sizeCount,
                                                    const Timestamp& validAsOfTS) const {
    return BSON("_id" << uuid << kValidAsOfKey << validAsOfTS << kMetaDataKey
                      << BSON(kCountKey << sizeCount.count << kSizeKey << sizeCount.size));
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

}  // namespace mongo
