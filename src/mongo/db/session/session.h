// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/decorable.h"
#include "mongo/util/modules.h"

namespace mongo {

/**
 * A decorable container for state associated with an active transaction session running on a MongoD
 * or MongoS server. Refer to SessionCatalog for more information on the semantics of sessions.
 */
class [[MONGO_MOD_PUBLIC]] Session : public Decorable<Session> {
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    friend class ObservableSession;
    friend class SessionCatalog;

public:
    explicit Session(LogicalSessionId sessionId) : _sessionId(std::move(sessionId)) {}

    ~Session() override {
        invariant(!_numWaitingToCheckOut);
    }

    const LogicalSessionId& getSessionId() const {
        return _sessionId;
    }

    Session* getParentSession() const {
        return _parentSession;
    }

private:
    // The session id of the transaction session that this object represents.
    const LogicalSessionId _sessionId;

    // A pointer to the parent Session for this Session if there is one. Set at construction for
    // child sessions. Children and parents are reaped atomically, so this pointer should always be
    // valid if it is not null.
    Session* _parentSession{nullptr};

    // Counts how many threads are blocked waiting for this Session to become available. Used to
    // block reaping of this Session from the SessionCatalog.
    int _numWaitingToCheckOut{0};
};

}  // namespace mongo
