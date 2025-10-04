/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/dbtests/storage_debug_util.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/ordering.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/index_catalog.h"
#include "mongo/db/local_catalog/index_catalog_entry.h"
#include "mongo/db/local_catalog/index_descriptor.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/db/validate/validate_results.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


namespace mongo {

namespace StorageDebugUtil {

void printCollectionAndIndexTableEntries(OperationContext* opCtx, const NamespaceString& nss) {
    invariant(!shard_role_details::getLocker(opCtx)->isLocked());
    AutoGetCollection coll(opCtx, nss, MODE_IS);

    LOGV2(51807, "Dumping collection table and index tables' entries for debugging...");

    // Iterate and print the collection table (record store) documents.
    RecordStore* rs = coll->getRecordStore();
    auto rsCursor = rs->getCursor(opCtx, *shard_role_details::getRecoveryUnit(opCtx));
    boost::optional<Record> rec = rsCursor->next();
    LOGV2(51808, "[Debugging] Collection table entries:");
    while (rec) {
        LOGV2(51809,
              "[Debugging](record) {record_id}, Value: {record_data}",
              "record_id"_attr = rec->id,
              "record_data"_attr = rec->data.toBson());
        rec = rsCursor->next();
    }

    // Iterate and print each index's table of documents.
    const auto indexCatalog = coll->getIndexCatalog();
    const auto it = indexCatalog->getIndexIterator(IndexCatalog::InclusionPolicy::kReady);
    auto& ru = *shard_role_details::getRecoveryUnit(opCtx);
    while (it->more()) {
        const auto indexCatalogEntry = it->next();
        const auto indexDescriptor = indexCatalogEntry->descriptor();
        const auto iam = indexCatalogEntry->accessMethod()->asSortedData();
        if (!iam) {
            LOGV2(6325101,
                  "[Debugging] skipping index {index_name} because it isn't SortedData",
                  "index_name"_attr = indexDescriptor->indexName());
            continue;
        }
        auto indexCursor = iam->newCursor(opCtx, ru, /*forward*/ true);

        const BSONObj& keyPattern = indexDescriptor->keyPattern();
        const auto ordering = Ordering::make(keyPattern);

        LOGV2(51810,
              "[Debugging] {keyPattern_str} index table entries:",
              "keyPattern_str"_attr = keyPattern);

        for (auto keyStringEntry = indexCursor->nextKeyString(ru); keyStringEntry;
             keyStringEntry = indexCursor->nextKeyString(ru)) {
            auto keyString = key_string::toBsonSafe(keyStringEntry->keyString.getView(),
                                                    ordering,
                                                    keyStringEntry->keyString.getTypeBits());
            key_string::logKeyString(keyStringEntry->loc,
                                     keyStringEntry->keyString,
                                     keyPattern,
                                     keyString,
                                     "[Debugging](index)");
        }
    }
}

void printValidateResults(const ValidateResults& results) {
    BSONObjBuilder resultObj;

    results.appendToResultObj(&resultObj, /*debugging=*/true);

    LOGV2(51812, "Results", "results"_attr = resultObj.done());
}

}  // namespace StorageDebugUtil

}  // namespace mongo
