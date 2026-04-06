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

#include "mongo/db/replicated_fast_count/replicated_fast_count_op_observer.h"

#include "mongo/db/collection_crud/collection_write_path.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_delta_utils.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_enabled.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo::replicated_fast_count {

namespace {

bool shouldWriteToFastCountStore(OperationContext* opCtx, const NamespaceString& nss) {
    if (!isReplicatedFastCountEnabled(opCtx) || !isReplicatedFastCountEligible(nss)) {
        return false;
    }

    // Don't attempt to write to the internal fast count collections when creating them.
    if (isInternalFastCountNss(nss)) {
        return false;
    }

    // Don't write to the fast count store if we are a standalone node.
    if (!repl::ReplicationCoordinator::get(opCtx)->getSettings().isReplSet()) {
        return false;
    }

    // Don't write during initial sync. Collection creation observers fire as part of cloning each
    // collection, but config.fast_count_metadata_store itself is also cloned directly from the
    // sync source, so writing here would produce duplicate-key errors.
    if (repl::ReplicationCoordinator::get(opCtx)->isInInitialSyncOrRollback()) {
        return false;
    }

    return true;
}
}  // namespace

void ReplicatedFastCountOpObserver::onCreateCollection(
    OperationContext* opCtx,
    const NamespaceString& collectionName,
    const CollectionOptions& options,
    const BSONObj&,
    const OplogSlot&,
    const boost::optional<CreateCollCatalogIdentifier>&,
    bool,
    bool,
    bool) {

    if (!shouldWriteToFastCountStore(opCtx, collectionName)) {
        return;
    }

    massert(
        12054100,
        "Received collection options without uuid in replicated fast count onCreateCollection hook",
        options.uuid);
    const UUID uuid = *options.uuid;

    // We use AllowLockAcquisitionOnTimestampedUnitOfWork to allow us to acquire the lock on the
    // fast count store collection within the same write unit of work that has reserved a timestamp
    // for the collection creation.
    //
    // This acquisition can block on any stronger acquisition on the fast count collection. This
    // could cause a deadlock if an operation performing that acquisition were waiting on oplog
    // visibility to advance past this create operation, since collection creation would wait to
    // acquire the lock on the fast count collection while the operation holding the lock on the
    // fast count collection would wait for our operation to commit and become visible. However,
    // there are no operations that acquire the fast count collection and await oplog visibility, so
    // this should be safe.
    AllowLockAcquisitionOnTimestampedUnitOfWork allowLockAcquisition(
        shard_role_details::getLocker(opCtx));

    // We want to write to the fast count collection when encountering a collection creation without
    // generating a separate oplog entry. We cannot group this write with the create operation into
    // the same applyOps entry because DDL operations require their own entry.
    repl::UnreplicatedWritesBlock uwb(opCtx);

    auto acquisition = replicated_fast_count::acquireFastCountCollectionForWrite(opCtx);

    if (!acquisition) {
        // TODO SERVER-123159: Define limited set of collections that we expect this to be true for.
        LOGV2(12054102,
              "Did not find fast count store when creating collection",
              "ns"_attr = collectionName.toStringForErrorMsg(),
              "uuid"_attr = uuid);
        return;
    }

    const int64_t size = 0;
    const int64_t count = 0;
    const Timestamp validAsOf = Timestamp(0, 0);

    const BSONObj initialEntry =
        BSON("_id" << uuid << replicated_fast_count::kValidAsOfKey << validAsOf
                   << replicated_fast_count::kMetadataKey
                   << BSON(replicated_fast_count::kCountKey
                           << count << replicated_fast_count::kSizeKey << size));

    massertStatusOK(collection_internal::insertDocument(opCtx,
                                                        acquisition->getCollectionPtr(),
                                                        InsertStatement(initialEntry),
                                                        /*opDebug=*/nullptr));
}

repl::OpTime ReplicatedFastCountOpObserver::onDropCollection(OperationContext* opCtx,
                                                             const NamespaceString& collectionName,
                                                             const UUID& uuid,
                                                             std::uint64_t,
                                                             bool,
                                                             bool) {

    if (!shouldWriteToFastCountStore(opCtx, collectionName)) {
        return {};
    }

    // See explanations in onCreateCollection.
    AllowLockAcquisitionOnTimestampedUnitOfWork allowLockAcquisition(
        shard_role_details::getLocker(opCtx));
    repl::UnreplicatedWritesBlock uwb(opCtx);

    auto acquisition = replicated_fast_count::acquireFastCountCollectionForWrite(opCtx);

    if (!acquisition) {
        // TODO SERVER-123159: Define limited set of collections that we expect this to be true for.
        LOGV2(12054103,
              "Did not find fast count store when dropping collection",
              "ns"_attr = collectionName.toStringForErrorMsg(),
              "uuid"_attr = uuid);
        return {};
    }

    const RecordId recordId = replicated_fast_count::getFastCountStoreKey(uuid);
    Snapshotted<BSONObj> doc;
    massert(12054101,
            "Tracked collection being dropped does not have fast count store entry",
            acquisition->getCollectionPtr()->findDoc(opCtx, recordId, &doc));

    collection_internal::deleteDocument(opCtx,
                                        acquisition->getCollectionPtr(),
                                        kUninitializedStmtId,
                                        recordId,
                                        /*opDebug=*/nullptr);

    return {};
}

}  // namespace mongo::replicated_fast_count
