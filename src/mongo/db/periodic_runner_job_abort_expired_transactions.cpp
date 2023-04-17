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


#include "mongo/platform/basic.h"

#include "mongo/db/periodic_runner_job_abort_expired_transactions.h"

#include "mongo/db/client.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/kill_sessions_local.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/db/transaction/transaction_participant_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/util/periodic_runner.h"

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
            opCtx->lockState()->setMaxLockTimeout(Milliseconds(0));

            // This thread needs storage rollback to complete timely, so instruct the storage
            // engine to not do any extra eviction for this thread, if supported.
            opCtx->recoveryUnit()->setNoEvictionAfterRollback();

            try {
                killAllExpiredTransactions(opCtx.get());
            } catch (ExceptionForCat<ErrorCategory::CancellationError>& ex) {
                LOGV2_DEBUG(4684101, 2, "Periodic job canceled", "{reason}"_attr = ex.reason());
            }
        },
        getPeriod(gTransactionLifetimeLimitSeconds.load()),
        // TODO(SERVER-74656): Please revisit if this periodic job could be made killable.
        false /*isKillableByStepdown*/);

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
