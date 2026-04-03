/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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
class MONGO_MOD_OPEN ReclaimedPreparedTxnTracker {
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
    mutable stdx::mutex _mutex;
    bool _discoveryStarted = false;
    bool _discoveryComplete = false;
    long long _remainingToTrack = 0;
    long long _recoveryDurationMicros = 0;
    Timer _recoveryTimer;
};

}  // namespace mongo
