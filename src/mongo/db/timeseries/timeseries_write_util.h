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

#include <algorithm>
#include <boost/optional/optional.hpp>
#include <functional>
#include <memory>
#include <variant>
#include <vector>

#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/oid.h"
#include "mongo/db/catalog/collection.h"
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
#include "mongo/stdx/unordered_map.h"
#include "mongo/stdx/unordered_set.h"

namespace mongo::timeseries {

/**
 * Asserts the buckets collection exists and has valid time-series options.
 *
 * Assumes already holding a lock on the collection.
 */
void assertTimeseriesBucketsCollection(const Collection* bucketsColl);

/**
 * Returns the document for writing a new bucket with 'measurements'. Generates the id and
 * calculates the min and max fields while building the document.
 *
 * The measurements must already be known to fit in the same bucket. No checks will be done.
 */
BSONObj makeBucketDocument(const std::vector<BSONObj>& measurements,
                           const NamespaceString& nss,
                           const UUID& collectionUUID,
                           const TimeseriesOptions& options,
                           const StringDataComparator* comparator);

using TimeseriesBatches = std::vector<std::shared_ptr<bucket_catalog::WriteBatch>>;
using TimeseriesStmtIds = stdx::unordered_map<bucket_catalog::WriteBatch*, std::vector<StmtId>>;

/**
 * Retrieves the opTime and electionId according to the current replication mode.
 */
void getOpTimeAndElectionId(OperationContext* opCtx,
                            boost::optional<repl::OpTime>* opTime,
                            boost::optional<OID>* electionId);

enum class BucketReopeningPermittance {
    kAllowed,
    kDisallowed,
};

using CompressAndWriteBucketFunc = std::function<void(
    OperationContext*, const bucket_catalog::BucketId&, const NamespaceString&, StringData)>;

/**
 * Attempts to insert a measurement doc into a bucket in the bucket catalog and retries
 * automatically on certain errors. Only reopens existing buckets if the insert was initiated from a
 * user insert.
 *
 * Returns the write batch of the insert and other information if succeeded.
 */
StatusWith<timeseries::bucket_catalog::InsertResult> attemptInsertIntoBucket(
    OperationContext* opCtx,
    bucket_catalog::BucketCatalog& bucketCatalog,
    const Collection* bucketsColl,
    TimeseriesOptions& timeSeriesOptions,
    const BSONObj& measurementDoc,
    BucketReopeningPermittance,
    const CompressAndWriteBucketFunc& compressAndWriteBucketFunc);

/**
 * Prepares the final write batches needed for performing the writes to storage.
 */
template <typename T, typename Fn>
std::vector<std::reference_wrapper<std::shared_ptr<timeseries::bucket_catalog::WriteBatch>>>
determineBatchesToCommit(T& batches, Fn&& extractElem) {
    stdx::unordered_set<bucket_catalog::WriteBatch*> processedBatches;
    std::vector<std::reference_wrapper<std::shared_ptr<timeseries::bucket_catalog::WriteBatch>>>
        batchesToCommit;
    for (auto& elem : batches) {
        std::shared_ptr<timeseries::bucket_catalog::WriteBatch>& batch = extractElem(elem);
        if (!processedBatches.contains(batch.get())) {
            batchesToCommit.push_back(batch);
            processedBatches.insert(batch.get());
        }
    }

    // Sort by bucket so that preparing the commit for each batch cannot deadlock.
    std::sort(batchesToCommit.begin(), batchesToCommit.end(), [](auto left, auto right) {
        return left.get()->bucketId.oid < right.get()->bucketId.oid;
    });

    return batchesToCommit;
}

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
    const CompressAndWriteBucketFunc& compressAndWriteBucketFunc,
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
void timeseriesRequestChecks(const CollectionPtr& coll,
                             R* request,
                             std::function<void(R*, const TimeseriesOptions&)> checkRequestFn) {
    timeseries::assertTimeseriesBucketsCollection(coll.get());
    auto timeseriesOptions = coll->getTimeseriesOptions().value();
    checkRequestFn(request, timeseriesOptions);
}

/**
 * Function that performs checks on a delete request being performed on a timeseries collection.
 */
void deleteRequestCheckFunction(DeleteRequest* request, const TimeseriesOptions& options);

/**
 * Function that performs checks on an update request being performed on a timeseries collection.
 */
void updateRequestCheckFunction(UpdateRequest* request, const TimeseriesOptions& options);

TimeseriesBatches insertBatchOfMeasurements(OperationContext* opCtx,
                                            bucket_catalog::BucketCatalog& catalog,
                                            const Collection* bucketsColl,
                                            const StringDataComparator* comparator,
                                            const std::vector<BSONObj>& measurements,
                                            bucket_catalog::InsertContext& insertContext);

}  // namespace mongo::timeseries
