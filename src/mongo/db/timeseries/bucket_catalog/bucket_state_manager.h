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

#include "mongo/bson/oid.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_identifiers.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_state.h"
#include "mongo/util/concurrency/with_lock.h"

namespace mongo::timeseries::bucket_catalog {

struct Bucket;

/**
 * A helper class to maintain global state about the catalog era used to support asynchronous
 * 'clear' operations. Provides thread-safety by taking the catalog '_mutex' for all operations.
 */
class BucketStateManager {
public:
    using Era = std::uint64_t;
    using ShouldClearFn = std::function<bool(const NamespaceString&)>;
    using StateChangeFn =
        std::function<boost::optional<BucketState>(boost::optional<BucketState>, Era)>;

    explicit BucketStateManager(Mutex* m);

    Era getEra();
    Era getEraAndIncrementCount();
    void decrementCountForEra(Era value);
    Era getCountForEra(Era value);

    /**
     * Asynchronously clears all buckets belonging to namespaces satisfying the 'shouldClear'
     * predicate.
     */
    void clearSetOfBuckets(std::function<bool(const NamespaceString&)>&& shouldClear);

    /**
     * Returns the number of clear operations currently stored in the clear registry.
     */
    uint64_t getClearOperationsCount();

    /**
     * Retrieves the bucket state if it is tracked in the catalog. Modifies the bucket state if
     * the bucket is found to have been cleared.
     */
    boost::optional<BucketState> getBucketState(Bucket* bucket);

    /**
     * Retrieves the bucket state if it is tracked in the catalog.
     */
    boost::optional<BucketState> getBucketState(const BucketId& bucketId) const;

    /**
     * Checks whether the bucket has been cleared before changing the bucket state as requested.
     * If the bucket has been cleared, it will set the kCleared flag instead and ignore the
     * requested 'change'. For more details about how the 'change' is processed, see the other
     * variant of this function that takes an 'OID' parameter.
     */
    boost::optional<BucketState> changeBucketState(Bucket* bucket, const StateChangeFn& change);

    /**
     * Changes the bucket state, taking into account the current state, the requested 'change',
     * and allowed state transitions. The return value, if set, is the final state of the bucket
     * with the given ID.
     *
     * If no state is currently tracked for 'id', then the optional input state to 'change' will
     * be 'none'. To initialize the state, 'change' may return a valid `BucketState', and it
     * will be added to the set of tracked states.
     *
     * Similarly, if 'change' returns 'none', the value will be removed from the registry. To
     * perform a noop (i.e. if upon inspecting the input, the change would be invalid), 'change'
     * may simply return its input state unchanged.
     */
    boost::optional<BucketState> changeBucketState(const BucketId& bucketId,
                                                   const StateChangeFn& change);

    /**
     * Appends statistics for observability.
     */
    void appendStats(BSONObjBuilder* builder) const;

protected:
    void _decrementEraCountHelper(Era era);
    void _incrementEraCountHelper(Era era);
    boost::optional<BucketState> _changeBucketStateHelper(WithLock withLock,
                                                          const BucketId& bucketId,
                                                          const StateChangeFn& change);

    /**
     * Returns whether the Bucket has been marked as cleared by checking against the
     * clearRegistry. Advances Bucket's era up to current global era if the bucket has not been
     * cleared.
     */
    bool _isMemberOfClearedSet(WithLock catalogLock, Bucket* bucket);

    /**
     * A helper function to set the kCleared flag for the given bucket. Results in a noop if the
     * bucket state isn't currently tracked.
     */
    boost::optional<BucketState> _markIndividualBucketCleared(WithLock catalogLock,
                                                              const BucketId& bucketId);

    /**
     * Removes clear operations from the clear registry that no longer need to be tracked.
     */
    void _cleanClearRegistry();

    // Pointer to 'BucketCatalog::_mutex'.
    Mutex* _mutex;

    // Global number tracking the current number of eras that have passed. Incremented each time
    // a bucket is cleared.
    Era _era;

    // Mapping of era to counts of how many buckets are associated with that era.
    std::map<Era, uint64_t> _countMap;

    // Bucket state for synchronization with direct writes
    stdx::unordered_map<BucketId, BucketState, BucketHasher> _bucketStates;

    // Registry storing clear operations. Maps from era to a lambda function which takes in
    // information about a Bucket and returns whether the Bucket has been cleared.
    std::map<Era, ShouldClearFn> _clearRegistry;
};

}  // namespace mongo::timeseries::bucket_catalog
