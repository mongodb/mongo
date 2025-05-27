/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/bson/bsonobj.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_identifiers.h"
#include "mongo/db/timeseries/bucket_catalog/execution_stats.h"
#include "mongo/util/future.h"

#include <boost/none.hpp>
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
