// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/dbtests/storage_debug_util.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/ordering.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog_entry.h"
#include "mongo/db/shard_role/shard_catalog/index_descriptor.h"
#include "mongo/db/shard_role/transaction_resources.h"
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
