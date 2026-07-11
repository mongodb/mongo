// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/oid.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/db/record_id.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_catalog.h"
#include "mongo/db/timeseries/bucket_catalog/write_batch.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/db/timeseries/timeseries_index_schema_conversion_functions.h"
#include "mongo/db/version_context.h"
#include "mongo/util/modules.h"

#include <functional>
#include <memory>
#include <string_view>
#include <variant>
#include <vector>

#include <boost/optional/optional.hpp>

[[MONGO_MOD_PUBLIC]];

namespace mongo::timeseries {

/**
 * Asserts the buckets collection exists and has valid time-series options.
 *
 * Assumes already holding a lock on the collection.
 */
void assertTimeseriesBucketsCollection(const Collection* bucketsColl);

/**
 * Performs modifications atomically for a user command on a time-series collection.
 *
 * Replaces the bucket document for a partial bucket modification and removes the bucket for a full
 * bucket modification. Inserts or updates bucket documents if provided.
 *
 * All the modifications are written and replicated atomically.
 */
void performAtomicWrites(
    OperationContext* opCtx,
    const CollectionPtr& coll,
    const RecordId& recordId,
    const boost::optional<std::variant<mongo::write_ops::UpdateCommandRequest,
                                       mongo::write_ops::DeleteCommandRequest>>& modificationOp,
    const std::vector<mongo::write_ops::InsertCommandRequest>& insertOps,
    const std::vector<mongo::write_ops::UpdateCommandRequest>& updateOps,
    bool fromMigrate,
    StmtId stmtId);

/**
 * Constructs the write request with the provided measurements and performs the write atomically for
 * a time-series user delete on one bucket.
 */
void performAtomicWritesForDelete(OperationContext* opCtx,
                                  const CollectionPtr& coll,
                                  const RecordId& recordId,
                                  const std::vector<BSONObj>& unchangedMeasurements,
                                  bool fromMigrate,
                                  StmtId stmtId,
                                  Date_t currentMinTime);

/**
 * Constructs the write requests with the provided measurements and performs the writes atomically
 * for a time-series user update on one bucket.
 */
void performAtomicWritesForUpdate(
    OperationContext* opCtx,
    const CollectionPtr& coll,
    const RecordId& recordId,
    const boost::optional<std::vector<BSONObj>>& unchangedMeasurements,
    const std::vector<BSONObj>& modifiedMeasurements,
    bucket_catalog::BucketCatalog& sideBucketCatalog,
    bool fromMigrate,
    StmtId stmtId,
    std::set<bucket_catalog::BucketId>* bucketIds,
    boost::optional<Date_t> currentMinTime);

/**
 * Change the bucket namespace to time-series view namespace for time-series command.
 */
BSONObj timeseriesViewCommand(const BSONObj& cmd, std::string cmdName, std::string_view viewNss);

/**
 * Translates the hint provided for an update/delete request on a timeseries view to match the
 * indexes of the underlying bucket collection.
 */
template <typename R>
void timeseriesHintTranslation(const CollectionPtr& coll, R* request) {
    // Only translate the hint if it is specified with an index key.
    auto timeseriesOptions = coll->getTimeseriesOptions();
    if (timeseries::isHintIndexKey(request->getHint())) {
        request->setHint(uassertStatusOK(timeseries::createBucketsIndexSpecFromTimeseriesIndexSpec(
            *timeseriesOptions, request->getHint())));
    }
}

/**
 * Performs checks on an update/delete request being performed on a timeseries view.
 */
template <typename R>
void timeseriesRequestChecks(
    const VersionContext& vCtx,
    const CollectionPtr& coll,
    R* request,
    std::function<void(const VersionContext&, R*, const TimeseriesOptions&)> checkRequestFn) {
    timeseries::assertTimeseriesBucketsCollection(coll.get());
    auto timeseriesOptions = coll->getTimeseriesOptions().value();
    checkRequestFn(vCtx, request, timeseriesOptions);
}

/**
 * Function that performs checks on a delete request being performed on a timeseries collection.
 */
void deleteRequestCheckFunction(const VersionContext& vCtx,
                                DeleteRequest* request,
                                const TimeseriesOptions& options);

/**
 * Function that performs checks on an update request being performed on a timeseries collection.
 */
void updateRequestCheckFunction(const VersionContext& vCtx,
                                UpdateRequest* request,
                                const TimeseriesOptions& options);
}  // namespace mongo::timeseries
