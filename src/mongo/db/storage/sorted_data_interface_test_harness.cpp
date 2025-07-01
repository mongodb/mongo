/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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
