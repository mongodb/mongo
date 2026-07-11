// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/crypto/sha256_block.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_cache_gen.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/duration.h"
#include "mongo/util/modules.h"
#include "mongo/util/time_support.h"

#include <cstddef>
#include <list>
#include <mutex>
#include <ratio>
#include <stack>
#include <utility>

#include <boost/optional/optional.hpp>

namespace mongo {

/**
 * The InternalSessionPool creates a pool of reusable sessions, as needed, that can be used
 * to execute an internal transaction. Sessions are reaped from the pool if they have not been used
 * for over 15 minutes. The session pool is partitioned by uid.
 */
class [[MONGO_MOD_PUBLIC]] InternalSessionPool {

public:
    class Session {
        friend class InternalSessionPool;

    public:
        Session(LogicalSessionId lsid, TxnNumber txnNumber)
            : _lsid(std::move(lsid)), _txnNumber(txnNumber) {}

        const LogicalSessionId& getSessionId() const {
            return _lsid;
        }
        const TxnNumber& getTxnNumber() const {
            return _txnNumber;
        }

    private:
        bool _isExpired(Date_t now) {
            auto timeElapsed = now - _lastSeen;
            return timeElapsed > Minutes(localLogicalSessionTimeoutMinutes / 2);
        }

        LogicalSessionId _lsid;
        TxnNumber _txnNumber;
        Date_t _lastSeen;
    };

    InternalSessionPool() = default;

    static InternalSessionPool* get(ServiceContext* serviceContext);
    static InternalSessionPool* get(OperationContext* opCtx);

    /**
     * Use this method to acquire an internal session for a system initiated transaction.
     */
    Session acquireSystemSession();

    /**
     * Use this method to acquire an internal session when the user is not currently running a
     * session.
     */
    Session acquireStandaloneSession(OperationContext* opCtx);

    /**
     * Use this method to acquire an internal session nested in a client session.
     */
    Session acquireChildSession(OperationContext* opCtx, const LogicalSessionId& parentLsid);

    /**
     * Use this method to release all types of acquired sessions back to the session pool. Upon
     * release, all expired sessions are removed from the session pool.
     */
    void release(Session session);

    /**
     * Use this method to confirm the size of list at _userSessionPool[userDigest]. To be used for
     * testing only.
     */
    std::size_t numSessionsForUser_forTest(SHA256Block userDigest);

private:
    void _reapExpiredSessions(WithLock);
    boost::optional<InternalSessionPool::Session> _acquireSession(SHA256Block userDigest, WithLock);

    // Used for associating parent lsids with existing Sessions of the form <id, uid, txnUUID>.
    LogicalSessionIdMap<Session> _childSessions;

    // Map partitioning the session pool by logged in user.
    stdx::unordered_map<SHA256Block, std::list<Session>> _perUserSessionPool;

    // Protects the internal data structures.
    mutable std::mutex _mutex;
};

}  // namespace mongo
