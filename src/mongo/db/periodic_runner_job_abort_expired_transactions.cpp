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

#include "mongo/db/periodic_runner_job_abort_expired_transactions.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/client.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/kill_sessions_local.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/db/transaction/transaction_participant_gen.h"
#include "mongo/idl/mutable_observer_registry.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/duration.h"
#include "mongo/util/periodic_runner.h"

#include <string>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


namespace mongo {

namespace {

using Argument = decltype(TransactionParticipant::observeTransactionLifetimeLimitSeconds)::Argument;

// When setting the period for this job, we wait 500ms for every second, so that we abort
// expired transactions every transactionLifetimeLimitSeconds/2
Milliseconds getPeriod(const Argument& transactionLifetimeLimitSeconds) {
    Milliseconds period(transactionLifetimeLimitSeconds * 500);

    // Ensure: 1 <= period <= 60 seconds
    period = (period < Seconds(1)) ? Milliseconds(Seconds(1)) : period;
    period = (period > Seconds(60)) ? Milliseconds(Seconds(60)) : period;

    return period;
}

}  // namespace

// Tracks the number of passes the "abortExpiredTransactions" thread makes to abort expired
// transactions.
auto& abortExpiredTransactionsPasses = *MetricBuilder<Counter64>("abortExpiredTransactions.passes");
// Tracks the number of transactions the "abortExpiredTransactions" thread successfully killed.
auto& abortExpiredTransactionsSuccessfulKills =
    *MetricBuilder<Counter64>("abortExpiredTransactions.successfulKills");
// Tracks the number of transactions unsuccessfully killed by the "abortExpiredTransactions" thread
// due to timing out trying to checkout a sessions.
auto& abortExpiredTransactionsTimedOutKills =
    *MetricBuilder<Counter64>("abortExpiredTransactions.timedOutKills");

auto PeriodicThreadToAbortExpiredTransactions::get(ServiceContext* serviceContext)
    -> PeriodicThreadToAbortExpiredTransactions& {
    auto& jobContainer = _serviceDecoration(serviceContext);
    jobContainer._init(serviceContext);

    return jobContainer;
}

auto PeriodicThreadToAbortExpiredTransactions::operator*() const noexcept -> PeriodicJobAnchor& {
    stdx::lock_guard lk(_mutex);
    return *_anchor;
}

auto PeriodicThreadToAbortExpiredTransactions::operator->() const noexcept -> PeriodicJobAnchor* {
    stdx::lock_guard lk(_mutex);
    return _anchor.get();
}

void PeriodicThreadToAbortExpiredTransactions::_init(ServiceContext* serviceContext) {
    stdx::lock_guard lk(_mutex);
    if (_anchor) {
        return;
    }

    auto periodicRunner = serviceContext->getPeriodicRunner();
    invariant(periodicRunner);

    PeriodicRunner::PeriodicJob job(
        "abortExpiredTransactions",
        [](Client* client) {
            // The opCtx destructor handles unsetting itself from the
            // Client. (The PeriodicRunner's Client must be reset before
            // returning.)
            auto opCtx = client->makeOperationContext();

            // Set the Locker such that all lock requests' timeouts will
            // be overridden and set to 0. This prevents the expired
            // transaction aborter thread from stalling behind any
            // non-transaction, exclusive lock taking operation blocked
            // behind an active transaction's intent lock.
            shard_role_details::getLocker(opCtx.get())->setMaxLockTimeout(Milliseconds(0));

            // This thread needs storage rollback to complete timely, so instruct the storage
            // engine to not do any extra eviction for this thread, if supported.
            shard_role_details::getRecoveryUnit(opCtx.get())->setNoEvictionAfterCommitOrRollback();

            try {
                int64_t numKills = 0;
                int64_t numTimeOuts = 0;
                killAllExpiredTransactions(
                    opCtx.get(),
                    Milliseconds(gAbortExpiredTransactionsSessionCheckoutTimeout.load()),
                    &numKills,
                    &numTimeOuts);
                abortExpiredTransactionsPasses.increment(1);
                abortExpiredTransactionsSuccessfulKills.increment(numKills);
                abortExpiredTransactionsTimedOutKills.increment(numTimeOuts);
            } catch (ExceptionFor<ErrorCategory::CancellationError>& ex) {
                LOGV2_DEBUG(4684101, 2, "Periodic job canceled", "{reason}"_attr = ex.reason());
            } catch (ExceptionFor<ErrorCategory::Interruption>& ex) {
                LOGV2_DEBUG(7465601, 2, "Periodic job canceled", "{reason}"_attr = ex.reason());
            }
        },
        getPeriod(gTransactionLifetimeLimitSeconds.load()),
        true /*isKillableByStepdown*/);

    _anchor = std::make_shared<PeriodicJobAnchor>(periodicRunner->makeJob(std::move(job)));

    TransactionParticipant::observeTransactionLifetimeLimitSeconds.addObserver([anchor = _anchor](
                                                                                   const Argument&
                                                                                       secs) {
        try {
            anchor->setPeriod(getPeriod(secs));
        } catch (const DBException& ex) {
            LOGV2(
                20892,
                "Failed to update period of thread which aborts expired transactions {ex_toStatus}",
                "ex_toStatus"_attr = ex.toStatus());
        }
    });
}

}  // namespace mongo
