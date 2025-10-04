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

#pragma once

#include "mongo/db/global_catalog/chunk_manager.h"
#include "mongo/db/versioning_protocol/stale_exception.h"
#include "mongo/s/write_ops/write_command_ref.h"

#include <vector>

namespace mongo {

/**
 * The NSTargeter interface is used by a WriteOp to generate and target child write operations
 * to a particular collection.
 *
 * The lifecyle of a NSTargeter is:
 *
 *   0. targetDoc/targetQuery as many times as is required
 *
 *   1a. On targeting failure we may need to refresh, note that it happened.
 *   1b. On stale config from a child write operation we may need to refresh, note the error.
 *
 *   2. refreshIfNeeded() to get newer targeting information
 *
 *   3. Goto 0.
 *
 * The refreshIfNeeded() operation must try to make progress against noted targeting or stale
 * config failures, see comments below.  No functions may block for shared resources or network
 * calls except refreshIfNeeded().
 *
 * Implementers are free to define more specific targeting error codes to allow more complex
 * error handling.
 *
 * The interface must not be used from multiple threads.
 */
class NSTargeter {
public:
    struct TargetingResult {
        TargetingResult() = default;

        TargetingResult(std::vector<ShardEndpoint> endpoints) : endpoints(std::move(endpoints)) {}

        TargetingResult(std::vector<ShardEndpoint> endpoints,
                        bool useTwoPhaseWriteProtocol,
                        bool isNonTargetedRetryableWriteWithId)
            : endpoints(std::move(endpoints)),
              useTwoPhaseWriteProtocol(useTwoPhaseWriteProtocol),
              isNonTargetedRetryableWriteWithId(isNonTargetedRetryableWriteWithId) {}

        std::vector<ShardEndpoint> endpoints;
        bool useTwoPhaseWriteProtocol = false;
        bool isNonTargetedRetryableWriteWithId = false;
    };

    virtual ~NSTargeter() = default;

    /**
     * Returns the namespace targeted.
     */
    virtual const NamespaceString& getNS() const = 0;

    virtual bool isTrackedTimeSeriesBucketsNamespace() const = 0;

    virtual bool isTrackedTimeSeriesNamespace() const = 0;

    /**
     * Returns a ShardEndpoint for a single document write or throws ShardKeyNotFound if 'doc' is
     * malformed with respect to the shard key pattern of the collection.
     */
    virtual ShardEndpoint targetInsert(OperationContext* opCtx, const BSONObj& doc) const = 0;

    /**
     * Returns a TargetingResult (which contains a vector of ShardEndpoints) for a potentially
     * multi-shard update or throws ShardKeyNotFound if 'itemRef' misses a shard key, but the type
     * of update requires it.
     */
    virtual TargetingResult targetUpdate(OperationContext* opCtx,
                                         const BatchItemRef& itemRef) const = 0;

    /**
     * Returns a TargetingResult (which contains a vector of ShardEndpoints) for a potentially
     * multi-shard delete or throws ShardKeyNotFound if 'itemRef' misses a shard key, but the type
     * of delete requires it.
     */
    virtual TargetingResult targetDelete(OperationContext* opCtx,
                                         const BatchItemRef& itemRef) const = 0;

    /**
     * Returns a vector of ShardEndpoints for all shards.
     */
    virtual std::vector<ShardEndpoint> targetAllShards(OperationContext* opCtx) const = 0;

    /**
     * Informs the targeter that a targeting failure occurred during one of the last targeting
     * operations.  If this is noted, we cannot note stale responses.
     */
    virtual void noteCouldNotTarget() = 0;

    /**
     * Informs the targeter of stale config responses for this namespace from an endpoint, with
     * further information available in the returned staleInfo.
     *
     * Any stale responses noted here will be taken into account on the next refresh.
     *
     * If stale responses are noted, we must not have noted that we cannot target.
     */
    virtual void noteStaleCollVersionResponse(OperationContext* opCtx,
                                              const StaleConfigInfo& staleInfo) = 0;

    /**
     * Informs the targeter of stale db routing version responses for this db from an endpoint,
     * with further information available in the returned staleInfo.
     *
     * Any stale responses noted here will be taken into account on the next refresh.
     *
     * If stale responses are noted, we must not have noted that we cannot target.
     */
    virtual void noteStaleDbVersionResponse(OperationContext* optCtx,
                                            const StaleDbRoutingVersion& staleInfo) = 0;

    virtual bool hasStaleShardResponse() = 0;

    /**
     * Informs the targeter of CannotImplicitlyCreateCollection responses for this collection from
     * an endpoint, with further information available in the returned createInfo.
     *
     * Any cannotImplicitlyCreateCollection errors noted here will be taken into account on the next
     * create.
     */
    virtual void noteCannotImplicitlyCreateCollectionResponse(
        OperationContext* optCtx, const CannotImplicitlyCreateCollectionInfo& createInfo) = 0;

    /**
     * Refreshes the targeting metadata for the namespace if needed, based on previously-noted
     * stale responses and targeting failures.
     *
     * After this function is called, the targeter should be in a state such that the noted
     * stale responses are not seen again and if a targeting failure occurred it reloaded -
     * it should try to make progress.
     *
     * Returns if the targeting used here was changed.
     *
     * NOTE: This function may block for shared resources or network calls.
     */
    virtual bool refreshIfNeeded(OperationContext* opCtx) = 0;

    /**
     * Creates a collection if there were previously noted cannotImplicitlyCreateCollection
     * failures.
     *
     * After this function is called, the targeter should be in a state such that the noted
     * cannotImplicitlyCreateCollection responses are not seen again.
     *
     * Returns if the collection was created.
     *
     * NOTE: This function may block for shared resources or network calls.
     */
    virtual bool createCollectionIfNeeded(OperationContext* opCtx) = 0;

    /**
     * Returns the number of shards that own one or more chunks for the targeted collection.
     *
     * To be only used for logging/metrics which do not need to be always correct. The returned
     * value may be incorrect when this targeter is at point-in-time (it will reflect the 'latest'
     * number of shards, rather than the one at the point-in-time).
     */
    virtual int getAproxNShardsOwningChunks() const = 0;

    /**
     * Returns whether the targeted collection is sharded or not
     */
    virtual bool isTargetedCollectionSharded() const = 0;
};

}  // namespace mongo
