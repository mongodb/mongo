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

#include <cstdint>
#include <memory>
#include <string>

#include "mongo/base/status.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/replica_set_aware_service.h"
#include "mongo/db/s/balancer/auto_merger_policy.h"
#include "mongo/db/s/balancer/balancer_chunk_selection_policy.h"
#include "mongo/db/s/balancer/balancer_policy.h"
#include "mongo/db/s/balancer/cluster_statistics.h"
#include "mongo/db/s/balancer/move_unsharded_policy.h"
#include "mongo/db/service_context.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/mutex.h"
#include "mongo/s/request_types/balancer_collection_status_gen.h"
#include "mongo/s/request_types/move_range_request_gen.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/thread.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/duration.h"

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
     * Provide access to the Balancer decoration on ServiceContext.
     */
    static Balancer* get(ServiceContext* serviceContext);
    static Balancer* get(OperationContext* operationContext);

    Balancer();
    ~Balancer();

    /**
     * Invoked when the config server primary enters the 'PRIMARY' state and is invoked while the
     * caller is holding the global X lock. Kicks off the main balancer thread (which will in turn
     * instantiate a secondary worker and the CommandsScheduler) and returns immediately.
     * Auto-balancing (if enabled) should commence shortly, and manual migrations will be processed
     * and run.
     *
     * Must only be called if the balancer thread set is in the Terminated state (i.e., just
     * constructed or joinTermination() has been called before).
     * Any code in this call must not try to acquire any locks or to wait on operations, which
     * acquire locks.
     */
    void initiate(OperationContext* opCtx);

    /**
     * Invoked when this node which is currently serving as a 'PRIMARY' steps down and is invoked
     * while the global X lock is held. Requests to the hierarchy of balancer threads to leave and
     * returns immediately without waiting for them to terminate. (Once the termination is complete,
     * manual migrations will be rejected).
     *
     * This method might be called multiple times in succession, which is what happens as a result
     * of incomplete transition to primary so it is resilient to that.
     *
     * The joinTermination() method must be called afterwards in order to wait for the main
     * balancer thread to terminate and to allow initiateBalancer to be called again.
     */
    void requestTermination();

    /**
     * Invoked when a node on its way to becoming a primary finishes draining and is about to
     * acquire the global X lock in order to allow writes. Waits for the hierarchy of balancer
     * threads to terminate and primes the balancer so that initiateBalancer can be called.
     *
     * This must not be called while holding any locks!
     */
    void joinTermination();

    /**
     * Potentially blocking method, which will return immediately if the balancer is not running a
     * balancer round and will block until the current round completes otherwise. If the operation
     * context's deadline is exceeded, it will throw an ExceededTimeLimit exception.
     */
    void joinCurrentRound(OperationContext* opCtx);

    /**
     * Blocking call, which requests the balancer to move a range to the specified location
     * in accordance with the active balancer policy. An error will be returned if the attempt to
     * move fails for any reason.
     *
     * NOTE: This call disregards the balancer enabled/disabled status and will proceed with the
     *       move regardless.
     */
    Status moveRange(OperationContext* opCtx,
                     const NamespaceString& nss,
                     const ConfigsvrMoveRange& request,
                     bool issuedByRemoteUser);

    /**
     * Appends the runtime state of the balancer instance to the specified builder.
     */
    void report(OperationContext* opCtx, BSONObjBuilder* builder);

    /**
     * Informs the balancer that a setting that affects it changed.
     */
    void notifyPersistedBalancerSettingsChanged(OperationContext* opCtx);

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
    static constexpr int kMaxOutstandingStreamingOperations = 50;

    /**
     * Possible runtime states of the set of threads instantiated by the balancer.
     * The diagram below depicts the allowed transitions.
     * Terminated --> Running --> Terminating
     *    ^            /          /
     *    |           /          /
     *     \---------------------
     */
    enum class ThreadSetState {
        // There is no worker thread instantiated by the balancer
        Terminated,
        // The balancer is initiliasing its worker threads (or they are all already active)
        Running,
        // A request to terminate all the balancer worker threads is ongoing
        Terminating,
    };

    struct MigrationStats {
        int unshardedCollections{0};
        int rebalancedChunks{0};
        int defragmentedChunks{0};
    };

    /**
     * ReplicaSetAwareService entry points.
     */
    void onStartup(OperationContext* opCtx) final {}
    void onSetCurrentConfig(OperationContext* opCtx) final {}
    void onInitialDataAvailable(OperationContext* opCtx, bool isMajorityDataAvailable) final {}
    void onRollback() final {}
    void onShutdown() final;
    void onStepUpBegin(OperationContext* opCtx, long long term) final;
    void onStepUpComplete(OperationContext* opCtx, long long term) final;
    void onStepDown() final;
    void onBecomeArbiter() final;
    inline std::string getServiceName() const override final {
        return "Balancer";
    }

    /**
     * The main balancer loop, which runs in a separate thread.
     */
    void _mainThread();

    /**
     * The secondary balancer loop, which performs merges and splits.
     */
    void _consumeActionStreamLoop();

    /**
     * Checks whether the balancer is going through a termination sequence of its threads.
     */
    bool _terminationRequested();

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
     * Iterates through all chunks in all collections, except for the sessions collection, splitting
     * any chunks that straddle zone boundaries.
     */
    Status _splitChunksIfNeeded(OperationContext* opCtx);

    /**
     * Schedules migrations for the specified set of chunks and returns how many chunks were
     * successfully processed.
     */
    MigrationStats _doMigrations(OperationContext* opCtx,
                                 const MigrateInfoVector& unshardedToMove,
                                 const MigrateInfoVector& chunksToRebalance,
                                 const MigrateInfoVector& chunksToDefragment);

    void _onActionsStreamPolicyStateUpdate();

    /**
     * To be invoked on completion of an action requested to by an ActionStream policy to
     * update the policy state (which will generate follow-up actions based on the received
     * outcome).
     */
    void _applyStreamingActionResponseToPolicy(const BalancerStreamAction& action,
                                               const BalancerStreamActionResponse& response,
                                               ActionsStreamPolicy* policy);

    // Protects the state below
    Mutex _mutex = MONGO_MAKE_LATCH("Balancer::_mutex");

    // Indicates the current state of the worker threads instantiated by the balancer
    // (_thread, _actionStreamConsumerThread and _commandScheduler)
    ThreadSetState _threadSetState{ThreadSetState::Terminated};

    // The main balancer threads
    stdx::thread _thread;
    stdx::thread _actionStreamConsumerThread;
    stdx::condition_variable _joinCond;

    // The operation context of the main balancer thread. This value may only be available in the
    // kRunning state and is used to force interrupt of any blocking calls made by the balancer
    // thread.
    OperationContext* _threadOperationContext{nullptr};

    AtomicWord<int> _outstandingStreamingOps{0};

    AtomicWord<bool> _actionStreamsStateUpdated{true};

    // Indicates whether the balancer is currently executing a balancer round
    bool _inBalancerRound{false};

    // Counts the number of balancing rounds performed since the balancer thread was first activated
    int64_t _numBalancerRounds{0};

    // Condition variable, which is signalled every time the above runtime state of the balancer
    // changes (in particular, state/balancer round and number of balancer rounds).
    stdx::condition_variable _condVar;

    stdx::condition_variable _actionStreamCondVar;

    // Number of migrations in last round
    MigrationStats _balancedLastTime;

    // Source for cluster statistics. Depends on the source of randomness above so it should be
    // created after it and destroyed before it.
    std::unique_ptr<ClusterStatistics> _clusterStats;

    // Balancer policy. Depends on the cluster statistics instance and source of randomness above so
    // it should be created after them and destroyed before them.
    std::unique_ptr<BalancerChunkSelectionPolicy> _chunkSelectionPolicy;

    std::unique_ptr<BalancerCommandsScheduler> _commandScheduler;

    std::unique_ptr<BalancerDefragmentationPolicy> _defragmentationPolicy;

    std::unique_ptr<AutoMergerPolicy> _autoMergerPolicy;

    std::unique_ptr<MoveUnshardedPolicy> _moveUnshardedPolicy;

    stdx::unordered_set<NamespaceString> _imbalancedCollectionsCache;
};

}  // namespace mongo
