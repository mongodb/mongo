// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/replicated_fast_count/size_count_store.h"

#include "mongo/db/collection_crud/container_write.h"
#include "mongo/db/shard_role/transaction_resources.h"

#include <string_view>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo::replicated_fast_count {
namespace {

BSONObj entryToContainerValue(const SizeCountStore::Entry& entry) {
    BSONObjBuilder builder;
    builder.append(kCountKey, entry.count);
    builder.append(kSizeKey, entry.size);
    if (entry.hash) {
        builder.append(kHashKey, *entry.hash);
    }
    const BSONObj metadata = builder.obj();
    return BSON(kValidAsOfKey << entry.timestamp << kMetadataKey << metadata);
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

SizeCountStore::Entry SizeCountStore::parseContainerValue(std::span<const char> value) {
    return parseEntry(BSONObj(value.data()));
}

std::span<const char> SizeCountStore::uuidToContainerKey(const UUID& uuid) {
    auto cdr = uuid.toCDR();
    return {reinterpret_cast<const char*>(cdr.data()), cdr.length()};
}

RecordStore* SizeCountStore::rs_ForTest() const {
    return _recordStore.get();
}

StringKeyedContainer& SizeCountStore::_getStringKeyedContainer() const {
    auto container = _recordStore->getContainer();
    massert(12566002,
            "Expected replicated fast count metadata record store to hold a StringKeyedContainer",
            std::holds_alternative<std::reference_wrapper<StringKeyedContainer>>(container));
    return std::get<std::reference_wrapper<StringKeyedContainer>>(container);
}

boost::optional<SizeCountStore::Entry> SizeCountStore::read(OperationContext* opCtx,
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

void SizeCountStore::write(OperationContext* opCtx, UUID uuid, const Entry& entry) {
    assertInWriteUnitOfWorkAndLocked(opCtx, "write");

    auto& ru = *shard_role_details::getRecoveryUnit(opCtx);
    auto& container = _getStringKeyedContainer();
    auto keySpan = uuidToContainerKey(uuid);

    auto cursor = container.getCursor(ru);
    const auto previousValue = cursor->find(keySpan);

    const auto hashToWrite = [&]() -> boost::optional<int64_t> {
        // Check if a previous entry exists and contains a hash.
        if (previousValue.has_value()) {
            const SizeCountStore::Entry previousEntry =
                SizeCountStore::parseContainerValue(*previousValue);
            if (!previousEntry.hash.has_value()) {
                // The pre-existing entry does not already contain a hash. Do not write a hash,
                // regardless of the hash in `entry`. This accounts for collections with size/count
                // metadata created before hash validation was enabled.
                return boost::none;
            }
        }
        return entry.hash;
    }();

    const BSONObj value = entryToContainerValue(SizeCountStore::Entry{.timestamp = entry.timestamp,
                                                                      .size = entry.size,
                                                                      .count = entry.count,
                                                                      .hash = hashToWrite});
    const std::span<const char> valueSpan = bsonToSpan(value);

    // Check if the key exists. Containers currently only support strict inserts or strict updates.
    if (previousValue.has_value()) {
        massertStatusOK(container_write::update(opCtx, ru, container, keySpan, valueSpan));
    } else {
        massertStatusOK(container_write::insert(opCtx, ru, container, keySpan, valueSpan));
    }
}

void SizeCountStore::writeToTable(OperationContext* opCtx, UUID uuid, const Entry& entry) {
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

void SizeCountStore::insert(OperationContext* opCtx, UUID uuid, const Entry& entry) {
    assertInWriteUnitOfWorkAndLocked(opCtx, "insert");

    auto& ru = *shard_role_details::getRecoveryUnit(opCtx);
    auto& container = _getStringKeyedContainer();
    auto val = entryToContainerValue(entry);
    massertStatusOK(
        container_write::insert(opCtx, ru, container, uuidToContainerKey(uuid), bsonToSpan(val)));
}

size_t SizeCountStore::remove(OperationContext* opCtx, UUID uuid) {
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

void SizeCountStore::readAndIncrementReplicatedMetadata(OperationContext* opCtx,
                                                        ReplicatedMetadataDeltas& deltas) const {
    massert(12915202,
            "Must hold the GlobalLock in a read mode when calling "
            "SizeCountStore::readAndIncrementReplicatedMetadata()",
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
        delta.metadata.sizeCount.count += entry.count;
        delta.metadata.sizeCount.size += entry.size;
        // If either hash is boost::none, delta.metadata.hash is set to boost::none.
        delta.metadata.hash = combineValidationHashes(delta.metadata.hash, entry.hash);
    }
}

}  // namespace mongo::replicated_fast_count
