/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/timeseries/timeseries_op_observer.h"

#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_catalog.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_catalog_helpers.h"
#include "mongo/db/timeseries/timeseries_extended_range.h"
#include "mongo/stdx/unordered_set.h"

#include <vector>


namespace mongo {

void TimeSeriesOpObserver::onInserts(OperationContext* opCtx,
                                     const CollectionPtr& coll,
                                     std::vector<InsertStatement>::const_iterator first,
                                     std::vector<InsertStatement>::const_iterator last,
                                     std::vector<bool> fromMigrate,
                                     bool defaultFromMigrate) {
    const auto& nss = coll->ns();

    if (!nss.isTimeseriesBucketsCollection()) {
        return;
    }

    // Check if the bucket _id is sourced from a date outside the standard range. If our writes
    // end up erroring out or getting rolled back, then this flag will stay set. This is okay
    // though, as it only disables some query optimizations and won't result in any correctness
    // issues if the flag is set when it doesn't need to be (as opposed to NOT being set when it
    // DOES need to be -- that will cause correctness issues). Additionally, if the user tried
    // to insert measurements with dates outside the standard range, chances are they will do so
    // again, and we will have only set the flag a little early.
    invariant(opCtx->lockState()->isCollectionLockedForMode(nss, MODE_IX));
    // Hold reference to the catalog for collection lookup without locks to be safe.
    auto catalog = CollectionCatalog::get(opCtx);
    auto bucketsColl = catalog->lookupCollectionByNamespace(opCtx, nss);
    tassert(6905201, "Could not find collection for write", bucketsColl);
    auto timeSeriesOptions = bucketsColl->getTimeseriesOptions();
    if (timeSeriesOptions.has_value()) {
        if (auto currentSetting = bucketsColl->getRequiresTimeseriesExtendedRangeSupport();
            !currentSetting &&
            timeseries::bucketsHaveDateOutsideStandardRange(
                timeSeriesOptions.value(), first, last)) {
            bucketsColl->setRequiresTimeseriesExtendedRangeSupport(opCtx);
        }
    }
}

void TimeSeriesOpObserver::onUpdate(OperationContext* opCtx,
                                    const OplogUpdateEntryArgs& args,
                                    OpStateAccumulator* opAccumulator) {
    const auto& nss = args.coll->ns();

    if (!nss.isTimeseriesBucketsCollection()) {
        return;
    }

    if (args.updateArgs->source != OperationSource::kTimeseriesInsert) {
        OID bucketId = args.updateArgs->updatedDoc["_id"].OID();
        timeseries::bucket_catalog::handleDirectWrite(opCtx, nss, bucketId);
    }
}

void TimeSeriesOpObserver::aboutToDelete(OperationContext* opCtx,
                                         const CollectionPtr& coll,
                                         const BSONObj& doc) {
    const auto& nss = coll->ns();

    if (!nss.isTimeseriesBucketsCollection()) {
        return;
    }

    OID bucketId = doc["_id"].OID();
    timeseries::bucket_catalog::handleDirectWrite(opCtx, nss, bucketId);
}

void TimeSeriesOpObserver::onDropDatabase(OperationContext* opCtx, const DatabaseName& dbName) {
    auto& bucketCatalog = timeseries::bucket_catalog::BucketCatalog::get(opCtx);
    timeseries::bucket_catalog::clear(bucketCatalog, dbName.db());
}

repl::OpTime TimeSeriesOpObserver::onDropCollection(OperationContext* opCtx,
                                                    const NamespaceString& collectionName,
                                                    const UUID& uuid,
                                                    std::uint64_t numRecords,
                                                    CollectionDropType dropType) {
    if (collectionName.isTimeseriesBucketsCollection()) {
        auto& bucketCatalog = timeseries::bucket_catalog::BucketCatalog::get(opCtx);
        timeseries::bucket_catalog::clear(bucketCatalog,
                                          collectionName.getTimeseriesViewNamespace());
    }

    return {};
}

void TimeSeriesOpObserver::_onReplicationRollback(OperationContext* opCtx,
                                                  const RollbackObserverInfo& rbInfo) {
    stdx::unordered_set<NamespaceString> timeseriesNamespaces;
    for (const auto& ns : rbInfo.rollbackNamespaces) {
        if (ns.isTimeseriesBucketsCollection()) {
            timeseriesNamespaces.insert(ns.getTimeseriesViewNamespace());
        }
    }

    if (timeseriesNamespaces.empty()) {
        return;
    }

    auto& bucketCatalog = timeseries::bucket_catalog::BucketCatalog::get(opCtx);
    timeseries::bucket_catalog::clear(
        bucketCatalog,
        [timeseriesNamespaces = std::move(timeseriesNamespaces)](const NamespaceString& bucketNs) {
            return timeseriesNamespaces.contains(bucketNs);
        });
}

}  // namespace mongo
