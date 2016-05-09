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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#pragma once

#include "mongo/s/balancer/balancer_chunk_selection_policy.h"
#include "mongo/s/sharding_uptime_reporter.h"
#include "mongo/stdx/thread.h"

namespace mongo {

class ChunkType;
class ClusterStatistics;
class MigrationSecondaryThrottleOptions;
class OperationContext;
template <typename T>
class StatusWith;

/**
 * The balancer is a background task that tries to keep the number of chunks across all
 * servers of the cluster even. Although every mongos will have one balancer running, only one
 * of them will be active at the any given point in time. The balancer uses a distributed lock
 * for that coordination.
 *
 * The balancer does act continuously but in "rounds". At a given round, it would decide if
 * there is an imbalance by checking the difference in chunks between the most and least
 * loaded shards. It would issue a request for a chunk migration per round, if it found so.
 */
class Balancer {
public:
    Balancer();
    ~Balancer();

    /**
     * Retrieves the per-service instance of the Balancer.
     */
    static Balancer* get(OperationContext* operationContext);

    /**
     * Starts the main balancer loop and returns immediately without waiting for initialization.
     * This method may only be called once for the lifetime of the balancer.
     */
    void start(OperationContext* txn);

    /**
     * Blocking call, which requests the balancer to move a single chunk to a more appropriate
     * shard, in accordance with the active balancer policy. It is not guaranteed that the chunk
     * will actually move because it may already be at the best shard. An error will be returned if
     * the attempt to find a better shard or the actual migration fail for any reason.
     */
    Status rebalanceSingleChunk(OperationContext* txn, const ChunkType& chunk);

    /**
     * Blocking call, which requests the balancer to move a single chunk to the specified location
     * in accordance with the active balancer policy. An error will be returned if the attempt to
     * move fails for any reason.
     *
     * NOTE: This call disregards the balancer enabled/disabled status and will proceed with the
     *       move regardless. If should be used only for user-initiated moves.
     */
    Status moveSingleChunk(OperationContext* txn,
                           const ChunkType& chunk,
                           const ShardId& newShardId,
                           uint64_t maxChunkSizeBytes,
                           const MigrationSecondaryThrottleOptions& secondaryThrottle,
                           bool waitForDelete);

private:
    /**
     * The main balancer loop, which runs in a separate thread.
     */
    void _mainThread();

    /**
     * Checks that the balancer can connect to all servers it needs to do its job.
     *
     * @return true if balancing can be started
     *
     * This method throws on a network exception
     */
    bool _init(OperationContext* txn);

    /**
     * Returns true if all the servers listed in configdb as being shards are reachable and are
     * distinct processes (no hostname mixup).
     */
    bool _checkOIDs(OperationContext* txn);

    /**
     * Iterates through all chunks in all collections and ensures that no chunks straddle tag
     * boundary. If any do, they will be split.
     */
    Status _enforceTagRanges(OperationContext* txn);

    /**
     * Issues chunk migration request, one at a time.
     *
     * @param candidateChunks possible chunks to move
     * @param writeConcern detailed write concern. NULL means the default write concern.
     * @param waitForDelete wait for deletes to complete after each chunk move
     * @return number of chunks effectively moved
     */
    int _moveChunks(OperationContext* txn,
                    const BalancerChunkSelectionPolicy::MigrateInfoVector& candidateChunks,
                    const MigrationSecondaryThrottleOptions& secondaryThrottle,
                    bool waitForDelete);

    // The uptime reporter associated with this instance
    const ShardingUptimeReporter _stardingUptimeReporter;

    // The main balancer thread
    stdx::thread _thread;

    // Number of moved chunks in last round
    int _balancedLastTime;

    // Balancer policy
    std::unique_ptr<BalancerChunkSelectionPolicy> _chunkSelectionPolicy;

    // Source for cluster statistics
    std::unique_ptr<ClusterStatistics> _clusterStats;
};

}  // namespace mongo
