/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/session/internal_session_pool.h"
#include "mongo/db/session/logical_session_id.h"
#include <vector>

namespace mongo {
namespace repl {

using PooledSession = InternalSessionPool::Session;

/**
 * This class manages the sessions for split prepared transactions.
 *
 * Prepared transactions are split and applied in parallel on secondaries, and this class is
 * used to create split sessions and track the mapping of top-level sessions and their splits.
 * A primary can also see prepared transactions in the split state if it performed the split
 * and prepare without committing when it was still a secondary, so this class can be accessed
 * by both primaries and secondaries.
 */
class SplitPrepareSessionManager {
    SplitPrepareSessionManager(const SplitPrepareSessionManager&) = delete;
    SplitPrepareSessionManager& operator=(const SplitPrepareSessionManager&) = delete;

public:
    explicit SplitPrepareSessionManager(InternalSessionPool* sessionPool);

    /**
     * Creates split sessions for the given top-level session and track the mapping.
     *
     * Asserts if the given session is already split.
     */
    const std::vector<PooledSession>& splitSession(const LogicalSessionId& sessionId,
                                                   TxnNumber txnNumber,
                                                   uint32_t numSplits);

    /**
     * Returns a vector of split sessions for the given top-level session, or nothing if
     * the given session has not been split.
     */
    boost::optional<const std::vector<PooledSession>&> getSplitSessions(
        const LogicalSessionId& sessionId, TxnNumber txnNumber) const;

    /**
     * Returns true if the given session has been split, or false otherwise. This can be
     * used as an alternative to getSplitSessionIds() when the result is not needed.
     */
    bool isSessionSplit(const LogicalSessionId& sessionId, TxnNumber txnNumber) const;

    /**
     * Releases all the split sessions of the give top-level session into the session pool
     * and stops tracking their mapping.
     *
     * Asserts if the given session is not split.
     */
    void releaseSplitSessions(const LogicalSessionId& sessionId, TxnNumber txnNumber);

private:
    // Guards access to member variables.
    mutable Mutex _mutex = MONGO_MAKE_LATCH("SplitPrepareSessionManager::_mutex");

    // The global session pool storing reusable sessions, from which split sessions are acquired.
    InternalSessionPool* _sessionPool;

    // A map to track top-level sessions and their splits.
    LogicalSessionIdMap<std::pair<TxnNumber, std::vector<PooledSession>>> _splitSessionMap;
};

}  // namespace repl
}  // namespace mongo
