/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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
#include "mongo/db/periodic_runner_cache_pressure_rollback.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/admission/execution_control_parameters_gen.h"
#include "mongo/db/client.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/kill_sessions_local.h"
#include "mongo/db/session/session_catalog.h"
#include "mongo/db/stats/single_transaction_stats.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/db/transaction/transaction_participant_gen.h"
#include "mongo/idl/mutable_observer_registry.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/duration.h"
#include "mongo/util/periodic_runner.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/time_support.h"

#include <string>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo {

namespace {

using Argument =
    decltype(TransactionParticipant::observeCachePressureQueryPeriodMilliseconds)::Argument;

bool underCachePressure(OperationContext* opCtx) {
    auto ticketMgr = admission::TicketHolderManager::get(opCtx->getServiceContext());
    auto writeTicketHolder = ticketMgr->getTicketHolder(MODE_IX);
    auto readTicketHolder = ticketMgr->getTicketHolder(MODE_IS);

    bool rollbackCachePressure = opCtx->getServiceContext()->getStorageEngine()->underCachePressure(
        writeTicketHolder->used(), readTicketHolder->used());
    return rollbackCachePressure;
}

const auto periodicThreadToRollbackUnderCachePressureDecoration =
    ServiceContext::declareDecoration<PeriodicThreadToRollbackUnderCachePressure>();

}  // namespace

// Tracks the number of passes the "rollbackUnderCachePressure" thread makes to abort oldest
// transactions.
auto& rollbackUnderCachePressurePasses =
    *MetricBuilder<Counter64>("rollbackUnderCachePressure.passes");
// Tracks the number of transactions the "rollbackUnderCachePressure" thread successfully killed.
auto& rollbackUnderCachePressureSuccessfulKills =
    *MetricBuilder<Counter64>("rollbackUnderCachePressure.successfulKills");
// Tracks the number of sessions the "rollbackUnderCachePressure" thread has skipped.
auto& rollbackUnderCachePressureSkippedSessions =
    *MetricBuilder<Counter64>("rollbackUnderCachePressure.skippedSessions");
// Tracks the number of transactions unsuccessfully killed by the "rollbackUnderCachePressure"
// thread due to timing out trying to checkout a sessions.
auto& rollbackUnderCachePressureTimedOutKills =
    *MetricBuilder<Counter64>("rollbackUnderCachePressure.timedOutKills");

auto PeriodicThreadToRollbackUnderCachePressure::get(ServiceContext* serviceContext)
    -> PeriodicThreadToRollbackUnderCachePressure& {
    auto& jobContainer = periodicThreadToRollbackUnderCachePressureDecoration(serviceContext);
    jobContainer._init(serviceContext);
    return jobContainer;
}
auto PeriodicThreadToRollbackUnderCachePressure::operator*() const noexcept -> PeriodicJobAnchor& {
    stdx::lock_guard lk(_mutex);
    return *_anchor;
}

auto PeriodicThreadToRollbackUnderCachePressure::operator->() const noexcept -> PeriodicJobAnchor* {
    stdx::lock_guard lk(_mutex);
    return _anchor.get();
}

void PeriodicThreadToRollbackUnderCachePressure::_init(ServiceContext* serviceContext) {
    stdx::lock_guard lk(_mutex);
    if (_anchor) {
        return;
    }

    auto periodicRunner = serviceContext->getPeriodicRunner();
    invariant(periodicRunner);
    std::shared_ptr<PeriodicJobAnchor> tempAnchor;

    PeriodicRunner::PeriodicJob job(
        "rollbackUnderCachePressure",
        [](Client* client) {
            if (!feature_flags::gStorageEngineInterruptibility.isEnabled()) {
                return;
            }
            try {
                // The opCtx destructor handles unsetting itself from the Client. (The
                // PeriodicRunner's Client must be reset before returning.)
                auto uniqueOpCtx = client->makeOperationContext();
                OperationContext* opCtx = uniqueOpCtx.get();

                // Set the Locker such that all lock requests' timeouts will be overridden and set
                // to 0. This prevents the oldest transaction aborter thread from stalling behind
                // any non-transaction, exclusive lock taking operation blocked behind an active
                // transaction's intent lock.
                shard_role_details::getLocker(opCtx)->setMaxLockTimeout(Milliseconds(0));

                // This thread needs storage rollback to complete timely, so instruct the storage
                // engine to not do any extra eviction for this thread, if supported.
                shard_role_details::getRecoveryUnit(opCtx)->setNoEvictionAfterCommitOrRollback();


                int64_t killsTarget = gCachePressureAbortSessionKillLimitPerBatch;
                while (killsTarget > 0) {
                    if (!underCachePressure(opCtx)) {
                        break;
                    }

                    int64_t numKills = 0;
                    int64_t numSkips = 0;
                    int64_t numTimeOuts = 0;
                    killOldestTransaction(
                        opCtx,
                        Milliseconds(gCachePressureAbortCheckoutTimeoutMilliseconds),
                        &numKills,
                        &numSkips,
                        &numTimeOuts);
                    rollbackUnderCachePressurePasses.increment(1);
                    rollbackUnderCachePressureSuccessfulKills.increment(numKills);
                    rollbackUnderCachePressureSkippedSessions.increment(numSkips);
                    rollbackUnderCachePressureTimedOutKills.increment(numTimeOuts);

                    killsTarget -= numKills;
                }
            } catch (ExceptionFor<ErrorCategory::CancellationError>& ex) {
                LOGV2_DEBUG(10036701, 2, "Periodic job canceled", "reason"_attr = ex.reason());
            } catch (ExceptionFor<ErrorCategory::Interruption>& ex) {
                LOGV2_DEBUG(10036702, 2, "Periodic job interrupted", "reason"_attr = ex.reason());
            }
        },
        Milliseconds(gCachePressureQueryPeriodMilliseconds.load()),
        true /*isKillableByStepdown*/);

    tempAnchor = std::make_shared<PeriodicJobAnchor>(periodicRunner->makeJob(std::move(job)));

    TransactionParticipant::observeCachePressureQueryPeriodMilliseconds.addObserver(
        [anchor = tempAnchor](const Argument& millis) {
            try {
                if (millis == 0) {
                    anchor->stop();
                } else if (anchor->getPeriod() == Milliseconds(0)) {
                    anchor->start();
                }
                anchor->setPeriod(Milliseconds(millis));
            } catch (const DBException& ex) {
                LOGV2(10036703,
                      "Failed to update period of thread which rollbacks under cache pressure ",
                      "reason"_attr = ex.toStatus());
            }
        });
    _anchor = std::move(tempAnchor);
}

}  // namespace mongo
