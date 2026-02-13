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
#include "mongo/db/shard_role/shard_catalog/clustered_collection_util.h"
#include "mongo/db/update/document_diff_calculator.h"
#include "mongo/db/update/update_oplog_entry_serialization.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

MONGO_FAIL_POINT_DEFINE(hangAfterReplicatedFastCountSnapshot);

namespace mongo {

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
    // TODO SERVER-117650: Read existing collection to populate in-memory metadata. This is
    // currently executed before oplog application, so if there is an create entry for this
    // collection to apply we will currently miss it.
    uassert(11905700,
            "ReplicatedFastCountManager background thread already running. It should only be "
            "started up once.",
            !_backgroundThread.joinable());
    {
        auto acquisition = _acquireFastCountCollection(opCtx);

        if (acquisition && acquisition->collectionExists()) {
            LOGV2(11648801,
                  "ReplicatedFastCountManager::startup fastcount collection exists, "
                  "initializing sizes and counts");
            stdx::lock_guard lock(_metadataMutex);

            auto cursor = acquisition->getCollectionPtr()->getCursor(opCtx);
            while (auto record = cursor->next()) {
                Record& rec = *record;
                UUID uuid = _UUIDForKey(rec.id);
                BSONObj data = rec.data.releaseToBson();

                auto& meta = _metadata[uuid];
                meta.sizeCount.count = data.getField(kCountKey).Long();
                meta.sizeCount.size = data.getField(kSizeKey).Long();
            }
            LOGV2(11648802, "ReplicatedFastCountManager::startup initialization complete");
        } else {
            LOGV2(11648803,
                  "ReplicatedFastCountManager::startup fastcount collection does not "
                  "exist, no initialization needed");
        }
    }

    _backgroundThread = stdx::thread(
        &ReplicatedFastCountManager::_startBackgroundThread, this, opCtx->getServiceContext());
}

void ReplicatedFastCountManager::shutdown() {
    LOGV2(11648800, "Shutting down ReplicatedFastCountManager");

    {
        stdx::lock_guard lock(_metadataMutex);
        _isDisabled.storeRelaxed(true);
    }

    _condVar.notify_all();

    if (_backgroundThread.joinable()) {
        _backgroundThread.join();
    }
    // TODO SERVER-117515: Once this is being signaled from the checkpoint thread, make sure that we
    // do not miss writing any dirty metadata here.
}

void ReplicatedFastCountManager::commit(
    const boost::container::flat_map<UUID, CollectionSizeCount>& changes,
    boost::optional<Timestamp> commitTime) {
    stdx::lock_guard lock(_metadataMutex);
    for (const auto& [uuid, metadata] : changes) {
        // TODO SERVER-117656: Investigate why we sometimes get zero changes here.
        if (metadata.count == 0 && metadata.size == 0) {
            LOGV2_WARNING(11648808, "ReplicatedFastCountManager, Count & Size == 0");
            continue;
        }
        auto& stored = _metadata[uuid];
        stored.sizeCount.count += metadata.count;
        stored.sizeCount.size += metadata.size;
        stored.dirty = true;
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

void ReplicatedFastCountManager::runIteration_ForTest(OperationContext* opCtx) {
    _runIteration(opCtx);
}

void ReplicatedFastCountManager::disablePeriodicWrites_ForTest() {
    invariant(!_backgroundThread.joinable(),
              "Background thread started running before disabling periodic metadata writes");
    _writeMetadataPeriodically = false;
}

absl::flat_hash_map<UUID, ReplicatedFastCountManager::StoredSizeCount>
ReplicatedFastCountManager::_getSnapshotOfDirtyMetadata() {
    absl::flat_hash_map<UUID, StoredSizeCount> dirtyMetadata;
    {
        stdx::lock_guard lock(_metadataMutex);
        dirtyMetadata.reserve(_metadata.size());
        for (auto&& [metadataKey, metadataValue] : _metadata) {
            if (metadataValue.dirty) {
                dirtyMetadata[metadataKey] = metadataValue;
                metadataValue.dirty = false;
            }
        }
    }
    return dirtyMetadata;
}

void ReplicatedFastCountManager::_flush(
    OperationContext* opCtx,
    const CollectionPtr& coll,
    const absl::flat_hash_map<UUID, StoredSizeCount>& dirtyMetadata) {
    // TODO SERVER-117512: We're performing one write per collection here. But we should be
    // able to bundle many of these writes in a single applyOps using the WUOW
    // grouping interface. Might be a problem with updates.

    for (auto&& [metadataKey, metadataVal] : dirtyMetadata) {
        _writeOneMetadata(
            opCtx, coll, metadataKey, metadataVal.sizeCount, _keyForUUID(metadataKey));
    }
}

void ReplicatedFastCountManager::_startBackgroundThread(ServiceContext* svcCtx) {
    ThreadClient tc(_threadName, svcCtx->getService());
    AuthorizationSession::get(cc())->grantInternalAuthorization();
    auto uniqueOpCtx = tc->makeOperationContext();
    auto opCtx = uniqueOpCtx.get();

    try {
        _runBackgroundThreadOnTimer(opCtx);
    } catch (const DBException& ex) {
        LOGV2_WARNING(11648806,
                      "Failure in thread",
                      "threadName"_attr = _threadName,
                      "error"_attr = ex.toStatus());
    }

    LOGV2(11648804, "ReplicatedFastCountManager exited");
}

void ReplicatedFastCountManager::_runBackgroundThreadOnTimer(OperationContext* opCtx) {
    while (_writeMetadataPeriodically) {
        {
            stdx::unique_lock lock(_metadataMutex);
            // TODO SERVER-117515: We want to signal this from checkpoint thread
            _condVar.wait_for(
                lock, stdx::chrono::seconds(1), [this] { return _isDisabled.loadRelaxed(); });

            if (_isDisabled.loadRelaxed()) {
                break;
            }
        }

        try {
            _runIteration(opCtx);
        } catch (const DBException& ex) {
            if (ex.code() == ErrorCodes::InterruptedDueToReplStateChange &&
                !_isDisabled.loadRelaxed()) {
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
    }
}

void ReplicatedFastCountManager::_runIteration(OperationContext* opCtx) {
    const absl::flat_hash_map<UUID, StoredSizeCount> dirtyMetadata = _getSnapshotOfDirtyMetadata();

    if (MONGO_unlikely(hangAfterReplicatedFastCountSnapshot.shouldFail())) {
        hangAfterReplicatedFastCountSnapshot.pauseWhileSet();
    }

    auto acquisition = _acquireFastCountCollection(opCtx);
    uassert(ErrorCodes::NamespaceNotFound, "Expected fastcount collection to exist", acquisition);

    const CollectionPtr& fastCountColl = acquisition->getCollectionPtr();
    invariant(fastCountColl,
              str::stream()
                  << "Expected to acquire fastcount store as a collection, not a view. isView : "
                  << acquisition->isView());

    try {
        _flush(opCtx, fastCountColl, dirtyMetadata);
    } catch (const DBException& ex) {
        LOGV2_WARNING(7397500,
                      "Failed to persist collection sizeCount metadata",
                      "error"_attr = ex.toStatus());
    }
}

void ReplicatedFastCountManager::_writeOneMetadata(OperationContext* opCtx,
                                                   const CollectionPtr& fastCountColl,
                                                   const UUID& uuid,
                                                   const CollectionSizeCount& sizeCount,
                                                   const RecordId recordId) {
    WriteUnitOfWork wuow(opCtx);
    Snapshotted<BSONObj> doc;
    bool exists = fastCountColl->findDoc(opCtx, recordId, &doc);

    if (exists) {
        _updateOneMetadata(opCtx, fastCountColl, doc, uuid, recordId, sizeCount);
    } else {
        _insertOneMetadata(opCtx, fastCountColl, uuid, sizeCount);
    }

    wuow.commit();
}

void ReplicatedFastCountManager::_updateOneMetadata(OperationContext* opCtx,
                                                    const CollectionPtr& fastCountColl,
                                                    const Snapshotted<BSONObj>& doc,
                                                    const UUID& uuid,
                                                    const RecordId recordId,
                                                    const CollectionSizeCount& sizeCount) {
    // TODO SERVER-117886: Manually performing update without query system. This would be nice to
    // avoid extra dependencies but might be too tricky to get right.
    CollectionUpdateArgs args(doc.value());
    // TODO SERVER-117654: When we also store timestamp we should be able to recover/combine data
    // from old doc to keep this accurate.
    BSONObj newDoc = _getDocForWrite(uuid, sizeCount);

    auto diff = doc_diff::computeOplogDiff(doc.value(), newDoc, /*padding=*/0);

    if (diff) {
        args.update = update_oplog_entry::makeDeltaOplogEntry(*diff);
        args.criteria = BSON("_id" << uuid);
        collection_internal::updateDocument(
            opCtx, fastCountColl, recordId, doc, newDoc, &args.update, nullptr, nullptr, &args);
    } else {
        // TODO SERVER-117508: Increment t2 stat.
        LOGV2(11648805, "ReplicatedFastCountManager empty update", "uuid"_attr = uuid);
    }
}

void ReplicatedFastCountManager::_insertOneMetadata(OperationContext* opCtx,
                                                    const CollectionPtr& fastCountColl,
                                                    const UUID& uuid,
                                                    const CollectionSizeCount& sizeCount) {
    // TODO SERVER-118529: Consider error handling more carefully here.
    uassertStatusOK(
        collection_internal::insertDocument(opCtx,
                                            fastCountColl,
                                            InsertStatement(_getDocForWrite(uuid, sizeCount)),
                                            /*opDebug=*/nullptr));
}

boost::optional<CollectionOrViewAcquisition>
ReplicatedFastCountManager::_acquireFastCountCollection(OperationContext* opCtx) {
    {
        CollectionOrViewAcquisition acquisition =
            acquireCollectionOrView(opCtx,
                                    CollectionOrViewAcquisitionRequest::fromOpCtx(
                                        opCtx,
                                        NamespaceString::makeGlobalConfigCollection(
                                            NamespaceString::kSystemReplicatedFastCountStore),
                                        AcquisitionPrerequisites::OperationType::kWrite),
                                    LockMode::MODE_IX);

        if (acquisition.getCollectionPtr()) {
            return acquisition;
        }
    }

    uasserted(11718600, "Expected fastcount collection to exist");
}

BSONObj ReplicatedFastCountManager::_getDocForWrite(const UUID& uuid,
                                                    const CollectionSizeCount& sizeCount) const {
    return BSON("_id" << uuid << kCountKey << sizeCount.count << kSizeKey << sizeCount.size);
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

