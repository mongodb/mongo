// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/replicated_fast_count/size_count_timestamp_store.h"

#include "mongo/db/collection_crud/container_write.h"
#include "mongo/db/replicated_fast_count/size_count_store.h"
#include "mongo/db/shard_role/transaction_resources.h"

namespace mongo::replicated_fast_count {
namespace {

constexpr int64_t kTimestampContainerKey = 0;

IntegerKeyedContainer& getIntegerKeyedContainer(RecordStore& recordStore) {
    auto container = recordStore.getContainer();
    massert(
        12566000,
        "Expected replicated fast count timestamp record store to hold an IntegerKeyedContainer",
        std::holds_alternative<std::reference_wrapper<IntegerKeyedContainer>>(container));
    return std::get<std::reference_wrapper<IntegerKeyedContainer>>(container);
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

boost::optional<Timestamp> SizeCountTimestampStore::read(OperationContext* opCtx) const {
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

void SizeCountTimestampStore::write(OperationContext* opCtx, Timestamp timestamp) {
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

RecordStore* SizeCountTimestampStore::rs_ForTest() const {
    return _recordStore.get();
}

void SizeCountTimestampStore::writeToTable(OperationContext* opCtx, Timestamp timestamp) {
    assertInWriteUnitOfWorkAndLocked(opCtx);

    auto& ru = *shard_role_details::getRecoveryUnit(opCtx);
    auto& container = getIntegerKeyedContainer(*_recordStore);
    auto val = BSON(kValidAsOfKey << timestamp);
    std::span<const char> valSpan{val.objdata(), static_cast<size_t>(val.objsize())};

    // Bypass container_write — it checks canAcceptWritesFor which fails on a secondary in
    // INITIAL_SYNC state. This write is unreplicated so we can safely use
    // container::ExistingKeyPolicy::Overwrite.
    massertStatusOK(container.insert(
        ru, kTimestampContainerKey, valSpan, container::ExistingKeyPolicy::overwrite));
}
}  // namespace mongo::replicated_fast_count
