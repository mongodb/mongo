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

#include "mongo/db/replicated_fast_count/size_count_store.h"

#include "mongo/db/collection_crud/collection_write_path.h"
#include "mongo/db/collection_crud/container_write.h"
#include "mongo/db/record_id_helpers.h"
#include "mongo/db/shard_role/lock_manager/d_concurrency.h"
#include "mongo/db/shard_role/shard_catalog/clustered_collection_util.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/update/document_diff_calculator.h"
#include "mongo/db/update/update_oplog_entry_serialization.h"

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
    BSONObj data(value.data());
    return Entry{.timestamp = data.getField(kValidAsOfKey).timestamp(),
                 .size = data.getField(kMetadataKey).Obj().getField(kSizeKey).Long(),
                 .count = data.getField(kMetadataKey).Obj().getField(kCountKey).Long()};
}

boost::optional<SizeCountStore::Entry> CollectionSizeCountStore::read(OperationContext* opCtx,
                                                                      UUID uuid) const {
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

    const BSONObj& data = document.value();
    return SizeCountStore::Entry{
        .timestamp = data.getField(kValidAsOfKey).timestamp(),
        .size = data.getField(kMetadataKey).Obj().getField(kSizeKey).Long(),
        .count = data.getField(kMetadataKey).Obj().getField(kCountKey).Long()};
}

void CollectionSizeCountStore::write(OperationContext* opCtx, UUID uuid, const Entry& entry) {
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

void CollectionSizeCountStore::insert(OperationContext* opCtx, UUID uuid, const Entry& entry) {
    const auto acquisition = acquireFastCountCollectionForWrite(opCtx).value();
    const CollectionPtr& coll = acquisition.getCollectionPtr();

    const BSONObj newDoc = BSON("_id" << uuid << kValidAsOfKey << entry.timestamp << kMetadataKey
                                      << BSON(kCountKey << entry.count << kSizeKey << entry.size));

    massertStatusOK(collection_internal::insertDocument(
        opCtx, coll, InsertStatement(newDoc), /*opDebug=*/nullptr));
}

void CollectionSizeCountStore::remove(OperationContext* opCtx, UUID uuid) {
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
        return;
    }

    collection_internal::deleteDocument(opCtx,
                                        acquisition.getCollectionPtr(),
                                        docToDelete,
                                        kUninitializedStmtId,
                                        rid,
                                        /*opDebug=*/nullptr);
}

std::span<const char> ContainerSizeCountStore::uuidToContainerKey(const UUID& uuid) {
    auto cdr = uuid.toCDR();
    return {reinterpret_cast<const char*>(cdr.data()), cdr.length()};
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
        massertStatusOK(container_write::insert(
            opCtx, ru, container, keySpan, valSpan, container::ExistingKeyPolicy::reject));
    }
}

void ContainerSizeCountStore::insert(OperationContext* opCtx, UUID uuid, const Entry& entry) {
    auto& ru = *shard_role_details::getRecoveryUnit(opCtx);
    auto& container = _getStringKeyedContainer();
    auto val = entryToContainerValue(entry);
    massertStatusOK(container_write::insert(opCtx,
                                            ru,
                                            container,
                                            uuidToContainerKey(uuid),
                                            bsonToSpan(val),
                                            container::ExistingKeyPolicy::reject));
}

void ContainerSizeCountStore::remove(OperationContext* opCtx, UUID uuid) {
    auto& ru = *shard_role_details::getRecoveryUnit(opCtx);
    auto& container = _getStringKeyedContainer();
    auto status = container_write::remove(opCtx, ru, container, uuidToContainerKey(uuid));
    if (!status.isOK()) {
        LOGV2_WARNING(12566001,
                      "Attempted to delete an entry for uuid {uuid} from the fast count "
                      "container, but the operation failed.",
                      "uuid"_attr = uuid.toString(),
                      "error"_attr = status);
    }
}

}  // namespace mongo::replicated_fast_count
