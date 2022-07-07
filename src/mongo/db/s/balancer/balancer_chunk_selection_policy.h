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

#include <boost/optional.hpp>
#include <vector>

#include "mongo/db/s/balancer/balancer_policy.h"
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
    BalancerChunkSelectionPolicy(const BalancerChunkSelectionPolicy&) = delete;
    BalancerChunkSelectionPolicy& operator=(const BalancerChunkSelectionPolicy&) = delete;

public:
    virtual ~BalancerChunkSelectionPolicy();

    /**
     * Potentially blocking method, which gives out a set of chunks, which need to be split because
     * they violate the policy for some reason. The reason is decided by the policy and may include
     * chunk is too big or chunk straddles a zone range.
     */
    virtual StatusWith<SplitInfoVector> selectChunksToSplit(OperationContext* opCtx) = 0;

    /**
     * Given a valid namespace returns all the splits the balancer would need to perform
     * with the current state
     */
    virtual StatusWith<SplitInfoVector> selectChunksToSplit(OperationContext* opCtx,
                                                            const NamespaceString& nss) = 0;

    /**
     * Potentially blocking method, which gives out a set of chunks to be moved.
     */
    virtual StatusWith<MigrateInfoVector> selectChunksToMove(
        OperationContext* opCtx, stdx::unordered_set<ShardId>* unavailableShards) = 0;

    /**
     * Given a valid namespace returns all the Migrations the balancer would need to perform
     * with the current state
     */
    virtual StatusWith<MigrateInfosWithReason> selectChunksToMove(OperationContext* opCtx,
                                                                  const NamespaceString& nss) = 0;

    /**
     * Requests a single chunk to be relocated to a different shard, if possible. If some error
     * occurs while trying to determine the best location for the chunk, a failed status is
     * returned. If the chunk is already at the best shard that it can be, returns boost::none.
     * Otherwise returns migration information for where the chunk should be moved.
     */
    virtual StatusWith<boost::optional<MigrateInfo>> selectSpecificChunkToMove(
        OperationContext* opCtx, const NamespaceString& nss, const ChunkType& chunk) = 0;

    /**
     * Asks the chunk selection policy to validate that the specified chunk migration is allowed
     * given the current rules. Returns OK if the migration won't violate any rules or any other
     * failed status otherwise.
     */
    virtual Status checkMoveAllowed(OperationContext* opCtx,
                                    const ChunkType& chunk,
                                    const ShardId& newShardId) = 0;

protected:
    BalancerChunkSelectionPolicy();
};

}  // namespace mongo
