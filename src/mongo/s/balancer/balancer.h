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

#include "mongo/base/disallow_copying.h"
#include "mongo/s/balancer/balancer_chunk_selection_policy.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/thread.h"

namespace mongo {

class ChunkType;
class ClusterStatistics;
class MigrationSecondaryThrottleOptions;
class OperationContext;
class ServiceContext;
class Status;

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
    MONGO_DISALLOW_COPYING(Balancer);

public:
    Balancer();
    ~Balancer();

    /**
     * Instantiates an instance of the balancer and installs it on the specified service context.
     * This method is not thread-safe and must be called only once when the service is starting.
     */
    static void create(ServiceContext* serviceContext);

    /**
     * Retrieves the per-service instance of the Balancer.
     */
    static Balancer* get(ServiceContext* serviceContext);
    static Balancer* get(OperationContext* operationContext);

    /**
     * Starts the main balancer thread and returns immediately. If the thread was successfully
     * started (or if it was already running), returns an OK status. Otherwise, an error is
     * returned.
     *
     * Known errors include:
     *  ConflictingOperationInProgress - if the balancer is being shut down
     */
    Status startThread(OperationContext* txn);

    /**
     * If the main balancer thread is running, requests it to stop and returns immediately without
     * waiting for it to terminate. The join method must be called afterwards in order to wait for
     * the thread to complete.
     */
    void stopThread();

    /**
     * Must always be called after stop has been called and only then. Ensures that the balancer
     * thread has terminated.
     */
    void joinThread();

    /**
     * Potentially blocking method, which will return immediately if the balancer is not running a
     * balancer round and will block until the current round completes otherwise.
     */
    void joinCurrentRound(OperationContext* txn);

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

    /**
     * Appends the runtime state of the balancer instance to the specified builder.
     */
    void report(OperationContext* txn, BSONObjBuilder* builder);

private:
    /**
     * Possible runtime states of the balancer. The comments indicate the allowed next state.
     */
    enum State {
        kStopped,   // kRunning
        kRunning,   // kStopping
        kStopping,  // kStopped
    };

    /**
     * The main balancer loop, which runs in a separate thread.
     */
    void _mainThread();

    /**
     * Checks whether the balancer main thread has been requested to stop.
     */
    bool _stopRequested();

    /**
     * Signals the beginning and end of a balancing round.
     */
    void _beginRound(OperationContext* txn);
    void _endRound(OperationContext* txn, Seconds waitTimeout);

    /**
     * Blocks the caller for the specified timeout or until the balancer condition variable is
     * signaled, whichever comes first.
     */
    void _sleepFor(OperationContext* txn, Seconds waitTimeout);

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

    /**
     * Performs a split on the chunk with min value "minKey". If the split fails, it is marked as
     * jumbo.
     */
    void _splitOrMarkJumbo(OperationContext* txn,
                           const NamespaceString& nss,
                           const BSONObj& minKey);

    // The main balancer thread
    stdx::thread _thread;

    // Protects the state below
    stdx::mutex _mutex;

    // Indicates the current state of the balancer
    State _state{kStopped};

    // Indicates whether the balancer is currently executing a balancer round
    bool _inBalancerRound{false};

    // Counts the number of balancing rounds performed since the balancer thread was first activated
    int64_t _numBalancerRounds{0};

    // Condition variable, which is signalled every time the above runtime state of the balancer
    // changes (in particular, state/balancer round and number of balancer rounds).
    stdx::condition_variable _condVar;

    // Number of moved chunks in last round
    int _balancedLastTime;

    // Balancer policy
    std::unique_ptr<BalancerChunkSelectionPolicy> _chunkSelectionPolicy;

    // Source for cluster statistics
    std::unique_ptr<ClusterStatistics> _clusterStats;
};

}  // namespace mongo
