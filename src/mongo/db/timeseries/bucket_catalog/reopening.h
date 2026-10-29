// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_identifiers.h"
#include "mongo/db/timeseries/bucket_catalog/execution_stats.h"
#include "mongo/util/future.h"
#include "mongo/util/modules.h"

#include <boost/optional/optional.hpp>

namespace mongo::timeseries::bucket_catalog {

struct Stripe;
class BucketCatalog;

/**
 * Information of a Bucket that got archived while performing an operation on the BucketCatalog.
 */
struct ArchivedBucket {
    OID oid;
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
 * RAII type that manages the lifetime of a reopening request.
 */
class ReopeningScope {
public:
    // A reopening candidate can be an OID for archive-based reopening, or an aggregation pipeline
    // for query-based reopening.
    using CandidateType = std::variant<OID, std::vector<BSONObj>>;

    ReopeningScope() = delete;
    ~ReopeningScope();

    ReopeningScope(BucketCatalog& catalog,
                   Stripe& stripe,
                   WithLock stripeLock,
                   const BucketKey& key,
                   const CandidateType& candidate);

private:
    Stripe* _stripe;
    BucketKey _key;
    boost::optional<OID> _oid;
};

/**
 * Waits for the specified ReopeningRequest to be completed. Blocking.
 */
void waitForReopeningRequest(ReopeningRequest& request);

}  // namespace mongo::timeseries::bucket_catalog
