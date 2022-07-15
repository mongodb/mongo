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


#include "mongo/db/catalog/collection_compact.h"

#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/catalog/index_key_validate.h"
#include "mongo/db/catalog/multi_index_block.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index_builds_coordinator.h"
#include "mongo/db/operation_context.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


namespace mongo {

using logv2::LogComponent;

namespace {

CollectionPtr getCollectionForCompact(OperationContext* opCtx,
                                      const NamespaceString& collectionNss) {
    invariant(opCtx->lockState()->isCollectionLockedForMode(collectionNss, MODE_IX));

    auto collectionCatalog = CollectionCatalog::get(opCtx);
    CollectionPtr collection = collectionCatalog->lookupCollectionByNamespace(opCtx, collectionNss);

    if (!collection) {
        std::shared_ptr<const ViewDefinition> view =
            collectionCatalog->lookupView(opCtx, collectionNss);
        uassert(ErrorCodes::CommandNotSupportedOnView, "can't compact a view", !view);
        uasserted(ErrorCodes::NamespaceNotFound, "collection does not exist");
    }

    return collection;
}

}  // namespace

StatusWith<int64_t> compactCollection(OperationContext* opCtx,
                                      const NamespaceString& collectionNss) {
    AutoGetDb autoDb(opCtx, collectionNss.dbName(), MODE_IX);
    Database* database = autoDb.getDb();
    uassert(ErrorCodes::NamespaceNotFound, "database does not exist", database);

    // The collection lock will be downgraded to an intent lock if the record store supports
    // online compaction.
    boost::optional<Lock::CollectionLock> collLk;
    collLk.emplace(opCtx, collectionNss, MODE_X);

    CollectionPtr collection = getCollectionForCompact(opCtx, collectionNss);
    DisableDocumentValidation validationDisabler(opCtx);

    auto recordStore = collection->getRecordStore();

    OldClientContext ctx(opCtx, collectionNss);

    if (!recordStore->compactSupported())
        return Status(ErrorCodes::CommandNotSupported,
                      str::stream() << "cannot compact collection with record store: "
                                    << recordStore->name());

    if (recordStore->supportsOnlineCompaction()) {
        // Storage engines that allow online compaction should do so using an intent lock on the
        // collection.
        collLk.emplace(opCtx, collectionNss, MODE_IX);

        // Ensure the collection was not dropped during the re-lock.
        collection = getCollectionForCompact(opCtx, collectionNss);
        recordStore = collection->getRecordStore();
    }

    LOGV2_OPTIONS(20284,
                  {LogComponent::kCommand},
                  "compact {namespace} begin",
                  "Compact begin",
                  "namespace"_attr = collectionNss);

    auto oldTotalSize = recordStore->storageSize(opCtx) + collection->getIndexSize(opCtx);
    auto indexCatalog = collection->getIndexCatalog();

    Status status = recordStore->compact(opCtx);
    if (!status.isOK())
        return status;

    // Compact all indexes (not including unfinished indexes)
    status = indexCatalog->compactIndexes(opCtx);
    if (!status.isOK())
        return status;

    auto totalSizeDiff =
        oldTotalSize - recordStore->storageSize(opCtx) - collection->getIndexSize(opCtx);
    LOGV2(20286,
          "compact {namespace} end, bytes freed: {freedBytes}",
          "Compact end",
          "namespace"_attr = collectionNss,
          "freedBytes"_attr = totalSizeDiff);
    return totalSizeDiff;
}

}  // namespace mongo
