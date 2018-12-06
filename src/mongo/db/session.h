
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

#include "mongo/base/disallow_copying.h"
#include "mongo/db/logical_session_id.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/decorable.h"

namespace mongo {

class OperationContext;

/**
 * A decorable container for state associated with an active session running on a MongoD or MongoS
 * server. Refer to SessionCatalog for more information on the semantics of sessions.
 */
class Session : public Decorable<Session> {
    MONGO_DISALLOW_COPYING(Session);

    friend class SessionCatalog;

public:
    explicit Session(LogicalSessionId sessionId);

    /**
     * The logical session id that this object represents.
     */
    const LogicalSessionId& getSessionId() const {
        return _sessionId;
    }

    /**
     * Returns a pointer to the current operation running on this Session, or nullptr if there is no
     * operation currently running on this Session.
     */
    OperationContext* currentOperation() const;

    /**
     * Increments the number of "killers" for this session and returns a 'kill token' to to be
     * passed later on to 'checkOutSessionForKill' method of the SessionCatalog in order to permit
     * the caller to execute any kill cleanup tasks. This token is later on passed to
     * '_markNotKilled' in order to decrement the number of "killers".
     *
     * Marking session as killed is an internal property only that will cause any further calls to
     * 'checkOutSession' to block until 'checkOutSessionForKill' is called the same number of times
     * as 'kill' was called and the returned scoped object destroyed.
     *
     * If the first killer finds the session checked-out, this method will also interrupt the
     * operation context which has it checked-out.
     *
     * Must be called under the owning SessionCatalog's lock.
     */
    struct KillToken {
        KillToken(LogicalSessionId lsid) : lsidToKill(std::move(lsid)) {}
        KillToken(KillToken&&) = default;
        KillToken& operator=(KillToken&&) = default;

        LogicalSessionId lsidToKill;
    };
    KillToken kill(WithLock sessionCatalogLock, ErrorCodes::Error reason = ErrorCodes::Interrupted);

    /**
     * Returns whether 'kill' has been called on this session.
     */
    bool killed() const;

private:
    /**
     * Set/clear the current check-out state of the session by storing the operation which has this
     * session currently checked-out.
     *
     * Must be called under the SessionCatalog mutex and internally will acquire the Session mutex.
     */
    void _markCheckedOut(WithLock sessionCatalogLock, OperationContext* checkoutOpCtx);
    void _markCheckedIn(WithLock sessionCatalogLock);

    /**
     * Used by the session catalog when checking a session back in after a call to 'kill'. See the
     * comments for 'kill for more details.
     */
    void _markNotKilled(WithLock sessionCatalogLock, KillToken killToken);

    // The id of the session with which this object is associated
    const LogicalSessionId _sessionId;

    // Protects the member variables below. The order of lock acquisition should always be:
    //
    // 1) SessionCatalog mutex (if applicable)
    // 2) Session mutex
    // 3) Any decoration mutexes and/or the currently running Client's lock
    mutable stdx::mutex _mutex;

    // A pointer back to the currently running operation on this Session, or nullptr if there
    // is no operation currently running for the Session.
    OperationContext* _checkoutOpCtx{nullptr};

    // Incremented every time 'kill' is invoked and decremented by '_markNotKilled'.
    int _killsRequested{0};
};

}  // namespace mongo
