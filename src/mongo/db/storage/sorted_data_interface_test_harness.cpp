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

#include <algorithm>
#include <memory>

#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/unittest/unittest.h"

auto mongo::SortedDataInterfaceHarnessHelper::newSortedDataInterface(
    bool unique, bool partial, std::initializer_list<IndexKeyEntry> toInsert)
    -> std::unique_ptr<SortedDataInterface> {
    invariant(std::is_sorted(
        toInsert.begin(), toInsert.end(), IndexEntryComparison(Ordering::make(BSONObj()))));

    auto index = newSortedDataInterface(unique, partial);
    insertToIndex(this, index.get(), toInsert);
    return index;
}

void mongo::insertToIndex(OperationContext* opCtx,
                          SortedDataInterface* index,
                          std::initializer_list<IndexKeyEntry> toInsert) {
    WriteUnitOfWork wuow(opCtx);
    for (auto&& entry : toInsert) {
        ASSERT_OK(index->insert(opCtx, makeKeyString(index, entry.key, entry.loc), true));
    }
    wuow.commit();
}

void mongo::removeFromIndex(OperationContext* opCtx,
                            SortedDataInterface* index,
                            std::initializer_list<IndexKeyEntry> toRemove) {
    WriteUnitOfWork wuow(opCtx);
    for (auto&& entry : toRemove) {
        index->unindex(opCtx, makeKeyString(index, entry.key, entry.loc), true);
    }
    wuow.commit();
}

mongo::KeyString::Value mongo::makeKeyString(SortedDataInterface* sorted,
                                             BSONObj bsonKey,
                                             const boost::optional<RecordId>& rid) {
    KeyString::Builder builder(sorted->getKeyStringVersion(), bsonKey, sorted->getOrdering());
    if (rid) {
        builder.appendRecordId(*rid);
    }
    return builder.getValueCopy();
}

mongo::KeyString::Value mongo::makeKeyStringForSeek(SortedDataInterface* sorted,
                                                    BSONObj bsonKey,
                                                    bool isForward,
                                                    bool inclusive) {
    BSONObj finalKey = BSONObj::stripFieldNames(bsonKey);
    KeyString::Builder builder(sorted->getKeyStringVersion(),
                               finalKey,
                               sorted->getOrdering(),
                               isForward == inclusive ? KeyString::Discriminator::kExclusiveBefore
                                                      : KeyString::Discriminator::kExclusiveAfter);
    return builder.getValueCopy();
}

namespace mongo {
namespace {
std::function<std::unique_ptr<mongo::SortedDataInterfaceHarnessHelper>()>
    sortedDataInterfaceHarnessFactory;
}

void registerSortedDataInterfaceHarnessHelperFactory(
    std::function<std::unique_ptr<mongo::SortedDataInterfaceHarnessHelper>()> factory) {
    sortedDataInterfaceHarnessFactory = std::move(factory);
}

auto newSortedDataInterfaceHarnessHelper()
    -> std::unique_ptr<mongo::SortedDataInterfaceHarnessHelper> {
    return sortedDataInterfaceHarnessFactory();
}

}  // namespace mongo
