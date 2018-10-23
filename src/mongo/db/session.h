
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

private:
    /**
     * Set/clear the current check-out state of the session by storing the operation which has this
     * session currently checked-out.
     *
     * Must be called under the SessionCatalog mutex and internally will acquire the Session mutex.
     */
    void _markCheckedOut(WithLock sessionCatalogLock, OperationContext* checkoutOpCtx);
    void _markCheckedIn(WithLock sessionCatalogLock);

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
};

}  // namespace mongo
