// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/s/primary_only_service_helpers/operation_session_tracker.h"

#include "mongo/db/session/internal_session_pool.h"

namespace mongo {

namespace {
void assertSessionValid(const OperationSessionInfo& osi) {
    tassert(12098600, "Session missing txn number", osi.getTxnNumber().has_value());
    tassert(12098601, "Session missing session id", osi.getSessionId().has_value());
}

InternalSessionPool::Session osiToPoolSession(const OperationSessionInfo& osi) {
    assertSessionValid(osi);
    return InternalSessionPool::Session(*osi.getSessionId(), *osi.getTxnNumber());
}

OperationSessionInfo poolSessionToOsi(const InternalSessionPool::Session& session) {
    OperationSessionInfo osi;
    osi.setSessionId(session.getSessionId());
    osi.setTxnNumber(session.getTxnNumber());
    return osi;
}

OperationSessionInfo calculateNextSession(
    OperationContext* opCtx, const boost::optional<OperationSessionInfo>& currentSession) {
    if (currentSession) {
        auto nextSession = *currentSession;
        assertSessionValid(nextSession);
        nextSession.setTxnNumber(*nextSession.getTxnNumber() + 1);
        return nextSession;
    } else {
        return poolSessionToOsi(InternalSessionPool::get(opCtx)->acquireSystemSession());
    }
}
}  // namespace

OperationSessionTracker::OperationSessionTracker(OperationSessionPersistence* persistence)
    : _persistence(persistence) {}

OperationSessionInfo OperationSessionTracker::getNextSession(OperationContext* opCtx) {
    auto currentSession = _persistence->readSession(opCtx);
    auto nextSession = calculateNextSession(opCtx, currentSession);
    _persistence->writeSession(opCtx, nextSession);
    return nextSession;
}

boost::optional<OperationSessionInfo> OperationSessionTracker::getCurrentSession(
    OperationContext* opCtx) const {
    return _persistence->readSession(opCtx);
}

void OperationSessionTracker::releaseSession(OperationContext* opCtx) {
    auto session = _persistence->readSession(opCtx);
    if (!session) {
        return;
    }
    // Note that it is possible to leak the session from the pool if we throw after persisting. This
    // is not harmful, as the session will eventually time out and get reaped. The alternative of
    // possibly releasing the session twice is harmful, as two operations could then end up using
    // the same session.
    _persistence->writeSession(opCtx, boost::none);
    InternalSessionPool::get(opCtx)->release(osiToPoolSession(*session));
}

void OperationSessionTracker::performCausalityBarrier(OperationContext* opCtx,
                                                      CausalityBarrier& barrier) {
    auto session = getNextSession(opCtx);
    barrier.perform(opCtx, session);
}

}  // namespace mongo
