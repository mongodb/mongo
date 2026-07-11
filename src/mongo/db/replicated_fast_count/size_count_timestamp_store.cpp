// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/replicated_fast_count/size_count_timestamp_store.h"

#include "mongo/db/collection_crud/collection_write_path.h"
#include "mongo/db/collection_crud/container_write.h"
#include "mongo/db/record_id_helpers.h"
#include "mongo/db/replicated_fast_count/size_count_store.h"
#include "mongo/db/shard_role/lock_manager/d_concurrency.h"
#include "mongo/db/shard_role/shard_catalog/clustered_collection_util.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/update/document_diff_calculator.h"
#include "mongo/db/update/update_oplog_entry_serialization.h"

namespace mongo::replicated_fast_count {
namespace {

// Used for the container implementation.
constexpr int64_t kTimestampContainerKey = 0;
// Used for the collection implementation.
constexpr int32_t kTimestampDocId = 0;

IntegerKeyedContainer& getIntegerKeyedContainer(RecordStore& recordStore) {
    auto container = recordStore.getContainer();
    massert(
        12566000,
        "Expected replicated fast count timestamp record store to hold an IntegerKeyedContainer",
        std::holds_alternative<std::reference_wrapper<IntegerKeyedContainer>>(container));
    return std::get<std::reference_wrapper<IntegerKeyedContainer>>(container);
}

boost::optional<CollectionOrViewAcquisition> acquireTimestampCollectionForRead(
    OperationContext* opCtx) {
    CollectionOrViewAcquisition acquisition = acquireCollectionOrViewMaybeLockFree(
        opCtx,
        CollectionOrViewAcquisitionRequest::fromOpCtx(
            opCtx,
            NamespaceString::makeGlobalConfigCollection(
                NamespaceString::kReplicatedFastCountStoreTimestamps),
            AcquisitionPrerequisites::OperationType::kRead));

    if (acquisition.getCollectionPtr()) {
        return acquisition;
    }

    return boost::none;
}

boost::optional<CollectionOrViewAcquisition> acquireTimestampCollectionForWrite(
    OperationContext* opCtx) {
    CollectionOrViewAcquisition acquisition =
        acquireCollectionOrView(opCtx,
                                CollectionOrViewAcquisitionRequest::fromOpCtx(
                                    opCtx,
                                    NamespaceString::makeGlobalConfigCollection(
                                        NamespaceString::kReplicatedFastCountStoreTimestamps),
                                    AcquisitionPrerequisites::OperationType::kWrite),
                                LockMode::MODE_IX);

    if (acquisition.getCollectionPtr()) {
        return acquisition;
    }

    return boost::none;
}

void assertInWriteUnitOfWorkAndLocked(OperationContext* opCtx) {
    massert(12280400,
            "SizeCountTimestampStore::write() must be called within a WriteUnitOfWork",
            shard_role_details::getLocker(opCtx)->inAWriteUnitOfWork());
    massert(
        12915201,
        "Must hold the GlobalLock in a write mode when calling SizeCountTimestampStore::write()",
        shard_role_details::getLocker(opCtx)->isWriteLocked());
}
}  // namespace

boost::optional<Timestamp> CollectionSizeCountTimestampStore::read(OperationContext* opCtx) const {
    massert(12915206,
            "Must hold the GlobalLock in a read mode when calling SizeCountTimestampStore::read()",
            shard_role_details::getLocker(opCtx)->isReadLocked());

    const auto acquisition = acquireTimestampCollectionForRead(opCtx);
    if (!acquisition.has_value()) {
        return boost::none;
    }

    const CollectionPtr& coll = acquisition->getCollectionPtr();
    const RecordId rid =
        record_id_helpers::keyForDoc(BSON("_id" << kTimestampDocId),
                                     clustered_util::makeDefaultClusteredIdIndex().getIndexSpec(),
                                     /*collator=*/nullptr)
            .getValue();
    Snapshotted<BSONObj> document;
    if (!coll->findDoc(opCtx, rid, &document)) {
        return boost::none;
    }

    const BSONObj& data = document.value();
    return data.getField(kValidAsOfKey).timestamp();
}

void CollectionSizeCountTimestampStore::write(OperationContext* opCtx, Timestamp timestamp) {
    assertInWriteUnitOfWorkAndLocked(opCtx);

    const auto acquisition = acquireTimestampCollectionForWrite(opCtx).value();
    const CollectionPtr& coll = acquisition.getCollectionPtr();
    const RecordId rid =
        record_id_helpers::keyForDoc(BSON("_id" << kTimestampDocId),
                                     clustered_util::makeDefaultClusteredIdIndex().getIndexSpec(),
                                     /*collator=*/nullptr)
            .getValue();
    const BSONObj newDoc = BSON("_id" << kTimestampDocId << kValidAsOfKey << timestamp);

    Snapshotted<BSONObj> existingDoc;
    if (coll->findDoc(opCtx, rid, &existingDoc)) {
        const auto diff = doc_diff::computeOplogDiff(existingDoc.value(), newDoc, /*padding=*/0);
        invariant(diff.has_value(),
                  fmt::format("Expected computed diff to be smaller than the post-image: "
                              "pre={}, post={}",
                              existingDoc.value().toString(),
                              newDoc.toString()));
        if (!diff->isEmpty()) {
            CollectionUpdateArgs args(existingDoc.value());
            args.update = update_oplog_entry::makeDeltaOplogEntry(*diff);
            args.criteria = BSON("_id" << kTimestampDocId);
            collection_internal::updateDocument(
                opCtx, coll, rid, existingDoc, newDoc, &args.update, nullptr, nullptr, &args);
        }
    } else {
        massertStatusOK(collection_internal::insertDocument(
            opCtx, coll, InsertStatement(newDoc), /*opDebug=*/nullptr));
    }
}

boost::optional<Timestamp> ContainerSizeCountTimestampStore::read(OperationContext* opCtx) const {
    massert(12915200,
            "Must hold the GlobalLock in a read mode when calling SizeCountTimestampStore::read()",
            shard_role_details::getLocker(opCtx)->isReadLocked());

    auto& ru = *shard_role_details::getRecoveryUnit(opCtx);
    auto& container = getIntegerKeyedContainer(*_recordStore);
    auto cursor = container.getCursor(ru);
    auto result = cursor->find(kTimestampContainerKey);
    if (!result) {
        return boost::none;
    }
    BSONObj data(result->data());
    return data.getField(kValidAsOfKey).timestamp();
}

void ContainerSizeCountTimestampStore::write(OperationContext* opCtx, Timestamp timestamp) {
    assertInWriteUnitOfWorkAndLocked(opCtx);

    auto& ru = *shard_role_details::getRecoveryUnit(opCtx);
    auto& container = getIntegerKeyedContainer(*_recordStore);
    auto val = BSON(kValidAsOfKey << timestamp);
    std::span<const char> valSpan{val.objdata(), static_cast<size_t>(val.objsize())};

    // Check if the key exists. Containers currently only support strict inserts or strict updates.
    auto cursor = container.getCursor(ru);
    if (cursor->find(kTimestampContainerKey)) {
        massertStatusOK(
            container_write::update(opCtx, ru, container, kTimestampContainerKey, valSpan));
    } else {
        massertStatusOK(
            container_write::insert(opCtx, ru, container, kTimestampContainerKey, valSpan));
    }
}

RecordStore* ContainerSizeCountTimestampStore::rs_ForTest() const {
    return _recordStore.get();
}
}  // namespace mongo::replicated_fast_count
