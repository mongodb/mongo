/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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
#include "mongo/s/catalog/type_collection.h"

namespace mongo {

class ChunkType;
class NamespaceString;
class OperationContext;
template <typename T>
class StatusWith;

class BalancerCommandsScheduler;

/**
 * Interface used by the balancer for merging chunks.
 */
class BalancerChunkMerger {
    BalancerChunkMerger(const BalancerChunkMerger&) = delete;
    BalancerChunkMerger& operator=(const BalancerChunkMerger&) = delete;

public:
    virtual ~BalancerChunkMerger() = default;

    enum class Progress { NotDone, Done };

    struct Params {
        float shardImbalanceThreshold = 0.1f;
        float moveChunkThreshold = 0.2f;
        float splitChunkThreshold = 1.25f;
        float mergeChunkThreshold = 0.75f;
        float estimateChunkThreshold = 0.85f;

        int maxMergesOnShardsAtLessCollectionVersion = 1;
        int maxMergesOnShardsAtCollectionVersion = 10;
    };

    virtual void waitForStop() = 0;
    virtual void onStepDown() = 0;

    virtual bool isMergeCandidate(OperationContext* opCtx, NamespaceString const& nss) const = 0;

    virtual std::vector<CollectionType> selectCollections(OperationContext* opCtx) = 0;

    /**
     * Schedule chunk data size estimation requests and merge chunk requests.
     * Implementations may run concurrent operations on all shards.
     */
    virtual StatusWith<Progress> mergeChunksOnShards(OperationContext* opCtx,
                                                     CollectionType const&) = 0;

    /**
     * Schedule chunk move & merge requests
     */
    virtual StatusWith<Progress> moveMergeOrSplitChunks(OperationContext* opCtx,
                                                        CollectionType const&) = 0;

    virtual bool isConverged(OperationContext* opCtx, CollectionType const&) = 0;

protected:
    BalancerChunkMerger(){};

    Params _params;
};

}  // namespace mongo
