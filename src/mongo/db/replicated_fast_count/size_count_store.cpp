// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/replicated_fast_count/size_count_store.h"

#include "mongo/db/collection_crud/collection_write_path.h"
#include "mongo/db/collection_crud/container_write.h"
#include "mongo/db/record_id_helpers.h"
#include "mongo/db/shard_role/lock_manager/d_concurrency.h"
#include "mongo/db/shard_role/shard_catalog/clustered_collection_util.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/update/document_diff_calculator.h"
#include "mongo/db/update/update_oplog_entry_serialization.h"

#include <string_view>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo::replicated_fast_count {
namespace {

BSONObj entryToContainerValue(const SizeCountStore::Entry& entry) {
    return BSON(kValidAsOfKey << entry.timestamp << kMetadataKey
                              << BSON(kCountKey << entry.count << kSizeKey << entry.size));
}

std::span<const char> bsonToSpan(const BSONObj& obj) {
    return {obj.objdata(), static_cast<size_t>(obj.objsize())};
}

void assertInWriteUnitOfWorkAndLocked(OperationContext* opCtx, std::string_view op) {
    massert(12915205,
            fmt::format("SizeCountStore::{}() must be called within a WriteUnitOfWork", op),
            shard_role_details::getLocker(opCtx)->inAWriteUnitOfWork());
    massert(12915204,
            fmt::format(
                "Must hold the GlobalLock in a write mode when calling SizeCountStore::{}()", op),
            shard_role_details::getLocker(opCtx)->isWriteLocked());
}

// Extracts the 64-bit collection hash from a persisted `meta` subdocument. Returns boost::none if
// the subdocument does not carry the field.
boost::optional<int64_t> parseHash(const BSONObj& meta) {
    const BSONElement hashElem = meta.getField(kHashKey);
    if (hashElem.eoo()) {
        return boost::none;
    }
    tassert(13197400,
            "hash must be stored as a 64-bit integer",
            hashElem.type() == BSONType::numberLong);
    return hashElem.Long();
}

// Decodes a persisted metadata document into an Entry, extracting the `meta` subdocument once.
SizeCountStore::Entry parseEntry(const BSONObj& data) {
    const BSONObj meta = data.getField(kMetadataKey).Obj();
    return SizeCountStore::Entry{.timestamp = data.getField(kValidAsOfKey).timestamp(),
                                 .size = meta.getField(kSizeKey).Long(),
                                 .count = meta.getField(kCountKey).Long(),
                                 .hash = parseHash(meta)};
}
}  // namespace

boost::optional<CollectionOrViewAcquisition> acquireFastCountCollectionForRead(
    OperationContext* opCtx) {
    CollectionOrViewAcquisition acquisition = acquireCollectionOrViewMaybeLockFree(
        opCtx,
        CollectionOrViewAcquisitionRequest::fromOpCtx(
            opCtx,
            NamespaceString::makeGlobalConfigCollection(NamespaceString::kReplicatedFastCountStore),
            AcquisitionPrerequisites::OperationType::kRead));

    if (acquisition.getCollectionPtr()) {
        return acquisition;
    }

    return boost::none;
}

boost::optional<CollectionOrViewAcquisition> acquireFastCountCollectionForWrite(
    OperationContext* opCtx) {
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

SizeCountStore::Entry SizeCountStore::parseContainerValue(std::span<const char> value) {
    return parseEntry(BSONObj(value.data()));
}

boost::optional<SizeCountStore::Entry> CollectionSizeCountStore::read(OperationContext* opCtx,
                                                                      UUID uuid) const {
    massert(12915208,
            "Must hold the GlobalLock in a read mode when calling SizeCountStore::read()",
            shard_role_details::getLocker(opCtx)->isReadLocked());

    const auto acquisition = acquireFastCountCollectionForRead(opCtx);
    if (!acquisition.has_value()) {
        // TODO(SERVER-123051): Revisit this.
        return boost::none;
    }

    const CollectionPtr& coll = acquisition->getCollectionPtr();
    const RecordId rid =
        record_id_helpers::keyForDoc(BSON("_id" << uuid),
                                     clustered_util::makeDefaultClusteredIdIndex().getIndexSpec(),
                                     /*collator=*/nullptr)
            .getValue();
    Snapshotted<BSONObj> document;
    if (!coll->findDoc(opCtx, rid, &document)) {
        return boost::none;
    }

    return parseEntry(document.value());
}

void CollectionSizeCountStore::write(OperationContext* opCtx, UUID uuid, const Entry& entry) {
    assertInWriteUnitOfWorkAndLocked(opCtx, "write");

    const auto acquisition = acquireFastCountCollectionForWrite(opCtx).value();
    const CollectionPtr& coll = acquisition.getCollectionPtr();
    const RecordId rid =
        record_id_helpers::keyForDoc(BSON("_id" << uuid),
                                     clustered_util::makeDefaultClusteredIdIndex().getIndexSpec(),
                                     /*collator=*/nullptr)
            .getValue();

    const BSONObj newDoc = BSON("_id" << uuid << kValidAsOfKey << entry.timestamp << kMetadataKey
                                      << BSON(kCountKey << entry.count << kSizeKey << entry.size));

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
            args.criteria = BSON("_id" << uuid);
            collection_internal::updateDocument(
                opCtx, coll, rid, existingDoc, newDoc, &args.update, nullptr, nullptr, &args);
        }
    } else {
        massertStatusOK(collection_internal::insertDocument(
            opCtx, coll, InsertStatement(newDoc), /*opDebug=*/nullptr));
    }
}

void CollectionSizeCountStore::writeToTable(OperationContext* opCtx,
                                            UUID uuid,
                                            const Entry& entry) {
    MONGO_UNREACHABLE_TASSERT(12549704);
}

void CollectionSizeCountStore::insert(OperationContext* opCtx, UUID uuid, const Entry& entry) {
    assertInWriteUnitOfWorkAndLocked(opCtx, "insert");

    const auto acquisition = acquireFastCountCollectionForWrite(opCtx).value();
    const CollectionPtr& coll = acquisition.getCollectionPtr();

    const BSONObj newDoc = BSON("_id" << uuid << kValidAsOfKey << entry.timestamp << kMetadataKey
                                      << BSON(kCountKey << entry.count << kSizeKey << entry.size));

    massertStatusOK(collection_internal::insertDocument(
        opCtx, coll, InsertStatement(newDoc), /*opDebug=*/nullptr));
}

size_t CollectionSizeCountStore::remove(OperationContext* opCtx, UUID uuid) {
    assertInWriteUnitOfWorkAndLocked(opCtx, "remove");

    const auto acquisition = acquireFastCountCollectionForWrite(opCtx).value();
    const RecordId rid =
        record_id_helpers::keyForDoc(BSON("_id" << uuid),
                                     clustered_util::makeDefaultClusteredIdIndex().getIndexSpec(),
                                     /*collator=*/nullptr)
            .getValue();

    Snapshotted<BSONObj> docToDelete;

    if (!acquisition.getCollectionPtr()->findDoc(opCtx, rid, &docToDelete)) {
        LOGV2_WARNING(12054101,
                      "Attempted to delete an entry for uuid {uuid} from the fast count store, but "
                      "no such entry exists.",
                      "uuid"_attr = uuid.toString());
        return 0;
    }

    collection_internal::deleteDocument(opCtx,
                                        acquisition.getCollectionPtr(),
                                        docToDelete,
                                        kUninitializedStmtId,
                                        rid,
                                        /*opDebug=*/nullptr);
    return 1;
}

void CollectionSizeCountStore::readAndIncrementSizeCounts(OperationContext* opCtx,
                                                          SizeCountDeltas& deltas) const {
    massert(12915207,
            "Must hold the GlobalLock in a read mode when calling "
            "SizeCountStore::readAndIncrementSizeCounts()",
            shard_role_details::getLocker(opCtx)->isReadLocked());

    const auto acquisition = acquireFastCountCollectionForRead(opCtx).value();
    const CollectionPtr& coll = acquisition.getCollectionPtr();

    for (auto& [uuid, delta] : deltas) {
        if (delta.state != DDLState::kNone) {
            continue;
        }
        const RecordId rid = record_id_helpers::keyForDoc(
                                 BSON("_id" << uuid),
                                 clustered_util::makeDefaultClusteredIdIndex().getIndexSpec(),
                                 /*collator=*/nullptr)
                                 .getValue();
        Snapshotted<BSONObj> doc;
        if (coll->findDoc(opCtx, rid, &doc)) {
            const Entry entry = parseEntry(doc.value());
            delta.sizeCount.count += entry.count;
            delta.sizeCount.size += entry.size;
        }
    }
}

std::span<const char> ContainerSizeCountStore::uuidToContainerKey(const UUID& uuid) {
    auto cdr = uuid.toCDR();
    return {reinterpret_cast<const char*>(cdr.data()), cdr.length()};
}

RecordStore* ContainerSizeCountStore::rs_ForTest() const {
    return _recordStore.get();
}

StringKeyedContainer& ContainerSizeCountStore::_getStringKeyedContainer() const {
    auto container = _recordStore->getContainer();
    massert(12566002,
            "Expected replicated fast count metadata record store to hold a StringKeyedContainer",
            std::holds_alternative<std::reference_wrapper<StringKeyedContainer>>(container));
    return std::get<std::reference_wrapper<StringKeyedContainer>>(container);
}

boost::optional<SizeCountStore::Entry> ContainerSizeCountStore::read(OperationContext* opCtx,
                                                                     UUID uuid) const {
    massert(12915203,
            "Must hold the GlobalLock in a read mode when calling SizeCountStore::read()",
            shard_role_details::getLocker(opCtx)->isReadLocked());

    auto& ru = *shard_role_details::getRecoveryUnit(opCtx);
    auto& container = _getStringKeyedContainer();
    auto cursor = container.getCursor(ru);
    auto result = cursor->find(uuidToContainerKey(uuid));
    if (!result) {
        return boost::none;
    }
    return SizeCountStore::parseContainerValue(*result);
}

void ContainerSizeCountStore::write(OperationContext* opCtx, UUID uuid, const Entry& entry) {
    assertInWriteUnitOfWorkAndLocked(opCtx, "write");

    auto& ru = *shard_role_details::getRecoveryUnit(opCtx);
    auto& container = _getStringKeyedContainer();
    auto val = entryToContainerValue(entry);
    auto keySpan = uuidToContainerKey(uuid);
    auto valSpan = bsonToSpan(val);

    // Check if the key exists. Containers currently only support strict inserts or strict updates.
    auto cursor = container.getCursor(ru);
    if (cursor->find(keySpan)) {
        massertStatusOK(container_write::update(opCtx, ru, container, keySpan, valSpan));
    } else {
        massertStatusOK(container_write::insert(opCtx, ru, container, keySpan, valSpan));
    }
}

void ContainerSizeCountStore::writeToTable(OperationContext* opCtx, UUID uuid, const Entry& entry) {
    auto& ru = *shard_role_details::getRecoveryUnit(opCtx);
    auto& container = _getStringKeyedContainer();
    auto val = entryToContainerValue(entry);
    auto keySpan = uuidToContainerKey(uuid);
    auto valSpan = bsonToSpan(val);

    // Bypass container_write::insert/update — those check canAcceptWritesFor which fails on a
    // secondary in INITIAL_SYNC state. Write directly to the container and skip op observers
    // since these writes are not user-visible operations. This write is unreplicated so we can
    // safely use container::ExistingKeyPolicy::Overwrite.
    massertStatusOK(
        container.insert(ru, keySpan, valSpan, container::ExistingKeyPolicy::overwrite));
}

void ContainerSizeCountStore::insert(OperationContext* opCtx, UUID uuid, const Entry& entry) {
    assertInWriteUnitOfWorkAndLocked(opCtx, "insert");

    auto& ru = *shard_role_details::getRecoveryUnit(opCtx);
    auto& container = _getStringKeyedContainer();
    auto val = entryToContainerValue(entry);
    massertStatusOK(
        container_write::insert(opCtx, ru, container, uuidToContainerKey(uuid), bsonToSpan(val)));
}

size_t ContainerSizeCountStore::remove(OperationContext* opCtx, UUID uuid) {
    assertInWriteUnitOfWorkAndLocked(opCtx, "remove");

    auto& ru = *shard_role_details::getRecoveryUnit(opCtx);
    auto& container = _getStringKeyedContainer();
    auto status = container_write::remove(opCtx, ru, container, uuidToContainerKey(uuid));
    if (!status.isOK()) {
        LOGV2_WARNING(12566001,
                      "Attempted to delete an entry for uuid {uuid} from the fast count "
                      "container, but the operation failed.",
                      "uuid"_attr = uuid.toString(),
                      "error"_attr = status);
        return 0;
    }
    return 1;
}

void ContainerSizeCountStore::readAndIncrementSizeCounts(OperationContext* opCtx,
                                                         SizeCountDeltas& deltas) const {
    massert(12915202,
            "Must hold the GlobalLock in a read mode when calling "
            "SizeCountStore::readAndIncrementSizeCounts()",
            shard_role_details::getLocker(opCtx)->isReadLocked());

    auto& ru = *shard_role_details::getRecoveryUnit(opCtx);
    auto& container = _getStringKeyedContainer();
    auto cursor = container.getCursor(ru);
    for (auto& [uuid, delta] : deltas) {
        if (delta.state != DDLState::kNone) {
            continue;
        }
        auto result = cursor->find(uuidToContainerKey(uuid));
        if (!result) {
            continue;
        }
        auto entry = parseContainerValue(*result);
        delta.sizeCount.count += entry.count;
        delta.sizeCount.size += entry.size;
    }
}

}  // namespace mongo::replicated_fast_count
