/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/query/aidb/ai_index_accessor.h"

#include "mongo/db/index/index_access_method.h"
#include "mongo/db/storage/index_entry_comparison.h"
#include "mongo/db/storage/sorted_data_interface.h"

namespace mongo::ai {
IndexAccessor::IndexAccessor(OperationContext* opCtx,
                             const NamespaceString& collectionNamespace,
                             StringData indexName)
    : _opCtx{opCtx}, _autoColl{opCtx, collectionNamespace} {
    uassert(7777710, "collection not found", _autoColl);
    const IndexDescriptor* indexDescriptor =
        _autoColl->getIndexCatalog()->findIndexByName(opCtx, indexName);
    uassert(7777711, "index not found", indexDescriptor);
    _indexEntry = indexDescriptor->getEntry()->shared_from_this();

    _recordStore = _autoColl.getCollection()->getRecordStore();
}

std::vector<BSONObj> IndexAccessor::findAll(const BSONObj& key) {
    IndexAccessMethod* accessMethod = _indexEntry->accessMethod();
    invariant(accessMethod);
    SortedDataInterface* sortedDataInterface = accessMethod->getSortedDataInterface();
    invariant(sortedDataInterface);
    std::unique_ptr<SortedDataInterface::Cursor> indexCursor =
        sortedDataInterface->newCursor(_opCtx);
    invariant(indexCursor);

    indexCursor->setEndPosition(key, /* inclusive */ true);
    KeyString::Value keyString = IndexEntryComparison::makeKeyStringFromBSONKeyForSeek(
        key,
        sortedDataInterface->getKeyStringVersion(),
        sortedDataInterface->getOrdering(),
        /*forward*/ true,
        /*inclusive*/ true);
    boost::optional<IndexKeyEntry> keyEntry = indexCursor->seek(keyString);

    std::unique_ptr<SeekableRecordCursor> recordCursor = _recordStore->getCursor(_opCtx);
    std::vector<BSONObj> result{};


    while (keyEntry) {
        boost::optional<Record> record = recordCursor->seekExact(keyEntry->loc);
        invariant(record);
        result.emplace_back(record->data.releaseToBson());
        keyEntry = indexCursor->next();
    }
    return result;
}
}  // namespace mongo::ai
