/**
 *    Copyright (C) 2016 MongoDB Inc.
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

#include <boost/optional.hpp>
#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/s/balancer/balancer_policy.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/chunk_version.h"

namespace mongo {

class ChunkType;
class NamespaceString;
class OperationContext;
template <typename T>
class StatusWith;

/**
 * Interface used by the balancer for selecting chunks, which need to be moved around in order for
 * the sharded cluster to be balanced. It is up to the implementation to decide what exactly
 * 'balanced' means.
 */
class BalancerChunkSelectionPolicy {
    MONGO_DISALLOW_COPYING(BalancerChunkSelectionPolicy);

public:
    /**
     * Describes a chunk which needs to be split, because it violates the balancer policy.
     */
    struct SplitInfo {
        SplitInfo(ShardId shardId,
                  NamespaceString nss,
                  ChunkVersion collectionVersion,
                  ChunkVersion chunkVersion,
                  const BSONObj& minKey,
                  const BSONObj& maxKey,
                  std::vector<BSONObj> splitKeys);

        std::string toString() const;

        ShardId shardId;
        NamespaceString nss;
        ChunkVersion collectionVersion;
        ChunkVersion chunkVersion;
        BSONObj minKey;
        BSONObj maxKey;
        std::vector<BSONObj> splitKeys;
    };

    typedef std::vector<SplitInfo> SplitInfoVector;

    typedef std::vector<MigrateInfo> MigrateInfoVector;

    virtual ~BalancerChunkSelectionPolicy();

    /**
     * Potentially blocking method, which gives out a set of chunks, which need to be split because
     * they violate the policy for some reason. The reason is decided by the policy and may include
     * chunk is too big or chunk straddles a tag range.
     */
    virtual StatusWith<SplitInfoVector> selectChunksToSplit(OperationContext* txn) = 0;

    /**
     * Potentially blocking method, which gives out a set of chunks to be moved. The
     * aggressiveBalanceHint indicates to the balancing logic that it should lower the threshold for
     * difference in number of chunks across shards and thus potentially cause more chunks to move.
     */
    virtual StatusWith<MigrateInfoVector> selectChunksToMove(OperationContext* txn,
                                                             bool aggressiveBalanceHint) = 0;

    /**
     * Requests a single chunk to be relocated to a different shard, if possible. If some error
     * occurs while trying to determine the best location for the chunk, a failed status is
     * returned. If the chunk is already at the best shard that it can be, returns boost::none.
     * Otherwise returns migration information for where the chunk should be moved.
     */
    virtual StatusWith<boost::optional<MigrateInfo>> selectSpecificChunkToMove(
        OperationContext* txn, const ChunkType& chunk) = 0;

    /**
     * Asks the chunk selection policy to validate that the specified chunk migration is allowed
     * given the current rules. Returns OK if the migration won't violate any rules or any other
     * failed status otherwise.
     */
    virtual Status checkMoveAllowed(OperationContext* txn,
                                    const ChunkType& chunk,
                                    const ShardId& newShardId) = 0;

protected:
    BalancerChunkSelectionPolicy();
};

}  // namespace mongo
