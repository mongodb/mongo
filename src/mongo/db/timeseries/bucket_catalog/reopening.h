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

#include <boost/optional/optional.hpp>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>

#include <boost/none.hpp>

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_identifiers.h"
#include "mongo/db/timeseries/bucket_catalog/execution_stats.h"

namespace mongo::timeseries::bucket_catalog {

struct Stripe;
class BucketCatalog;

/**
 * Whether to include the memory overhead of the map data structure when calculating marginal memory
 * usage for a bucket.
 */
enum class IncludeMemoryOverheadFromMap { kInclude, kExclude };

/**
 * Function that should run validation against the bucket to ensure it's a proper bucket document.
 * Typically, this should execute Collection::checkValidation.
 */
using BucketDocumentValidator = std::function<std::pair<Collection::SchemaValidationResult, Status>(
    OperationContext*, const BSONObj&)>;

/**
 * Used to pass a bucket document into the BucketCatalog to reopen.
 */
struct BucketToReopen {
    BSONObj bucketDocument;
    BucketDocumentValidator validator;
};

/**
 * Information of a Bucket that got archived while performing an operation on the BucketCatalog.
 */
struct ArchivedBucket {
    ArchivedBucket() = delete;
    ArchivedBucket(const BucketId& bucketId, const tracked_string& timeField);

    BucketId bucketId;
    tracked_string timeField;
};


/**
 * A light wrapper around a promise type to allow potentially conflicting operations to ensure
 * orderly waiting and observability. Equivalent functionality exists in 'WriteBatch'.
 */
struct ReopeningRequest {
    ReopeningRequest() = delete;
    ReopeningRequest(ExecutionStatsController&& stats, boost::optional<OID> oid);

    ExecutionStatsController stats;
    boost::optional<OID> oid;
    SharedPromise<void> promise;
};

/**
 * RAII type that tracks the state needed to coordinate the reopening of closed buckets between
 * reentrant 'tryInsert'/'insert' calls.
 *
 * The constructor initializes all the private state necessary for the RAII-behavior, while the
 * public members are used to pass information between the bucket catalog and the caller about the
 * actual reopening attempt.
 */
class ReopeningContext {
public:
    // A reopening candidate can be an OID for archive-based reopening, or an aggregation pipeline
    // for query-based reopening.
    using CandidateType = std::variant<std::monostate, OID, std::vector<BSONObj>>;

    ReopeningContext() = delete;
    ~ReopeningContext();
    ReopeningContext(const ReopeningContext&) = delete;
    ReopeningContext& operator=(const ReopeningContext&) = delete;

    /**
     * Must save all state needed by 'clear()' due to its requirements.
     */
    ReopeningContext(BucketCatalog& catalog,
                     Stripe& stripe,
                     WithLock stripeLock,
                     BucketKey key,
                     uint64_t era,
                     CandidateType&& candidate);

    /**
     * Move-only type to ensure we do release state exactly once.
     */
    ReopeningContext(ReopeningContext&&);
    ReopeningContext& operator=(ReopeningContext&&);

    /**
     * Should only be called by the bucket catalog when the stripe lock has been acquired and will
     * be held through the duration of the remaining bucket reopening operations.
     */
    void clear(WithLock stripeLock);

    // Set by the bucket catalog to ensure proper synchronization of reopening attempt.
    uint64_t catalogEra;

    // Information needed for the caller to locate a candidate bucket to reopen from disk, populated
    // by the bucket catalog.
    CandidateType candidate;

    // Communicates to the BucketCatalog whether an attempt was made to fetch a query or bucket, and
    // the resulting bucket document that was found, if any. Populated by the caller.
    bool fetchedBucket{false};
    bool queriedBucket{false};
    boost::optional<BucketToReopen> bucketToReopen{boost::none};

private:
    Stripe* _stripe;
    BucketKey _key;
    boost::optional<OID> _oid;
    bool _cleared;

    void clear();
};

/**
 * Waits for the specified ReopeningRequest to be completed. Blocking.
 */
void waitForReopeningRequest(ReopeningRequest& request);

}  // namespace mongo::timeseries::bucket_catalog
