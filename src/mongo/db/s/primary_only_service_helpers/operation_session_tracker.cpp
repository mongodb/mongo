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
