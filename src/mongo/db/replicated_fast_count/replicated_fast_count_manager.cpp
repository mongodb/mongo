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
#include "mongo/db/replicated_fast_count/replicated_fast_count_advance_checkpoint.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_delta_utils.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_enabled.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_streaming_oplog_delta_accumulator.h"
#include "mongo/db/replicated_fast_count/size_count_checkpoint_coordinator.h"
#include "mongo/db/shard_role/lock_manager/d_concurrency.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_catalog/collection_catalog.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/ident.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/version_context.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

MONGO_FAIL_POINT_DEFINE(hangAfterReplicatedFastCountSnapshot);

namespace mongo::replicated_fast_count {
using namespace std::literals::string_view_literals;

namespace {

MONGO_FAIL_POINT_DEFINE(sleepAfterFlush);
MONGO_FAIL_POINT_DEFINE(failDuringFlush);
MONGO_FAIL_POINT_DEFINE(hangBeforePersistingNewFastCountEntries);

}  // namespace

static const ServiceContext::Decoration<ReplicatedFastCountManager> getReplicatedFastCountManager =
    ServiceContext::declareDecoration<ReplicatedFastCountManager>();

ReplicatedFastCountManager& ReplicatedFastCountManager::get(ServiceContext* svcCtx) {
    return getReplicatedFastCountManager(svcCtx);
}

void ReplicatedFastCountManager::initializeFastCountCommitFn() {
    setFastCountCommitFn([](OperationContext* opCtx,
                            const boost::container::flat_map<UUID, CollectionSizeCount>& changes) {
        getReplicatedFastCountManager(opCtx->getServiceContext()).commit(opCtx, changes);
    });
}

void ReplicatedFastCountManager::initializeContainerStores(
    std::unique_ptr<RecordStore> metadataRS, std::unique_ptr<RecordStore> timestampsRS) {
    LOGV2(12231710, "Initializing container stores");
    invariant(metadataRS, "metadata RecordStore must not be null");
    invariant(timestampsRS, "timestamps RecordStore must not be null");
    _sizeCountStore =
        std::make_unique<replicated_fast_count::ContainerSizeCountStore>(std::move(metadataRS));
    _timestampStore = std::make_unique<replicated_fast_count::ContainerSizeCountTimestampStore>(
        std::move(timestampsRS));
}

void ReplicatedFastCountManager::startup(OperationContext* opCtx) {
    if (!_sizeCountStore->usesContainers()) {
        massert(11718600,
                "Expected fastcount collection to exist on startup",
                acquireFastCountCollectionForRead(opCtx).has_value());
    }

    const Timestamp lastPersistedCheckpointTS = [&] {
        Lock::GlobalLock readLock(opCtx, MODE_IS, {.skipRSTLLock = opCtx->isLockFreeReadsOp()});
        return _timestampStore->read(opCtx).value_or(Timestamp{});
    }();

    const UUID oplogUuid = [&] {
        AutoGetOplogFastPath oplogRead(opCtx, OplogAccessMode::kRead);
        const auto& oplogColl = oplogRead.getCollection();
        massert(12912600, "oplog collection not found", oplogColl);
        return oplogColl->uuid();
    }();

    std::lock_guard lock(_checkpointerMutex);

    if (_checkpointer) {
        LOGV2(12542400, "ReplicatedFastCountManager already running; skipping startup");
        return;
    }

    LOGV2(12051100, "Starting up ReplicatedFastCountManager checkpoint coordinator");
    _checkpointer = std::make_unique<SizeCountCheckpointCoordinator>(
        *_sizeCountStore, *_timestampStore, oplogUuid);
    if (!_isUnderTest) {
        _checkpointer->startup(opCtx->getServiceContext(), lastPersistedCheckpointTS);
    }

    setIsRunning(true);
}

void ReplicatedFastCountManager::shutdown(OperationContext* opCtx) {
    LOGV2(11648800, "Shutting down ReplicatedFastCountManager");

    std::unique_ptr<SizeCountCheckpointCoordinator> checkpointer;
    {
        std::lock_guard lock(_checkpointerMutex);
        checkpointer = std::move(_checkpointer);
        if (!checkpointer) {
            return;
        }
    }

    if (!_isUnderTest) {
        checkpointer->shutdown();
        // Final synchronous flush after checkpoint coordinator threads have stopped.
        try {
            advanceCheckpoint(opCtx, *_sizeCountStore, *_timestampStore);
        } catch (const DBException& ex) {
            if (ex.code() == ErrorCodes::InterruptedDueToReplStateChange ||
                ex.code() == ErrorCodes::NotWritablePrimary) {
                LOGV2_DEBUG(12101806,
                            2,
                            "ReplicatedFastCountManager final checkpoint flush interrupted",
                            "error"_attr = ex.toStatus());
            } else {
                LOGV2_WARNING(12101807,
                              "ReplicatedFastCountManager failed to flush on shutdown",
                              "error"_attr = ex.toStatus());
            }
        }
    }

    LOGV2(12101800, "ReplicatedFastCountManager stopped");
    setIsRunning(false);
}

int ReplicatedFastCountManager::_hydrateMetadataFromContainer(
    OperationContext* opCtx,
    SizeCountAccumulator& accumulator,
    const RecordStore::RecordStoreContainer& containerVariant) {
    int numRecordsScanned = 0;

    auto& ru = *shard_role_details::getRecoveryUnit(opCtx);
    massert(12231702,
            "Expected replicated fast count metadata record store to hold a StringKeyedContainer",
            std::holds_alternative<std::reference_wrapper<StringKeyedContainer>>(containerVariant));
    auto& container =
        std::get<std::reference_wrapper<StringKeyedContainer>>(containerVariant).get();
    auto cursor = container.getCursor(ru);

    while (auto record = cursor->next()) {
        const auto& [keySpan, valueSpan] = *record;
        const auto uuid = UUID::fromCDR(ConstDataRange(keySpan.data(), keySpan.size()));
        const BSONObj data(valueSpan.data());

        BSONObj metadataField = data.getField(replicated_fast_count::kMetadataKey).Obj();
        accumulator[uuid].size += metadataField.getField(kSizeKey).Long();
        accumulator[uuid].count += metadataField.getField(kCountKey).Long();

        ++numRecordsScanned;
    }

    return numRecordsScanned;
}

int ReplicatedFastCountManager::_hydrateMetadataFromCollection(
    OperationContext* opCtx,
    SizeCountAccumulator& accumulator,
    const CollectionOrViewAcquisition& acquisition) {
    int numRecordsScanned = 0;
    auto cursor = acquisition.getCollectionPtr()->getCursor(opCtx);
    while (auto record = cursor->next()) {
        const UUID uuid = _UUIDForKey(record->id);
        const BSONObj data = record->data.releaseToBson();

        accumulator[uuid].size += data.getField(kMetadataKey).Obj().getField(kSizeKey).Long();
        accumulator[uuid].count += data.getField(kMetadataKey).Obj().getField(kCountKey).Long();

        ++numRecordsScanned;
    }
    return numRecordsScanned;
}

void ReplicatedFastCountManager::initializeMetadata(OperationContext* opCtx) {
    // Accumulates size/count values per collection UUID. Entries may be inserted by the fast count
    // collection scan and/or the oplog scan.
    SizeCountAccumulator accumulator;

    Lock::GlobalLock readLock(opCtx, MODE_IS, {.skipRSTLLock = opCtx->isLockFreeReadsOp()});
    bool useContainers = shouldUseReplicatedFastCountContainers(opCtx);
    {
        // Initialize the in-memory map by loading all persisted collection size/count information.
        // The block scope is required to avoid a lock cycle fassert in the collection path when
        // reading the oplog below.
        const auto startTime = Date_t::now();
        int numRecordsScanned = 0;

        if (useContainers) {
            // TODO SERVER-126250: We should only need the nullptr check since we won't have a
            // non-null CollectionSizeCountStore pointer.
            massert(12231701,
                    "_sizeCountStore should be uninitialized when initializeMetadata is called",
                    !_sizeCountStore || !_sizeCountStore->usesContainers());
            auto* storageEngine = opCtx->getServiceContext()->getStorageEngine();
            auto& ru = *shard_role_details::getRecoveryUnit(opCtx);
            if (!storageEngine->getEngine()->hasIdent(ru, ident::kFastCountMetadataStore)) {
                // This should only be the case on cold boot.
                LOGV2(12231703, "Internal fastcount container not present during initialization.");
                return;
            }
            // This RecordStore will be destroyed after hydrating the metadata since only one
            // RecordStore object can exist per ident.
            auto recordStore = storageEngine->getEngine()->getRecordStore(
                opCtx,
                NamespaceString::kAdminCommandNamespace,
                ident::kFastCountMetadataStore,
                RecordStore::Options{.keyFormat = KeyFormat::String},
                /*uuid=*/boost::none);
            massert(12231700, "Storage engine returned a null RecordStore", recordStore);
            numRecordsScanned =
                _hydrateMetadataFromContainer(opCtx, accumulator, recordStore->getContainer());
        } else {
            auto acquisition = replicated_fast_count::acquireFastCountCollectionForRead(opCtx);
            if (!acquisition.has_value()) {
                // This should only be the case on cold boot.
                LOGV2(11999600, "Internal fastcount collection not present during initialization.");
                return;
            }
            numRecordsScanned = _hydrateMetadataFromCollection(opCtx, accumulator, *acquisition);
        }

        LOGV2(11648801,
              "ReplicatedFastCountManager persisted size/count information read complete",
              "storeType"_attr = useContainers ? "container"sv : "collection"sv,
              "numRecordsScanned"_attr = numRecordsScanned,
              "duration"_attr = Date_t::now() - startTime);
    }

    // In container mode, _timestampStore is still the collection-backed implementation because
    // initializeContainerStores() is called after initializeMetadata(). Read directly from the
    // storage engine like the metadata hydration above does.
    const boost::optional<Timestamp> persistedTimestamp = [&]() -> boost::optional<Timestamp> {
        if (useContainers) {
            auto* storageEngine = opCtx->getServiceContext()->getStorageEngine();
            auto& ru = *shard_role_details::getRecoveryUnit(opCtx);
            if (!storageEngine->getEngine()->hasIdent(ru,
                                                      ident::kFastCountMetadataStoreTimestamps)) {
                LOGV2_WARNING(12743500,
                              "Internal fastcount Timestamps container did not exist during "
                              "initialization even though the Metadata container did");
                return boost::none;
            }
            auto timestampRS = storageEngine->getEngine()->getRecordStore(
                opCtx,
                NamespaceString::kAdminCommandNamespace,
                ident::kFastCountMetadataStoreTimestamps,
                RecordStore::Options{.keyFormat = KeyFormat::Long},
                /*uuid=*/boost::none);
            massert(
                12580002, "Storage engine returned a null RecordStore for timestamps", timestampRS);
            ContainerSizeCountTimestampStore tempStore(std::move(timestampRS));
            return tempStore.read(opCtx);
        }
        return _timestampStore->read(opCtx);
    }();

    const Date_t oplogScanStartTime = Date_t::now();

    const Timestamp seekAfterTimestamp = persistedTimestamp.value_or(Timestamp::min());

    // Scan the oplog from seekAfterTimestamp and accumulate size and count deltas for every
    // UUID that has been updated since the last checkpoint.
    const auto scanResult = [&]() -> OplogScanResult {
        AutoGetOplogFastPath oplogRead(opCtx, OplogAccessMode::kRead);
        const auto& oplogColl = oplogRead.getCollection();
        massert(12554000, "oplog collection not found", oplogColl);

        auto oplogCursor = oplogColl->getRecordStore()->getCursor(
            opCtx, *shard_role_details::getRecoveryUnit(opCtx));
        // We pass the oplog UUID here to include the oplog's own size and count in the
        // aggregation.
        return aggregateSizeCountDeltasInOplog(
            *oplogCursor, seekAfterTimestamp, oplogColl->uuid(), /*isCheckpoint=*/false);
    }();

    for (const auto& [uuid, delta] : scanResult.deltas) {
        accumulator[uuid].count += delta.sizeCount.count;
        accumulator[uuid].size += delta.sizeCount.size;
    }

    LOGV2(12554001,
          "ReplicatedFastCountManager oplog scan during initialization complete",
          "seekAfterTimestamp"_attr = seekAfterTimestamp,
          "metadataEntriesUpdated"_attr = scanResult.deltas.size(),
          "duration"_attr = Date_t::now() - oplogScanStartTime);

    const auto catalog = CollectionCatalog::latest(opCtx->getServiceContext());
    int numInitialized = 0;
    for (const auto& dbName : catalog->getAllDbNames()) {
        for (const auto& coll : catalog->range(dbName)) {
            if (!isReplicatedFastCountEligible(coll->ns())) {
                continue;
            }

            if (auto it = accumulator.find(coll->uuid()); it != accumulator.end()) {
                coll->getRecordStore()->setAccurateSizeCount(it->second.size, it->second.count);
            } else {
                // The oplog collection can have a different UUID on every node. When this happens,
                // the accumulator will not contain an entry for this node's oplog UUID because the
                // UUID will not be tracked in the fast count store or the oplog.
                if (coll->ns() != NamespaceString::kRsOplogNamespace) {
                    // TODO(SERVER-126350): Investigate why this log is sometimes emitted.
                    LOGV2_WARNING(
                        12580001,
                        "Replicated fast count eligible namespace found in the collection "
                        "catalog but not tracked in the metadata store or the oplog",
                        "nss"_attr = coll->ns().toStringForErrorMsg(),
                        "uuid"_attr = coll->uuid());
                }
            }

            ++numInitialized;
        }
    }

    LOGV2(12580000,
          "Initialized RecordStore replicated size counts from fast count metadata",
          "numCollectionsInitialized"_attr = numInitialized,
          "numEntriesInAccumulator"_attr = accumulator.size());

    // Seed the in-memory checkpoint timestamp from disk so the `oplog_lag_secs` gauge has a real
    // baseline on warm restart. Without this, the gauge would stay at 0 until the first post-boot
    // checkpoint flush (primary) or the first oplog-applied write to the timestamp store
    // (secondary).
    if (persistedTimestamp) {
        recordCheckpointAdvanced(*persistedTimestamp);
    }
}

void ReplicatedFastCountManager::commit(
    OperationContext* opCtx, const boost::container::flat_map<UUID, CollectionSizeCount>& changes) {
    const auto catalog = CollectionCatalog::latest(opCtx->getServiceContext());
    for (const auto& [uuid, delta] : changes) {
        if (delta.count == 0 && delta.size == 0) {
            continue;
        }
        const Collection* collection = catalog->lookupCollectionByUUID(opCtx, uuid);
        // In a single WUOW, if the collection size/count is changed and then the collection is
        // dropped, the collection will be removed from the catalog before we attempt to write these
        // uncommitted size/count changes. So, we necessarily skip updating the collection's record
        // store.
        if (!collection) {
            continue;
        }
        collection->getRecordStore()->adjustAccurateSizeCount(delta.size, delta.count);
        // TODO SERVER-120203: Re-enable this invariant once outstanding bugs are fixed.
        // invariant(stored.sizeCount.size >= 0 && stored.sizeCount.count >= 0,
        //           fmt::format("Expected fast count size and count to be non-negative, but saw
        //           size "
        //                       "{} and count {}",
        //                       stored.sizeCount.size,
        //                       stored.sizeCount.count));
    }
}

boost::optional<std::pair<CollectionSizeCount, Timestamp>>
ReplicatedFastCountManager::findPersisted(OperationContext* opCtx, UUID uuid) const {
    const auto entry = _sizeCountStore->read(opCtx, uuid);
    if (!entry) {
        return boost::none;
    }
    return std::pair{CollectionSizeCount{.size = entry->size, .count = entry->count},
                     entry->timestamp};
}

boost::optional<Timestamp> ReplicatedFastCountManager::findPersistedTimestampStoreTs(
    OperationContext* opCtx) const {
    return _timestampStore->read(opCtx);
}

void ReplicatedFastCountManager::flushAsync() {
    std::lock_guard lock(_checkpointerMutex);
    if (_checkpointer) {
        _checkpointer->requestFlush();
    }
}

void ReplicatedFastCountManager::flushSync_ForTest(OperationContext* opCtx) {
    std::lock_guard lock(_checkpointerMutex);
    invariant(_checkpointer, "flushSync_ForTest() requires startup() to have been called");
    _checkpointer->flushSync_ForTest(opCtx);
}

void ReplicatedFastCountManager::disablePeriodicWrites_ForTest() {
    invariant(!_checkpointer, "flushSync_ForTest() requires startup() to have been called");
    _isUnderTest = true;
}

bool ReplicatedFastCountManager::isRunning_ForTest() {
    std::lock_guard lock(_checkpointerMutex);
    return _checkpointer && _checkpointer->isRunning_ForTest();
}

bool ReplicatedFastCountManager::usesContainers_ForTest() const {
    return _sizeCountStore->usesContainers();
}

std::pair<SizeCountStore*, SizeCountTimestampStore*>
ReplicatedFastCountManager::getSizeCountStores_ForTest() const {
    return {_sizeCountStore.get(), _timestampStore.get()};
}

UUID ReplicatedFastCountManager::_UUIDForKey(const RecordId key) const {
    return UUID::parse(record_id_helpers::toBSONAs(key, "").firstElement()).getValue();
}

}  // namespace mongo::replicated_fast_count
