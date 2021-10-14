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

#include <atomic>
#include <boost/optional.hpp>
#include <vector>

#include "mongo/db/s/balancer/balancer_chunk_merger.h"
#include "mongo/db/s/balancer/balancer_commands_scheduler.h"

namespace mongo {

class BalancerChunkMergerImpl final : public BalancerChunkMerger {
public:
    BalancerChunkMergerImpl(BalancerCommandsScheduler& scheduler, ClusterStatistics& stats);
    ~BalancerChunkMergerImpl();

    void waitForStop() override;
    void onStepDown() override;

    bool isMergeCandidate(OperationContext* opCtx, NamespaceString const& nss) const override;

    std::vector<CollectionType> selectCollections(OperationContext* opCtx) override;

    /**
     *  Schedule chunk data size estimation requests + merge chunk requests.
     *      (1) Up to `maxMergesOnShardsAtLessCollectionVersion` concurrent mergeChunks
     *      across all shards which are below the collection major version
     *          AND
     *      (2) Up to `maxMergesOnShardsAtCollectionVersion` concurrent mergeChunks across all
     *      shards which are already on the collection major version
     *
     *  Merges due to (1) will bring the respective shard's major version to that of the collection,
     *  which unfortunately is interpreted by the routers as "something routing-related changed" and
     *  will result in refresh and a stall on the critical CRUD path. Because of this, the script
     *  only runs one at a time of these by default. On the other hand, merges due to (2) only
     *  increment the minor version and will not cause stalls on the CRUD path, so these can run
     * with higher concurrency.
     */
    StatusWith<Progress> mergeChunksOnShards(OperationContext* opCtx,
                                             CollectionType const&) override;

    /**
     * Schedule chunk move / merge + split requests
     */
    StatusWith<Progress> moveMergeOrSplitChunks(OperationContext* opCtx,
                                                CollectionType const&) override;

    bool isConverged(OperationContext* opCtx, CollectionType const&) override;

private:
    BalancerCommandsScheduler& _cmdScheduler;
    ClusterStatistics& _clusterStats;
};

}  // namespace mongo
