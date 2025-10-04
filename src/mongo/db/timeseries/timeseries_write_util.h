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

#pragma once

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/oid.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/db/record_id.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_catalog.h"
#include "mongo/db/timeseries/bucket_catalog/write_batch.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/db/timeseries/timeseries_index_schema_conversion_functions.h"
#include "mongo/db/version_context.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/stdx/unordered_set.h"

#include <algorithm>
#include <functional>
#include <memory>
#include <variant>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo::timeseries {

/**
 * Asserts the buckets collection exists and has valid time-series options.
 *
 * Assumes already holding a lock on the collection.
 */
void assertTimeseriesBucketsCollection(const Collection* bucketsColl);

/**
 * Retrieves the opTime and electionId according to the current replication mode.
 */
void getOpTimeAndElectionId(OperationContext* opCtx,
                            boost::optional<repl::OpTime>* opTime,
                            boost::optional<OID>* electionId);

/**
 * Prepares the final write batches needed for performing the writes to storage.
 */
std::vector<std::reference_wrapper<std::shared_ptr<timeseries::bucket_catalog::WriteBatch>>>
determineBatchesToCommit(bucket_catalog::TimeseriesWriteBatches& batches);
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
BSONObj timeseriesViewCommand(const BSONObj& cmd, std::string cmdName, StringData viewNss);

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
