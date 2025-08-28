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

#include "mongo/bson/bsonelement.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/collection_catalog.h"
#include "mongo/db/local_catalog/collection_operation_source.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_catalog.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_catalog_helpers.h"
#include "mongo/db/timeseries/bucket_catalog/global_bucket_catalog.h"
#include "mongo/db/timeseries/bucket_catalog/tracking_contexts.h"
#include "mongo/db/timeseries/timeseries_extended_range.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/tracking/vector.h"

#include <utility>
#include <vector>

#include <absl/container/node_hash_set.h>
#include <boost/optional/optional.hpp>

namespace mongo {

void TimeSeriesOpObserver::onInserts(OperationContext* opCtx,
                                     const CollectionPtr& coll,
                                     std::vector<InsertStatement>::const_iterator first,
                                     std::vector<InsertStatement>::const_iterator last,
                                     const std::vector<RecordId>& recordIds,
                                     std::vector<bool> fromMigrate,
                                     bool defaultFromMigrate,
                                     OpStateAccumulator* opAccumulator) {
    const auto& options = coll->getTimeseriesOptions();
    if (!options.has_value()) {
        return;
    }

    // Check if the bucket _id is sourced from a date outside the standard range. If our writes
    // end up erroring out or getting rolled back, then this flag will stay set. This is okay
    // though, as it only disables some query optimizations and won't result in any correctness
    // issues if the flag is set when it doesn't need to be (as opposed to NOT being set when it
    // DOES need to be -- that will cause correctness issues). Additionally, if the user tried
    // to insert measurements with dates outside the standard range, chances are they will do so
    // again, and we will have only set the flag a little early.

    tassert(6905201, "Could not find collection for write", coll);
    if (auto currentSetting = coll->getRequiresTimeseriesExtendedRangeSupport(); !currentSetting &&
        timeseries::bucketsHaveDateOutsideStandardRange(options.value(), first, last)) {
        coll->setRequiresTimeseriesExtendedRangeSupport(opCtx);
    }

    const auto& tsColl = coll.get();
    uassert(ErrorCodes::CannotInsertTimeseriesBucketsWithMixedSchema,
            "Cannot write time-series bucket containing mixed schema data, please run collMod "
            "with timeseriesBucketsMayHaveMixedSchemaData and retry your insert",
            !opCtx->isEnforcingConstraints() ||
                tsColl->getTimeseriesMixedSchemaBucketsState().canStoreMixedSchemaBucketsSafely() ||
                std::none_of(first, last, [tsColl](auto&& insert) {
                    auto mixedSchema =
                        tsColl->doesTimeseriesBucketsDocContainMixedSchemaData(insert.doc);
                    return mixedSchema.isOK() && mixedSchema.getValue();
                }));
}

void TimeSeriesOpObserver::onUpdate(OperationContext* opCtx,
                                    const OplogUpdateEntryArgs& args,
                                    OpStateAccumulator* opAccumulator) {
    const auto& options = args.coll->getTimeseriesOptions();
    if (!options.has_value()) {
        return;
    }

    if (args.updateArgs->source != OperationSource::kTimeseriesInsert) {
        timeseriesCounters.incrementDirectUpdated();
        auto mixedSchema = [&args] {
            auto result = args.coll->doesTimeseriesBucketsDocContainMixedSchemaData(
                args.updateArgs->updatedDoc);
            return result.isOK() && result.getValue();
        };

        uassert(ErrorCodes::CannotInsertTimeseriesBucketsWithMixedSchema,
                "Cannot write time-series bucket containing mixed schema data, please run collMod "
                "with timeseriesBucketsMayHaveMixedSchemaData and retry your update",
                !opCtx->isEnforcingConstraints() ||
                    args.coll->getTimeseriesMixedSchemaBucketsState()
                        .canStoreMixedSchemaBucketsSafely() ||
                    !mixedSchema());

        timeseries::bucket_catalog::handleDirectWrite(
            *shard_role_details::getRecoveryUnit(opCtx),
            timeseries::bucket_catalog::GlobalBucketCatalog::get(opCtx->getServiceContext()),
            options.value(),
            args.coll->uuid(),
            args.updateArgs->preImageDoc);
        timeseries::bucket_catalog::handleDirectWrite(
            *shard_role_details::getRecoveryUnit(opCtx),
            timeseries::bucket_catalog::GlobalBucketCatalog::get(opCtx->getServiceContext()),
            options.value(),
            args.coll->uuid(),
            args.updateArgs->updatedDoc);
    }
}

void TimeSeriesOpObserver::onDelete(OperationContext* opCtx,
                                    const CollectionPtr& coll,
                                    StmtId stmtId,
                                    const BSONObj& doc,
                                    const DocumentKey& documentKey,
                                    const OplogDeleteEntryArgs& args,
                                    OpStateAccumulator* opAccumulator) {

    const auto& options = coll->getTimeseriesOptions();
    if (!options.has_value()) {
        return;
    }

    timeseriesCounters.incrementDirectDeleted();

    timeseries::bucket_catalog::handleDirectWrite(
        *shard_role_details::getRecoveryUnit(opCtx),
        timeseries::bucket_catalog::GlobalBucketCatalog::get(opCtx->getServiceContext()),
        options.value(),
        coll->uuid(),
        doc);
}

repl::OpTime TimeSeriesOpObserver::onDropCollection(OperationContext* opCtx,
                                                    const NamespaceString& collectionName,
                                                    const UUID& uuid,
                                                    std::uint64_t numRecords,
                                                    bool markFromMigrate,
                                                    bool isTimeseries) {
    auto& bucketCatalog =
        timeseries::bucket_catalog::GlobalBucketCatalog::get(opCtx->getServiceContext());
    timeseries::bucket_catalog::drop(bucketCatalog, uuid);
    return {};
}

void TimeSeriesOpObserver::onReplicationRollback(OperationContext* opCtx,
                                                 const RollbackObserverInfo& rbInfo) {
    auto& bucketCatalog =
        timeseries::bucket_catalog::GlobalBucketCatalog::get(opCtx->getServiceContext());
    tracking::vector<UUID> clearedCollectionUUIDs = tracking::make_vector<UUID>(
        timeseries::bucket_catalog::getTrackingContext(
            bucketCatalog.trackingContexts,
            timeseries::bucket_catalog::TrackingScope::kBucketStateRegistry),
        rbInfo.rollbackUUIDs.begin(),
        rbInfo.rollbackUUIDs.end());
    timeseries::bucket_catalog::drop(bucketCatalog, std::move(clearedCollectionUUIDs));
}

}  // namespace mongo
