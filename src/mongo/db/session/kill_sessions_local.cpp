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


#include "mongo/db/session/kill_sessions_local.h"

#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/client_cursor/cursor_manager.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/kill_sessions.h"
#include "mongo/db/session/kill_sessions_common.h"
#include "mongo/db/session/kill_sessions_gen.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/db/session/session_catalog.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/duration.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"

#include <functional>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace {

/**
 * Shortcut method shared by the various forms of session kill below. Every session kill operation
 * consists of the following stages:
 *  1) Select the sessions to kill, based on their lsid or owning user account (achieved through the
 *     'matcher') and further refining that list through the 'filterFn'.
 *  2) If any of the selected sessions are currently checked out, interrupt the owning operation
 *     context with 'reason' as the code.
 *  3) Finish killing the selected and interrupted sessions through the 'killSessionFn'.
 */
void killSessionsAction(
    OperationContext* opCtx,
    const SessionKiller::Matcher& matcher,
    const std::function<bool(const ObservableSession&)>& filterFn,
    const std::function<void(OperationContext*, const SessionToKill&)>& killSessionFn,
    ErrorCodes::Error reason = ErrorCodes::Interrupted,
    Milliseconds* timeout = nullptr,
    int64_t* numTimeOuts = nullptr) {
    const auto catalog = SessionCatalog::get(opCtx);

    std::vector<SessionCatalog::KillToken> sessionKillTokens;
    catalog->scanSessions(matcher, [&](const ObservableSession& session) {
        if (filterFn(session))
            sessionKillTokens.emplace_back(session.kill(reason));
    });

    for (auto& sessionKillToken : sessionKillTokens) {
        try {
            auto session =
                catalog->checkOutSessionForKill(opCtx, std::move(sessionKillToken), timeout);

            // TODO (SERVER-33850): Rename KillAllSessionsByPattern and
            // ScopedKillAllSessionsByPatternImpersonator to not refer to session kill
            const KillAllSessionsByPattern* pattern = matcher.match(session.getSessionId());
            invariant(pattern);

            ScopedKillAllSessionsByPatternImpersonator impersonator(opCtx, *pattern);
            killSessionFn(opCtx, session);
        } catch (const ExceptionFor<ErrorCodes::ExceededTimeLimit>&) {
            // Failed to check out the session for kill, continue killing the rest of the sessions.
            if (numTimeOuts)
                (*numTimeOuts)++;
            continue;
        }
    }
}

}  // namespace

void killSessionsAbortUnpreparedTransactions(OperationContext* opCtx,
                                             const SessionKiller::Matcher& matcher,
                                             ErrorCodes::Error reason) {
    killSessionsAction(
        opCtx,
        matcher,
        [](const ObservableSession& session) {
            auto participant = TransactionParticipant::get(session);
            return participant.transactionIsInProgress();
        },
        [](OperationContext* opCtx, const SessionToKill& session) {
            auto participant = TransactionParticipant::get(session);
            // This is the same test as in the filter, but we must check again now
            // that the session is checked out.
            if (participant.transactionIsInProgress()) {
                participant.abortTransaction(opCtx);
            }
        },
        reason);
}

SessionKiller::Result killSessionsLocal(OperationContext* opCtx,
                                        const SessionKiller::Matcher& matcher,
                                        SessionKiller::UniformRandomBitGenerator* urbg) {
    killSessionsAbortUnpreparedTransactions(opCtx, matcher);
    uassertStatusOK(killSessionsLocalKillOps(opCtx, matcher));

    auto res = CursorManager::get(opCtx)->killCursorsWithMatchingSessions(opCtx, matcher);
    uassertStatusOK(res.first);

    return {std::vector<HostAndPort>{}};
}

void killAllExpiredTransactions(OperationContext* opCtx,
                                Milliseconds timeout,
                                int64_t* numKills,
                                int64_t* numTimeOuts) {
    SessionKiller::Matcher matcherAllSessions(
        KillAllSessionsByPatternSet{makeKillAllSessionsByPattern(opCtx)});
    killSessionsAction(
        opCtx,
        matcherAllSessions,
        [when = opCtx->getServiceContext()->getPreciseClockSource()->now()](
            const ObservableSession& session) {
            return TransactionParticipant::get(session).expiredAsOf(when);
        },
        [&numKills](OperationContext* opCtx, const SessionToKill& session) {
            auto txnParticipant = TransactionParticipant::get(session);
            // If the transaction is aborted here, it means it was aborted after
            // the filter.  The most likely reason for this is that the transaction
            // was active and the session kill aborted it.  We still want to log
            // that as aborted due to transactionLifetimeLimitSessions.
            if (txnParticipant.transactionIsInProgress() || txnParticipant.transactionIsAborted()) {
                LOGV2(20707,
                      "Aborting transaction because it has been running for longer than "
                      "'transactionLifetimeLimitSeconds'",
                      "sessionId"_attr = session.getSessionId().getId(),
                      "txnNumberAndRetryCounter"_attr =
                          txnParticipant.getActiveTxnNumberAndRetryCounter());
                if (txnParticipant.transactionIsInProgress()) {
                    txnParticipant.abortTransaction(opCtx);
                }
                (*numKills)++;
            }
        },
        ErrorCodes::TransactionExceededLifetimeLimitSeconds,
        &timeout,
        numTimeOuts);
}

void killOldestTransaction(OperationContext* opCtx,
                           Milliseconds timeout,
                           int64_t* numKills,
                           int64_t* numSkips,
                           int64_t* numTimeOuts) {
    SessionKiller::Matcher matcher(
        KillAllSessionsByPatternSet{makeKillAllSessionsByPattern(opCtx)});
    boost::optional<LogicalSessionId> oldest;
    Microseconds oldestDuration;
    const auto sessionCatalog = SessionCatalog::get(opCtx);
    sessionCatalog->scanSessions(matcher, [&](const ObservableSession& session) {
        TransactionParticipant::Observer txnParticipant = TransactionParticipant::get(session);
        ScopeGuard updateSkipStats([&numSkips] { (*numSkips)++; });

        // Filter out internal sessions.
        if (session.getSessionId().getTxnUUID()) {
            return;
        }

        // Filtering out prepared transactions as we are unable to abort a prepared transaction.
        if (txnParticipant.transactionIsPrepared()) {
            return;
        }

        // Transaction aborted, or we have a session without a transaction yet.
        if (!txnParticipant.transactionIsInProgress() || txnParticipant.transactionIsAborted()) {
            return;
        }

        updateSkipStats.dismiss();

        auto currentDuration =
            txnParticipant.getDuration(opCtx->getServiceContext()->getTickSource());

        if (!oldest || currentDuration > oldestDuration) {
            oldestDuration = currentDuration;
            oldest = session.getSessionId();
        }
    });
    if (!oldest) {
        LOGV2_DEBUG(10036706, 1, "Oldest transaction was not found");
        return;
    }

    try {
        auto killToken =
            SessionCatalog::get(opCtx)->killSession(*oldest, ErrorCodes::TemporarilyUnavailable);

        auto session =
            sessionCatalog->checkOutSessionForKill(opCtx, std::move(killToken), &timeout);

        // TODO (SERVER-33850): Rename KillAllSessionsByPattern and
        // ScopedKillAllSessionsByPatternImpersonator to not refer to session kill
        const KillAllSessionsByPattern* pattern = matcher.match(session.getSessionId());
        invariant(pattern);

        ScopedKillAllSessionsByPatternImpersonator impersonator(opCtx, *pattern);
        auto txnParticipant = TransactionParticipant::get(session);
        if (txnParticipant.transactionIsInProgress() || txnParticipant.transactionIsAborted()) {
            LOGV2(10036704,
                  "Aborting oldest transaction",
                  "sessionId"_attr = session.getSessionId().getId(),
                  "txnNumberAndRetryCounter"_attr =
                      txnParticipant.getActiveTxnNumberAndRetryCounter());
            if (txnParticipant.transactionIsInProgress()) {
                txnParticipant.abortTransaction(
                    opCtx,
                    Status(ErrorCodes::TemporarilyUnavailable,
                           "Transaction aborted due to cache pressure."));
            }
            (*numKills)++;
        }
    } catch (const ExceptionFor<ErrorCodes::ExceededTimeLimit>&) {
        // Failed to check out the session for kill.
        if (numTimeOuts) {
            (*numTimeOuts)++;
        }
    } catch (const ExceptionFor<ErrorCodes::NoSuchSession>&) {
        LOGV2_DEBUG(10036708, 1, "Aborting session error - session not found.");
    }
}
void killSessionsLocalShutdownAllTransactions(OperationContext* opCtx) {
    SessionKiller::Matcher matcherAllSessions(
        KillAllSessionsByPatternSet{makeKillAllSessionsByPattern(opCtx)});
    killSessionsAction(
        opCtx,
        matcherAllSessions,
        [](const ObservableSession& session) {
            return TransactionParticipant::get(session).transactionIsOpen();
        },
        [](OperationContext* opCtx, const SessionToKill& session) {
            TransactionParticipant::get(session).shutdown(opCtx);
        },
        ErrorCodes::InterruptedAtShutdown);
}

void killSessionsAbortAllPreparedTransactions(OperationContext* opCtx) {
    SessionKiller::Matcher matcherAllSessions(
        KillAllSessionsByPatternSet{makeKillAllSessionsByPattern(opCtx)});
    killSessionsAction(
        opCtx,
        matcherAllSessions,
        [](const ObservableSession& session) {
            // Filter for sessions that have a prepared transaction.
            return TransactionParticipant::get(session).transactionIsPrepared();
        },
        [](OperationContext* opCtx, const SessionToKill& session) {
            // We're holding the RSTL, so the transaction shouldn't be otherwise
            // affected.
            invariant(TransactionParticipant::get(session).transactionIsPrepared());
            // Abort the prepared transaction and invalidate the session it is
            // associated with.
            TransactionParticipant::get(session).abortTransaction(opCtx);
            TransactionParticipant::get(session).invalidate(opCtx);
        });
}

void yieldLocksForPreparedTransactions(OperationContext* opCtx) {
    // Create a new opCtx because we need an empty locker to refresh the locks.
    //
    // When checking out sessions below, the opCtx can hold the global lock acquired by the
    // prepared transaction, making it a target by the repl killOp thread. The input opCtx is
    // already unkillable so we just mark this one also unkillable to avoid crash.
    auto newClient = opCtx->getServiceContext()
                         ->getService(ClusterRole::ShardServer)
                         ->makeClient("prepared-txns-yield-locks",
                                      Client::noSession(),
                                      ClientOperationKillableByStepdown{false});


    AlternativeClientRegion acr(newClient);
    auto newOpCtx = cc().makeOperationContext();

    // Scan the sessions again to get the list of all sessions with prepared transaction
    // to yield their locks.
    LOGV2(6015318, "Yielding locks for prepared transactions.");
    SessionKiller::Matcher matcherAllSessions(
        KillAllSessionsByPatternSet{makeKillAllSessionsByPattern(newOpCtx.get())});
    killSessionsAction(
        newOpCtx.get(),
        matcherAllSessions,
        [](const ObservableSession& session) {
            return TransactionParticipant::get(session).transactionIsPrepared();
        },
        [](OperationContext* killerOpCtx, const SessionToKill& session) {
            auto txnParticipant = TransactionParticipant::get(session);
            // Yield locks for prepared transactions. When scanning and killing
            // operations, all prepared transactions are included in the list. Even
            // though new sessions may be created after the scan, none of them can
            // become prepared during stepdown, since the RSTL has been enqueued,
            // preventing any new writes.
            if (txnParticipant.transactionIsPrepared()) {
                LOGV2_DEBUG(20708,
                            3,
                            "Yielding locks of prepared transaction",
                            "sessionId"_attr = session.getSessionId().getId(),
                            "txnNumberAndRetryCounter"_attr =
                                txnParticipant.getActiveTxnNumberAndRetryCounter());
                txnParticipant.refreshLocksForPreparedTransaction(killerOpCtx, true);
            }
        },
        ErrorCodes::InterruptedDueToReplStateChange);
}

void invalidateSessionsForStepdown(OperationContext* opCtx) {
    // It is illegal to invalidate the sessions if the operation has a session checked out.
    invariant(!OperationContextSession::get(opCtx));

    LOGV2(6015319, "Invalidating sessions for stepdown.");
    SessionKiller::Matcher matcherAllSessions(
        KillAllSessionsByPatternSet{makeKillAllSessionsByPattern(opCtx)});
    killSessionsAction(
        opCtx,
        matcherAllSessions,
        [](const ObservableSession& session) {
            return !TransactionParticipant::get(session).transactionIsPrepared();
        },
        [](OperationContext* killerOpCtx, const SessionToKill& session) {
            auto txnParticipant = TransactionParticipant::get(session);
            if (!txnParticipant.transactionIsPrepared()) {
                txnParticipant.invalidate(killerOpCtx);
            }
        },
        ErrorCodes::InterruptedDueToReplStateChange);
}

}  // namespace mongo
