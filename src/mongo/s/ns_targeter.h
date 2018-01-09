/**
 *    Copyright (C) 2013 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <vector>

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/shard_id.h"

namespace mongo {

class OperationContext;

/**
 * Combines a shard and the version which that shard should be using
 */
struct ShardEndpoint {
    ShardEndpoint(const ShardId& shardName, const ChunkVersion& shardVersion)
        : shardName(shardName), shardVersion(shardVersion) {}

    ShardEndpoint(const ShardEndpoint& other)
        : shardName(other.shardName), shardVersion(other.shardVersion) {}

    ShardId shardName;
    ChunkVersion shardVersion;
};

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
 *   2. RefreshIfNeeded() to get newer targeting information
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
 * Interface must be externally synchronized if used in multiple threads, for now.
 * TODO: Determine if we should internally synchronize.
 */
class NSTargeter {
public:
    virtual ~NSTargeter() {}

    /**
     * Returns the namespace targeted.
     */
    virtual const NamespaceString& getNS() const = 0;

    /**
     * Returns a ShardEndpoint for a single document write.
     *
     * Returns !OK with message if document could not be targeted for other reasons.
     */
    virtual StatusWith<ShardEndpoint> targetInsert(OperationContext* opCtx,
                                                   const BSONObj& doc) const = 0;

    /**
     * Returns a vector of ShardEndpoints for a potentially multi-shard update.
     *
     * Returns OK and fills the endpoints; returns a status describing the error otherwise.
     */
    virtual StatusWith<std::vector<ShardEndpoint>> targetUpdate(
        OperationContext* opCtx, const write_ops::UpdateOpEntry& updateDoc) const = 0;

    /**
     * Returns a vector of ShardEndpoints for a potentially multi-shard delete.
     *
     * Returns OK and fills the endpoints; returns a status describing the error otherwise.
     */
    virtual StatusWith<std::vector<ShardEndpoint>> targetDelete(
        OperationContext* opCtx, const write_ops::DeleteOpEntry& deleteDoc) const = 0;

    /**
     * Returns a vector of ShardEndpoints for the entire collection.
     *
     * Returns !OK with message if the full collection could not be targeted.
     */
    virtual StatusWith<std::vector<ShardEndpoint>> targetCollection() const = 0;

    /**
     * Returns a vector of ShardEndpoints for all shards.
     *
     * Returns !OK with message if all shards could not be targeted.
     */
    virtual StatusWith<std::vector<ShardEndpoint>> targetAllShards(
        OperationContext* opCtx) const = 0;

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
     * If stale responses are is noted, we must not have noted that we cannot target.
     */
    virtual void noteStaleResponse(const ShardEndpoint& endpoint, const BSONObj& staleInfo) = 0;

    /**
     * Refreshes the targeting metadata for the namespace if needed, based on previously-noted
     * stale responses and targeting failures.
     *
     * After this function is called, the targeter should be in a state such that the noted
     * stale responses are not seen again and if a targeting failure occurred it reloaded -
     * it should try to make progress.  If provided, wasChanged is set to true if the targeting
     * information used here was changed.
     *
     * NOTE: This function may block for shared resources or network calls.
     * Returns !OK with message if could not refresh
     */
    virtual Status refreshIfNeeded(OperationContext* opCtx, bool* wasChanged) = 0;
};

}  // namespace mongo
