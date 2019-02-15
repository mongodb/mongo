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
#include "mongo/db/operation_context.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/decorable.h"

namespace mongo {

/**
 * A decorable container for state associated with an active session running on a MongoD or MongoS
 * server. Refer to SessionCatalog for more information on the semantics of sessions.
 */
class Session : public Decorable<Session> {
    MONGO_DISALLOW_COPYING(Session);

    friend class ObservableSession;
    friend class SessionCatalog;

public:
    explicit Session(LogicalSessionId sessionId) : _sessionId(std::move(sessionId)) {}

    /**
     * The logical session id that this object represents.
     */
    const LogicalSessionId& getSessionId() const {
        return _sessionId;
    }

    OperationContext* currentOperation_forTest() const {
        return _checkoutOpCtx;
    }

private:
    // The id of the session with which this object is associated
    const LogicalSessionId _sessionId;

    // A pointer back to the currently running operation on this Session, or nullptr if there
    // is no operation currently running for the Session.
    //
    // This field is only safe to read or write while holding the SessionCatalog::_mutex. In
    // practice, it is only used inside of the SessionCatalog itself.
    OperationContext* _checkoutOpCtx{nullptr};

    // Counter indicating the number of times ObservableSession::kill has been called on this
    // session, which have not yet had a corresponding call to checkOutSessionForKill.
    int _killsRequested{0};
};

}  // namespace mongo
