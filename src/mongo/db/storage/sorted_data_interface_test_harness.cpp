// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/storage/sorted_data_interface_test_harness.h"

#include "mongo/bson/ordering.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/db/storage/sorted_data_interface_test_assert.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/util/assert_util.h"

#include <algorithm>
#include <memory>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

std::function<std::unique_ptr<SortedDataInterfaceHarnessHelper>(int32_t cacheSizeMB)>
    sortedDataInterfaceHarnessFactory;

}  // namespace

std::unique_ptr<SortedDataInterface> SortedDataInterfaceHarnessHelper::newSortedDataInterface(
    OperationContext* opCtx,
    bool unique,
    bool partial,
    std::initializer_list<IndexKeyEntry> toInsert) {
    invariant(std::is_sorted(
        toInsert.begin(), toInsert.end(), IndexEntryComparison(Ordering::make(BSONObj()))));

    auto index = newSortedDataInterface(opCtx, unique, partial);
    insertToIndex(opCtx, index.get(), toInsert);
    return index;
}

void insertToIndex(OperationContext* opCtx,
                   SortedDataInterface* index,
                   std::initializer_list<IndexKeyEntry> toInsert,
                   bool dupsAllowed) {
    auto& ru = *shard_role_details::getRecoveryUnit(opCtx);
    StorageWriteTransaction txn(ru);
    for (auto&& entry : toInsert) {
        ASSERT_SDI_INSERT_OK(
            index->insert(opCtx, ru, makeKeyString(index, entry.key, entry.loc), dupsAllowed));
    }
    txn.commit();
}

void removeFromIndex(OperationContext* opCtx,
                     SortedDataInterface* index,
                     std::initializer_list<IndexKeyEntry> toRemove) {
    auto& ru = *shard_role_details::getRecoveryUnit(opCtx);
    StorageWriteTransaction txn(ru);
    for (auto&& entry : toRemove) {
        index->unindex(opCtx, ru, makeKeyString(index, entry.key, entry.loc), true);
    }
    txn.commit();
}

key_string::Value makeKeyString(SortedDataInterface* sorted,
                                BSONObj bsonKey,
                                const boost::optional<RecordId>& rid) {
    key_string::Builder builder(sorted->getKeyStringVersion(), bsonKey, sorted->getOrdering());
    if (rid) {
        builder.appendRecordId(*rid);
    }
    return builder.getValueCopy();
}

key_string::Builder makeKeyStringForSeek(SortedDataInterface* sorted, BSONObj bsonKey) {
    key_string::Builder builder(sorted->getKeyStringVersion(), bsonKey, sorted->getOrdering());
    return builder;
}

key_string::Builder makeKeyStringForSeek(SortedDataInterface* sorted,
                                         BSONObj bsonKey,
                                         bool isForward,
                                         bool inclusive) {
    BSONObj finalKey = BSONObj::stripFieldNames(bsonKey);
    key_string::Builder builder(sorted->getKeyStringVersion(),
                                finalKey,
                                sorted->getOrdering(),
                                isForward == inclusive
                                    ? key_string::Discriminator::kExclusiveBefore
                                    : key_string::Discriminator::kExclusiveAfter);
    return builder;
}

void registerSortedDataInterfaceHarnessHelperFactory(
    std::function<std::unique_ptr<SortedDataInterfaceHarnessHelper>(int32_t cacheSizeMB)> factory) {
    sortedDataInterfaceHarnessFactory = std::move(factory);
}

auto newSortedDataInterfaceHarnessHelper(int32_t cacheSizeMB)
    -> std::unique_ptr<SortedDataInterfaceHarnessHelper> {
    return sortedDataInterfaceHarnessFactory(cacheSizeMB);
}

}  // namespace mongo
