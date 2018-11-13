
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

#include "mongo/db/session.h"

#include "mongo/db/operation_context.h"

namespace mongo {

Session::Session(LogicalSessionId sessionId) : _sessionId(std::move(sessionId)) {}

OperationContext* Session::currentOperation() const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _checkoutOpCtx;
}

Session::KillToken Session::kill(WithLock sessionCatalogLock, ErrorCodes::Error reason) {
    stdx::lock_guard<stdx::mutex> lg(_mutex);
    uassert(ErrorCodes::ConflictingOperationInProgress,
            str::stream() << "Session " << getSessionId().getId()
                          << " is already killed and is in the process of being cleaned up",
            !_killRequested);
    _killRequested = true;

    // For currently checked-out sessions, interrupt the operation context so that the current owner
    // can release the session
    if (_checkoutOpCtx) {
        const auto serviceContext = _checkoutOpCtx->getServiceContext();

        stdx::lock_guard<Client> clientLock(*_checkoutOpCtx->getClient());
        serviceContext->killOperation(_checkoutOpCtx, reason);
    }

    return KillToken(getSessionId());
}

bool Session::killed() const {
    stdx::lock_guard<stdx::mutex> lg(_mutex);
    return _killRequested;
}

void Session::_markCheckedOut(WithLock sessionCatalogLock, OperationContext* checkoutOpCtx) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    invariant(!_checkoutOpCtx);
    _checkoutOpCtx = checkoutOpCtx;
}

void Session::_markCheckedIn(WithLock sessionCatalogLock) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    invariant(_checkoutOpCtx);
    _checkoutOpCtx = nullptr;
}

void Session::_markNotKilled(WithLock sessionCatalogLock, KillToken killToken) {
    stdx::lock_guard<stdx::mutex> lg(_mutex);
    invariant(_killRequested);
    _killRequested = false;
}

}  // namespace mongo
