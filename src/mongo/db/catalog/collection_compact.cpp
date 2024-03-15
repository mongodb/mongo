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

#include <cstdint>
#include <memory>
#include <string>

#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/timeseries/catalog_helper.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/db/views/view.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo {

MONGO_FAIL_POINT_DEFINE(pauseCompactCommandBeforeWTCompact);

using logv2::LogComponent;

namespace {

CollectionPtr getCollectionForCompact(OperationContext* opCtx, const NamespaceString& resolvedNss) {
    invariant(
        shard_role_details::getLocker(opCtx)->isCollectionLockedForMode(resolvedNss, MODE_IX));

    // Hold reference to the catalog for collection lookup without locks to be safe.
    auto collectionCatalog = CollectionCatalog::get(opCtx);
    CollectionPtr collection(collectionCatalog->lookupCollectionByNamespace(opCtx, resolvedNss));

    if (!collection) {
        std::shared_ptr<const ViewDefinition> view =
            collectionCatalog->lookupView(opCtx, resolvedNss);
        uassert(ErrorCodes::CommandNotSupportedOnView, "can't compact a view", !view);
        uasserted(ErrorCodes::NamespaceNotFound, "collection does not exist");
    }

    return collection;
}

}  // namespace

StatusWith<int64_t> compactCollection(OperationContext* opCtx,
                                      const CompactOptions& options,
                                      const NamespaceString& collectionNss) {
    AutoGetDb autoDb(opCtx, collectionNss.dbName(), MODE_IX);
    Database* database = autoDb.getDb();
    uassert(ErrorCodes::NamespaceNotFound, "database does not exist", database);

    // If we're dealing with a time-series collection, we need to lock the buckets namespace.
    NamespaceString resolvedNss = collectionNss;
    if (timeseries::getTimeseriesOptions(
            opCtx, collectionNss, /*convertToBucketsNamespace=*/true)) {
        resolvedNss = collectionNss.makeTimeseriesBucketsNamespace();
    }

    // The collection lock will be upgraded to an exclusive lock if the record store does not
    // support online compaction.
    boost::optional<Lock::CollectionLock> collLk;
    collLk.emplace(opCtx, resolvedNss, MODE_IX);

    CollectionPtr collection = getCollectionForCompact(opCtx, resolvedNss);
    DisableDocumentValidation validationDisabler(opCtx);

    auto recordStore = collection->getRecordStore();
    OldClientContext ctx(opCtx, resolvedNss);

    if (!recordStore->compactSupported())
        return Status(ErrorCodes::CommandNotSupported,
                      str::stream() << "cannot compact collection with record store: "
                                    << recordStore->name());

    if (!recordStore->supportsOnlineCompaction()) {
        // Storage engines that disallow online compaction should compact under an exclusive lock.
        collLk.emplace(opCtx, resolvedNss, MODE_X);

        // Ensure the collection was not dropped during the re-lock.
        collection = getCollectionForCompact(opCtx, resolvedNss);
        recordStore = collection->getRecordStore();
    }

    LOGV2_OPTIONS(20284, {LogComponent::kCommand}, "Compact begin", logAttrs(collectionNss));

    auto bytesBefore = recordStore->storageSize(opCtx) + collection->getIndexSize(opCtx);
    auto indexCatalog = collection->getIndexCatalog();

    pauseCompactCommandBeforeWTCompact.pauseWhileSet();

    auto compactCollectionStatus = recordStore->compact(opCtx, options);
    if (!compactCollectionStatus.isOK())
        return compactCollectionStatus;

    // Compact all indexes (not including unfinished indexes)
    auto compactIndexesStatus = indexCatalog->compactIndexes(opCtx, options);
    if (!compactIndexesStatus.isOK())
        return compactIndexesStatus;

    // The compact operation might grow the file size if there is little free space left, because
    // running a compact also triggers a checkpoint, which requires some space. Additionally, it is
    // possible for concurrent writes and index builds to cause the size to grow while compact is
    // running. So it is possible for the size after a compact to be larger than before it.
    if (options.dryRun) {
        // When a dry run is triggered, each compact call returns an estimated number of bytes that
        // can be reclaimed.
        int64_t estimatedBytes =
            compactCollectionStatus.getValue() + compactIndexesStatus.getValue();
        LOGV2(8352600,
              "Compact end",
              logAttrs(collectionNss),
              "estimatedBytes"_attr = estimatedBytes);
        return estimatedBytes;
    } else {
        auto bytesAfter = recordStore->storageSize(opCtx) + collection->getIndexSize(opCtx);
        auto bytesDiff = static_cast<int64_t>(bytesBefore) - static_cast<int64_t>(bytesAfter);
        LOGV2(7386700,
              "Compact end",
              logAttrs(collectionNss),
              "bytesBefore"_attr = bytesBefore,
              "bytesAfter"_attr = bytesAfter,
              "bytesDiff"_attr = bytesDiff);
        return bytesDiff;
    }
}

}  // namespace mongo
