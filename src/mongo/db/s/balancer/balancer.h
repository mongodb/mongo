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

#include "mongo/db/repl/replica_set_aware_service.h"
#include "mongo/db/s/balancer/balancer_chunk_selection_policy.h"
#include "mongo/db/s/balancer/balancer_random.h"
#include "mongo/platform/mutex.h"
#include "mongo/s/request_types/balancer_collection_status_gen.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/thread.h"

namespace mongo {

class ChunkType;
class ClusterStatistics;
class BalancerCommandsScheduler;
class BalancerDefragmentationPolicy;
class MigrationSecondaryThrottleOptions;
class OperationContext;
class ServiceContext;
class Status;

/**
 * The balancer is a background task that tries to keep the number of chunks across all
 * servers of the cluster even.
 *
 * The balancer does act continuously but in "rounds". At a given round, it would decide if
 * there is an imbalance by checking the difference in chunks between the most and least
 * loaded shards. It would issue a request for a chunk migration per round, if it found so.
 */
class Balancer : public ReplicaSetAwareServiceConfigSvr<Balancer> {
    Balancer(const Balancer&) = delete;
    Balancer& operator=(const Balancer&) = delete;

public:
    /**
     * Scoped class to manage the pause/resumeBalancer requests cycle.
     * See Balancer::requestPause() for more details.
     */
    class ScopedPauseBalancerRequest {
    public:
        ~ScopedPauseBalancerRequest() {
            _balancer->_removePauseRequest();
        }

    private:
        Balancer* _balancer;

        ScopedPauseBalancerRequest(Balancer* balancer) : _balancer(balancer) {
            _balancer->_addPauseRequest();
        }

        ScopedPauseBalancerRequest(const ScopedPauseBalancerRequest&) = delete;
        ScopedPauseBalancerRequest& operator=(const ScopedPauseBalancerRequest&) = delete;

        friend class Balancer;
    };
    /**
     * Provide access to the Balancer decoration on ServiceContext.
     */
    static Balancer* get(ServiceContext* serviceContext);
    static Balancer* get(OperationContext* operationContext);

    Balancer();
    ~Balancer();

    /**
     * Invoked when the config server primary enters the 'PRIMARY' state and is invoked while the
     * caller is holding the global X lock. Kicks off the main balancer thread and returns
     * immediately. Auto-balancing (if enabled) should commence shortly, and manual migrations will
     * be processed and run.
     *
     * Must only be called if the balancer is in the stopped state (i.e., just constructed or
     * waitForBalancerToStop has been called before). Any code in this call must not try to acquire
     * any locks or to wait on operations, which acquire locks.
     */
    void initiateBalancer(OperationContext* opCtx);

    /**
     * Invoked when this node which is currently serving as a 'PRIMARY' steps down and is invoked
     * while the global X lock is held. Requests the main balancer thread to stop and returns
     * immediately without waiting for it to terminate. Once the balancer has stopped, manual
     * migrations will be rejected.
     *
     * This method might be called multiple times in succession, which is what happens as a result
     * of incomplete transition to primary so it is resilient to that.
     *
     * The waitForBalancerToStop method must be called afterwards in order to wait for the main
     * balancer thread to terminate and to allow initiateBalancer to be called again.
     */
    void interruptBalancer();

    /**
     * Invoked when a node on its way to becoming a primary finishes draining and is about to
     * acquire the global X lock in order to allow writes. Waits for the balancer thread to
     * terminate and primes the balancer so that initiateBalancer can be called.
     *
     * This must not be called while holding any locks!
     */
    void waitForBalancerToStop();

    /**
     * Potentially blocking method, which will return immediately if the balancer is not running a
     * balancer round and will block until the current round completes otherwise. If the operation
     * context's deadline is exceeded, it will throw an ExceededTimeLimit exception.
     */
    void joinCurrentRound(OperationContext* opCtx);


    /**
     * Invoked by any client requiring a temporary suspension of the balancer thread
     * (I.E. the setFCV process). The request is NOT persisted by the balancer in its config
     * document and remains active as long as the returned ScopedPauseRequest doesn't get destroyed.
     */
    ScopedPauseBalancerRequest requestPause();

    /**
     * Blocking call, which requests the balancer to move a single chunk to a more appropriate
     * shard, in accordance with the active balancer policy. It is not guaranteed that the chunk
     * will actually move because it may already be at the best shard. An error will be returned if
     * the attempt to find a better shard or the actual migration fail for any reason.
     */
    Status rebalanceSingleChunk(OperationContext* opCtx,
                                const NamespaceString& nss,
                                const ChunkType& chunk);

    /**
     * Blocking call, which requests the balancer to move a single chunk to the specified location
     * in accordance with the active balancer policy. An error will be returned if the attempt to
     * move fails for any reason.
     *
     * NOTE: This call disregards the balancer enabled/disabled status and will proceed with the
     *       move regardless. If should be used only for user-initiated moves.
     */
    Status moveSingleChunk(OperationContext* opCtx,
                           const NamespaceString& nss,
                           const ChunkType& chunk,
                           const ShardId& newShardId,
                           const MigrationSecondaryThrottleOptions& secondaryThrottle,
                           bool waitForDelete,
                           bool forceJumbo);

    /**
     * Appends the runtime state of the balancer instance to the specified builder.
     */
    void report(OperationContext* opCtx, BSONObjBuilder* builder);

    /**
     * Informs the balancer that a setting that affects it changed.
     */
    void notifyPersistedBalancerSettingsChanged();

    /**
     * Informs the balancer that the user has requested defragmentation to be stopped on a
     * collection.
     */
    void abortCollectionDefragmentation(OperationContext* opCtx, const NamespaceString& nss);

    /**
     * Returns if a given collection is draining due to a removed shard, has chunks on an invalid
     * zone or the number of chunks is imbalanced across the cluster
     */
    BalancerCollectionStatusResponse getBalancerStatusForNs(OperationContext* opCtx,
                                                            const NamespaceString& nss);

private:
    /**
     * Possible runtime states of the balancer. The comments indicate the allowed next state.
     */
    enum State {
        kStopped,   // kRunning
        kRunning,   // kStopping | kStopped
        kStopping,  // kStopped
    };

    /**
     * ReplicaSetAwareService entry points.
     */
    void onStartup(OperationContext* opCtx) final {}
    void onStartupRecoveryComplete(OperationContext* opCtx) final {}
    void onInitialSyncComplete(OperationContext* opCtx) final {}
    void onShutdown() final {}
    void onStepUpBegin(OperationContext* opCtx, long long term) final;
    void onStepUpComplete(OperationContext* opCtx, long long term) final;
    void onStepDown() final;
    void onBecomeArbiter() final;

    /**
     * The main balancer loop, which runs in a separate thread.
     */
    void _mainThread();

    /**
     * The secondary balancer loop, which performs merges and splits.
     */
    void _consumeActionStreamLoop();

    /**
     * Checks whether the balancer main thread has been requested to stop.
     */
    bool _stopRequested();

    /**
     * Adds a request to pause the balancer main loop.
     */
    void _addPauseRequest();

    /**
     * Removes a previously added request to pause the balancer main loop.
     */
    void _removePauseRequest();

    /**
     * Assess whether the balancer has any active pause or stop request.
     */
    bool _stopOrPauseRequested();

    /**
     * Signals the beginning and end of a balancing round.
     */
    void _beginRound(OperationContext* opCtx);
    void _endRound(OperationContext* opCtx, Milliseconds waitTimeout);

    /**
     * Blocks the caller for the specified timeout or until the balancer condition variable is
     * signaled, whichever comes first.
     */
    void _sleepFor(OperationContext* opCtx, Milliseconds waitTimeout);

    /**
     * Returns true if all the servers listed in configdb as being shards are reachable and are
     * distinct processes (no hostname mixup).
     */
    bool _checkOIDs(OperationContext* opCtx);

    /**
     * Iterates through all chunks in all collections. If the collection is the sessions collection,
     * checks if the number of chunks is greater than or equal to the configured minimum number of
     * chunks for the sessions collection (minNumChunksForSessionsCollection). If it isn't,
     * calculates split points that evenly partition the key space into N ranges (where N is
     * minNumChunksForSessionsCollection rounded up the next power of 2), and splits any chunks that
     * straddle those split points. If the collection is any other collection, splits any chunks
     * that straddle tag boundaries.
     */
    Status _splitChunksIfNeeded(OperationContext* opCtx);

    /**
     * Schedules migrations for the specified set of chunks and returns how many chunks were
     * successfully processed.
     */
    int _moveChunks(OperationContext* opCtx,
                    const MigrateInfoVector& chunksToRebalance,
                    const MigrateInfoVector& chunksToDefragment);

    // Protects the state below
    Mutex _mutex = MONGO_MAKE_LATCH("Balancer::_mutex");

    // Indicates the current state of the balancer
    State _state{kStopped};

    // The main balancer threads
    stdx::thread _thread;
    stdx::thread _actionStreamConsumerThread;
    stdx::condition_variable _joinCond;

    // The operation context of the main balancer thread. This value may only be available in the
    // kRunning state and is used to force interrupt of any blocking calls made by the balancer
    // thread.
    OperationContext* _threadOperationContext{nullptr};

    // Indicates whether the balancer is currently executing a balancer round
    bool _inBalancerRound{false};

    // Counts the number of balancing rounds performed since the balancer thread was first activated
    int64_t _numBalancerRounds{0};

    // Condition variable, which is signalled every time the above runtime state of the balancer
    // changes (in particular, state/balancer round and number of balancer rounds).
    stdx::condition_variable _condVar;

    // Number of moved chunks in last round
    int _balancedLastTime;

    // Number of active pause balancer requests
    int _numPauseRequests{0};

    // Source of randomness when metadata needs to be randomized.
    BalancerRandomSource _random;

    // Source for cluster statistics. Depends on the source of randomness above so it should be
    // created after it and destroyed before it.
    std::unique_ptr<ClusterStatistics> _clusterStats;

    // Balancer policy. Depends on the cluster statistics instance and source of randomness above so
    // it should be created after them and destroyed before them.
    std::unique_ptr<BalancerChunkSelectionPolicy> _chunkSelectionPolicy;

    std::unique_ptr<BalancerCommandsScheduler> _commandScheduler;

    std::unique_ptr<BalancerDefragmentationPolicy> _defragmentationPolicy;
};

}  // namespace mongo
