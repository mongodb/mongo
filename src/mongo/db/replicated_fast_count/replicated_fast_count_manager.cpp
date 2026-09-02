// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/replicated_fast_count/replicated_fast_count_manager.h"

#include "mongo/db/collection_crud/collection_write_path.h"
#include "mongo/db/record_id_helpers.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_advance_checkpoint.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_delta_utils.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_enabled.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_max_oplog_scan_lag_secs_gen.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_streaming_oplog_delta_accumulator.h"
#include "mongo/db/replicated_fast_count/size_count_checkpoint_coordinator.h"
#include "mongo/db/shard_role/lock_manager/d_concurrency.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_catalog/collection_catalog.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/ident.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/version_context.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo::replicated_fast_count {
using namespace std::literals::string_view_literals;

static const ServiceContext::Decoration<ReplicatedFastCountManager> getReplicatedFastCountManager =
    ServiceContext::declareDecoration<ReplicatedFastCountManager>();

ReplicatedFastCountManager& ReplicatedFastCountManager::get(ServiceContext* svcCtx) {
    return getReplicatedFastCountManager(svcCtx);
}

std::pair<bool, boost::optional<Timestamp>> ReplicatedFastCountManager::_computeColdStartTimestamp(
    OperationContext* opCtx, bool returnTimestampToSeekFromIfSkippingScan) {
    auto* storageInterface = repl::StorageInterface::get(opCtx);
    const Timestamp oldestTs = storageInterface->getEarliestOplogTimestamp(opCtx);
    // An empty oplog reports Timestamp::min() as its earliest entry, we should be able to catch up
    // trivially by scanning from the beginning.
    if (oldestTs == Timestamp::min()) {
        return {false, Timestamp::min()};
    }

    // Seed from the last stable recovery timestamp.
    const auto lastStableRecovery =
        opCtx->getServiceContext()->getStorageEngine()->getLastStableRecoveryTimestamp();
    if (!lastStableRecovery || *lastStableRecovery <= oldestTs) {
        // Scan from the beginning.
        return {false, Timestamp::min()};
    }
    const Timestamp seed = *lastStableRecovery;

    const Seconds maxLag{gReplicatedFastCountMaxOplogScanLagSecs.load()};
    const Seconds lag{static_cast<int64_t>(seed.getSecs()) -
                      static_cast<int64_t>(oldestTs.getSecs())};
    if (lag <= maxLag) {
        // We should be able to catch up from the oldest entry, go ahead and perform the scan.
        return {false, Timestamp::min()};
    }

    LOGV2_WARNING(13060000,
                  "No persisted valid-as-of timestamp found and the oldest oplog entry is too far "
                  "behind the last stable recovery timestamp to catch up from.",
                  "oldestOplogTs"_attr = oldestTs,
                  "lastStableRecoveryTs"_attr = seed,
                  "maxOplogScanLagSecs"_attr = maxLag.count());

    if (!returnTimestampToSeekFromIfSkippingScan) {
        return {true, boost::none};
    }

    AutoGetOplogFastPath oplogRead(opCtx, OplogAccessMode::kRead);
    const auto& oplogColl = oplogRead.getCollection();
    massert(13060001, "oplog collection not found", oplogColl);
    auto cursor =
        oplogColl->getRecordStore()->getCursor(opCtx, *shard_role_details::getRecoveryUnit(opCtx));
    auto rec = cursor->seek(uassertStatusOK(record_id_helpers::keyForOptime(seed, KeyFormat::Long)),
                            SeekableRecordCursor::BoundInclusion::kInclude);
    invariant(rec);
    return {true, Timestamp(static_cast<unsigned long long>(rec->id.getLong()))};
}

void ReplicatedFastCountManager::initializeFastCountCommitFn() {
    setFastCountCommitFn([](OperationContext* opCtx, UncommittedFastCountChangeMap& changes) {
        getReplicatedFastCountManager(opCtx->getServiceContext()).commit(opCtx, changes);
    });
}

void ReplicatedFastCountManager::initializeContainerStores(
    std::unique_ptr<RecordStore> metadataRS, std::unique_ptr<RecordStore> timestampsRS) {
    LOGV2(12231710, "Initializing container stores");
    invariant(metadataRS, "metadata RecordStore must not be null");
    invariant(timestampsRS, "timestamps RecordStore must not be null");

    // TODO (SERVER-126250): This can be a nullptr check once we change the
    // ReplicatedFastCountManager() constructor to default initialize Collection stores.
    if (_sizeCountStore->usesContainers()) {
        massert(13337202,
                "Timestamp store must use containers when the size/count store uses containers",
                _timestampStore->usesContainers());
        LOGV2(13337200, "Replicated fast count container stores are already initialized; skipping");
        return;
    }

    _sizeCountStore =
        std::make_unique<replicated_fast_count::ContainerSizeCountStore>(std::move(metadataRS));
    _timestampStore = std::make_unique<replicated_fast_count::ContainerSizeCountTimestampStore>(
        std::move(timestampsRS));
}

void ReplicatedFastCountManager::startup(OperationContext* opCtx) {
    const UUID oplogUuid = [&] {
        AutoGetOplogFastPath oplogRead(opCtx, OplogAccessMode::kRead);
        const auto& oplogColl = oplogRead.getCollection();
        massert(12912600, "oplog collection not found", oplogColl);
        return oplogColl->uuid();
    }();

    const Timestamp lastPersistedCheckpointTS = [&] {
        Lock::GlobalLock readLock(opCtx, MODE_IS, {.skipRSTLLock = opCtx->isLockFreeReadsOp()});
        return _timestampStore->read(opCtx).value_or(Timestamp{});
    }();

    // If we do not have a persisted valid-as-of timestamp, and if we have to scan a significant
    // amount of oplog to catch up, we skip the scan and instead start scanning from the last
    // applied oplog timestamp. Otherwise, scan from the persisted valid-as-of timestamp. This keeps
    // checkpoints advancing at the cost of potentially incorrect size and count.
    //
    // TODO SERVER-130675: Stop skipping the oplog scan once the fast count system can always catch
    // up in time.
    const Timestamp startCheckpointingAfterTS = [&] {
        if (lastPersistedCheckpointTS != Timestamp{}) {
            return lastPersistedCheckpointTS;
        }
        const auto [shouldSkip, timestamp] =
            _computeColdStartTimestamp(opCtx, /*returnTimestampToSeekFromIfSkippingScan=*/true);
        if (shouldSkip) {
            LOGV2_WARNING(13060002,
                          "Skipping the catch-up scan while starting up the fast count manager. "
                          "Fast size and count may be inaccurate.");
        }
        return *timestamp;
    }();

    std::lock_guard lock(_checkpointerMutex);

    if (_checkpointer) {
        LOGV2(12542400, "ReplicatedFastCountManager already running; skipping startup");
        return;
    }

    LOGV2(12051100, "Starting up ReplicatedFastCountManager checkpoint coordinator");
    _checkpointer = std::make_unique<SizeCountCheckpointCoordinator>(
        *_sizeCountStore, *_timestampStore, oplogUuid, startCheckpointingAfterTS);
    if (!_isUnderTest) {
        _checkpointer->startup(opCtx->getServiceContext());
    }
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
        // Join the checkpointer threads before the final flush.
        checkpointer.reset();

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
            if (storageEngine->getEngine()->hasIdent(ru, ident::kFastCountMetadataStore)) {
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
            }
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
                              "initialization");
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

    // The oplog collection itself may not exist yet at cold boot.
    const bool oplogExists = [&] {
        AutoGetOplogFastPath oplogRead(opCtx, OplogAccessMode::kRead);
        return static_cast<bool>(oplogRead.getCollection());
    }();

    // If we do not have a persisted valid-as-of timestamp, and if we have to scan a significant
    // amount of oplog to catch up, we skip the scan entirely and accept a potentially incorrect
    // size and count. Otherwise, scan from the persisted valid-as-of timestamp (or from the
    // beginning of the oplog if we do not need to scan too much oplog). This prevents blocking
    // startup at the cost of potentialy having inaccurate fast count.
    //
    // TODO SERVER-130675: Stop skipping the oplog scan once the fast count system can always catch
    // up in time.
    const boost::optional<Timestamp> seekAfterTimestamp = [&]() -> boost::optional<Timestamp> {
        if (!oplogExists) {
            return boost::none;
        }
        if (persistedTimestamp) {
            return *persistedTimestamp;
        }
        const auto [skipScan, startFrom] =
            _computeColdStartTimestamp(opCtx, /*returnTimestampToSeekFromIfSkippingScan=*/false);
        if (skipScan) {
            LOGV2(
                13060003,
                "Skipping fast count oplog scan during initialization; fast size and count may be "
                "inaccurate");
            return boost::none;
        }
        return *startFrom;
    }();

    // Scan the oplog from seekAfterTimestamp and accumulate size and count deltas for every UUID
    // that has been updated since the last checkpoint. If we do not have a timestamp to seek from,
    // skip this phase entirely
    if (seekAfterTimestamp) {
        const auto scanResult = [&]() -> OplogScanResult {
            AutoGetOplogFastPath oplogRead(opCtx, OplogAccessMode::kRead);
            const auto& oplogColl = oplogRead.getCollection();
            massert(12554000, "oplog collection not found", oplogColl);

            auto oplogCursor = oplogColl->getRecordStore()->getCursor(
                opCtx, *shard_role_details::getRecoveryUnit(opCtx));
            // We pass the oplog UUID here to include the oplog's own size and count in the
            // aggregation.
            return aggregateReplicatedMetadataDeltasInOplog(*oplogCursor,
                                                            *seekAfterTimestamp,
                                                            oplogColl->uuid(),
                                                            /*isCheckpoint=*/false);
        }();

        for (const auto& [uuid, delta] : scanResult.deltas) {
            accumulator[uuid].count += delta.metadata.sizeCount.count;
            accumulator[uuid].size += delta.metadata.sizeCount.size;
        }

        LOGV2(12554001,
              "ReplicatedFastCountManager oplog scan during initialization complete",
              "seekAfterTimestamp"_attr = *seekAfterTimestamp,
              "metadataEntriesUpdated"_attr = scanResult.deltas.size(),
              "duration"_attr = Date_t::now() - oplogScanStartTime);
    }

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

void ReplicatedFastCountManager::finalizeMetadataFromInitialSync(OperationContext* opCtx) {
    Lock::GlobalLock readLock(opCtx, MODE_IS, {.skipRSTLLock = opCtx->isLockFreeReadsOp()});

    // The bound _timestampStore reflects the donor's checkpoint timestamp seeded during initial
    // sync. All seeded per-collection entries are consistent as of this timestamp, so it is the
    // correct point to begin accumulating oplog deltas from.
    const boost::optional<Timestamp> persistedTimestamp = _timestampStore->read(opCtx);
    const Timestamp seekAfterTimestamp = persistedTimestamp.value_or(Timestamp::min());

    // Scan the oplog once, accumulating size/count deltas per UUID since the checkpoint. Done in a
    // dedicated scope before any collection handles are acquired below, to avoid the lock-cycle
    // fassert in the collection-backed metadata read path.
    SizeCountAccumulator deltasByUuid;
    {
        const Date_t oplogScanStartTime = Date_t::now();
        const auto scanResult = [&]() -> OplogScanResult {
            AutoGetOplogFastPath oplogRead(opCtx, OplogAccessMode::kRead);
            const auto& oplogColl = oplogRead.getCollection();
            massert(12554004, "oplog collection not found", oplogColl);

            auto oplogCursor = oplogColl->getRecordStore()->getCursor(
                opCtx, *shard_role_details::getRecoveryUnit(opCtx));
            // Pass the oplog UUID so the oplog's own size and count are included in the
            // aggregation.
            return aggregateReplicatedMetadataDeltasInOplog(*oplogCursor,
                                                            seekAfterTimestamp,
                                                            oplogColl->uuid(),
                                                            /*isCheckpoint=*/false);
        }();
        for (const auto& [uuid, delta] : scanResult.deltas) {
            deltasByUuid[uuid].count += delta.metadata.sizeCount.count;
            deltasByUuid[uuid].size += delta.metadata.sizeCount.size;
        }
        LOGV2(12554005,
              "ReplicatedFastCountManager oplog scan during initial sync finalization complete",
              "seekAfterTimestamp"_attr = seekAfterTimestamp,
              "metadataEntriesUpdated"_attr = scanResult.deltas.size(),
              "duration"_attr = Date_t::now() - oplogScanStartTime);
    }

    // Gather the eligible collections up front so the per-collection persisted reads below do not
    // acquire a collection (collection-backed read path) while iterating the catalog. The catalog
    // snapshot keeps each RecordStore alive for the duration of this function.
    const auto catalog = CollectionCatalog::latest(opCtx->getServiceContext());
    int numFromCheckpoint = 0;
    for (const auto& dbName : catalog->getAllDbNames()) {
        for (const auto& coll : catalog->range(dbName)) {
            if (!isReplicatedFastCountEligible(coll->ns())) {
                continue;
            }
            const auto& uuid = coll->uuid();
            auto recordStore = coll->getRecordStore();
            const auto persisted = _sizeCountStore->read(opCtx, uuid);
            // The persisted entry is authoritative as of the checkpoint timestamp; add every oplog
            // delta since then to obtain the final count, avoiding a full collection scan. If there
            // is no persisted entry, that collection's size and count haven't been flushed yet so
            // treat that as 0 size/0 count.
            int64_t size = 0;
            int64_t count = 0;
            if (persisted) {
                size = persisted->size;
                count = persisted->count;
            }

            if (auto it = deltasByUuid.find(uuid); it != deltasByUuid.end()) {
                size += it->second.size;
                count += it->second.count;
            }
            recordStore->setAccurateSizeCount(size, count);
            ++numFromCheckpoint;
        }
    }

    LOGV2(12554006,
          "Reconciled in-memory replicated fast count after initial sync",
          "numCollectionsFromCheckpoint"_attr = numFromCheckpoint,
          "numOplogDeltas"_attr = deltasByUuid.size());
}

void ReplicatedFastCountManager::commit(OperationContext* opCtx,
                                        UncommittedFastCountChangeMap& changes) {
    for (auto& [uuid, change] : changes) {
        if (change.delta.count == 0 && change.delta.size == 0) {
            continue;
        }
        invariant(change.recordStore,
                  fmt::format("Missing RecordStore for fast count change on collection {}",
                              uuid.toString()));
        change.recordStore->adjustAccurateSizeCount(change.delta.size, change.delta.count);
        // TODO SERVER-120203: Re-enable this invariant once outstanding bugs are fixed.
        // invariant(stored.metadata.sizeCount.size >= 0 && stored.metadata.sizeCount.count >= 0,
        //           fmt::format("Expected fast count size and count to be non-negative, but saw
        //           size "
        //                       "{} and count {}",
        //                       stored.metadata.sizeCount.size,
        //                       stored.metadata.sizeCount.count));
    }
}

boost::optional<std::pair<CollectionReplicatedMetadata, Timestamp>>
ReplicatedFastCountManager::findPersisted(OperationContext* opCtx, UUID uuid) const {
    const auto entry = _sizeCountStore->read(opCtx, uuid);
    if (!entry) {
        return boost::none;
    }
    return std::pair{
        CollectionReplicatedMetadata{
            .sizeCount = CollectionSizeCount{.size = entry->size, .count = entry->count},
            .hash = entry->hash},
        entry->timestamp};
}

boost::optional<Timestamp> ReplicatedFastCountManager::findPersistedTimestampStoreTs(
    OperationContext* opCtx) const {
    return _timestampStore->read(opCtx);
}

void ReplicatedFastCountManager::populateFromInitialSync(
    OperationContext* opCtx,
    const std::vector<std::pair<UUID, FastCountEntry>>& entries,
    boost::optional<Timestamp> timestampStoreTs) {
    LOGV2(12549701,
          "Populating replicated fast count stores from initial sync",
          "numEntries"_attr = entries.size(),
          "timestampStoreTs"_attr = timestampStoreTs);
    {
        // Use writeToTable() rather than write() here: this node is still in INITIAL_SYNC, so it
        // cannot accept replicated writes and these entries are seeded locally without going
        // through the oplog.
        WriteUnitOfWork wuow(opCtx);
        for (const auto& [uuid, entry] : entries) {
            _sizeCountStore->writeToTable(
                opCtx, uuid, SizeCountStore::Entry{entry.timestamp, entry.size, entry.count});
        }
        if (timestampStoreTs) {
            _timestampStore->writeToTable(opCtx, *timestampStoreTs);
        }
        wuow.commit();
    }
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
    _isUnderTest = true;
}

bool ReplicatedFastCountManager::isRunning_ForTest() {
    std::lock_guard lock(_checkpointerMutex);
    return _checkpointer && _checkpointer->isRunning_ForTest();
}

bool ReplicatedFastCountManager::usesContainers_ForTest() const {
    tassert(13337201,
            "Size/count store and timestamp store must agree on whether they use containers",
            _sizeCountStore->usesContainers() == _timestampStore->usesContainers());
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
