/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/base/string_data.h"
#include "mongo/crypto/sha256_block.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_cache_gen.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/duration.h"
#include "mongo/util/time_support.h"

#include <cstddef>
#include <list>
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
class InternalSessionPool {

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
    mutable stdx::mutex _mutex;
};

}  // namespace mongo
