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
#include "mongo/db/timeseries/catalog_helper.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo {

MONGO_FAIL_POINT_DEFINE(pauseCompactCommandBeforeWTCompact);

using logv2::LogComponent;

namespace {

CollectionPtr getCollectionForCompact(OperationContext* opCtx,
                                      const NamespaceString& collectionNss) {
    invariant(opCtx->lockState()->isCollectionLockedForMode(collectionNss, MODE_IX));

    NamespaceString resolvedNs = collectionNss;
    if (auto timeseriesOptions = timeseries::getTimeseriesOptions(
            opCtx, collectionNss, /*convertToBucketsNamespace=*/true)) {
        resolvedNs = collectionNss.makeTimeseriesBucketsNamespace();
    }

    auto collectionCatalog = CollectionCatalog::get(opCtx);
    CollectionPtr collection(collectionCatalog->lookupCollectionByNamespace(opCtx, resolvedNs));

    if (!collection) {
        std::shared_ptr<const ViewDefinition> view =
            collectionCatalog->lookupView(opCtx, resolvedNs);
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

    // The collection lock will be upgraded to an exclusive lock if the record store does not
    // support online compaction.
    boost::optional<Lock::CollectionLock> collLk;
    collLk.emplace(opCtx, collectionNss, MODE_IX);

    CollectionPtr collection = getCollectionForCompact(opCtx, collectionNss);
    DisableDocumentValidation validationDisabler(opCtx);

    auto recordStore = collection->getRecordStore();

    OldClientContext ctx(opCtx, collectionNss);

    if (!recordStore->compactSupported())
        return Status(ErrorCodes::CommandNotSupported,
                      str::stream() << "cannot compact collection with record store: "
                                    << recordStore->name());

    if (!recordStore->supportsOnlineCompaction()) {
        // Storage engines that disallow online compaction should compact under an exclusive lock.
        collLk.emplace(opCtx, collectionNss, MODE_X);

        // Ensure the collection was not dropped during the re-lock.
        collection = getCollectionForCompact(opCtx, collectionNss);
        recordStore = collection->getRecordStore();
    }

    LOGV2_OPTIONS(20284,
                  {LogComponent::kCommand},
                  "compact {namespace} begin",
                  "Compact begin",
                  logAttrs(collectionNss));

    auto bytesBefore = recordStore->storageSize(opCtx) + collection->getIndexSize(opCtx);
    auto indexCatalog = collection->getIndexCatalog();

    pauseCompactCommandBeforeWTCompact.pauseWhileSet();

    Status status = recordStore->compact(opCtx);
    if (!status.isOK())
        return status;

    // Compact all indexes (not including unfinished indexes)
    status = indexCatalog->compactIndexes(opCtx);
    if (!status.isOK())
        return status;

    auto bytesAfter = recordStore->storageSize(opCtx) + collection->getIndexSize(opCtx);
    auto bytesDiff = static_cast<int64_t>(bytesBefore) - static_cast<int64_t>(bytesAfter);

    // The compact operation might grow the file size if there is little free space left, because
    // running a compact also triggers a checkpoint, which requires some space. Additionally, it is
    // possible for concurrent writes and index builds to cause the size to grow while compact is
    // running. So it is possible for the size after a compact to be larger than before it.
    LOGV2(7386700,
          "Compact end",
          logAttrs(collectionNss),
          "bytesBefore"_attr = bytesBefore,
          "bytesAfter"_attr = bytesAfter,
          "bytesDiff"_attr = bytesDiff);

    return bytesDiff;
}

}  // namespace mongo
