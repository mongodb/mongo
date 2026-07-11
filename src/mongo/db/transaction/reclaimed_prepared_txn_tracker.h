// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/future_util.h"
#include "mongo/util/modules.h"
#include "mongo/util/timer.h"

namespace mongo {
/**
 * Tracks when all prepared transactions reclaimed during startup recovery from a precise checkpoint
 * have exited the prepared state (i.e. have been committed or aborted).
 *
 * Usage:
 * - Producer (startup recovery): recoverPreparedTransactionsFromPreciseCheckpoint() calls
 *   beginDiscovery(expectedCount), then calls
 * trackPrepareExit(TransactionParticipant::onExitPrepare()) once per reclaimed prepared
 * transaction, then calls discoveryComplete().
 * - Consumers: any component that needs to wait until all reclaimed prepared transactions have
 *   exited prepare can call onAllReclaimedPreparedTxnsResolved() after discoveryComplete().
 */
class [[MONGO_MOD_OPEN]] ReclaimedPreparedTxnTracker {
    ReclaimedPreparedTxnTracker(const ReclaimedPreparedTxnTracker&) = delete;
    ReclaimedPreparedTxnTracker& operator=(const ReclaimedPreparedTxnTracker&) = delete;

public:
    ReclaimedPreparedTxnTracker() = default;
    ReclaimedPreparedTxnTracker(TickSource* tickSource);
    ~ReclaimedPreparedTxnTracker() = default;

    static ReclaimedPreparedTxnTracker* get(ServiceContext* serviceContext);
    static ReclaimedPreparedTxnTracker* get(OperationContext* opCtx);

    void discoveryComplete();

    void beginDiscovery(long long expectedCount);

    SharedSemiFuture<void> onAllReclaimedPreparedTxnsResolved();

    void trackPrepareExit(SharedSemiFuture<void> onExitPrepareFuture);

    long long getNumReclaimedPreparedTxnsRemaining() const;
    long long getRecoveryDurationMicros() const;

private:
    struct State {
        mongo::Atomic<long long> unresolvedPreparedTxnsCount{0};
        SharedPromise<void> allPreparedTxnsResolved;
    };

    std::shared_ptr<State> _state;

    // Protects the below private members.
    mutable std::mutex _mutex;
    bool _discoveryStarted = false;
    bool _discoveryComplete = false;
    long long _remainingToTrack = 0;
    long long _recoveryDurationMicros = 0;
    Timer _recoveryTimer;
};

}  // namespace mongo
