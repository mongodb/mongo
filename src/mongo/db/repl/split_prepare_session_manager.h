// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/session/internal_session_pool.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/util/modules.h"

#include <cstdint>
#include <mutex>
#include <utility>
#include <vector>

#include <boost/optional.hpp>

namespace [[MONGO_MOD_PUBLIC]] mongo {
namespace repl {

using PooledSession = InternalSessionPool::Session;

/**
 * This struct contains information for a split session, including the split
 * session itself and the id of the requester who wants to acquire the split
 * session, which is usually a writer thread from the oplog applier.
 */
struct SplitSessionInfo {
    PooledSession session;
    uint32_t requesterId;

    SplitSessionInfo(PooledSession&& sess, uint32_t reqId)
        : session(std::move(sess)), requesterId(reqId) {}
};

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
     * The txnNumber is the txnNumber of the top-level session that needs to be split. It
     * servers as a sanity check: onces a session is split, it can not be split again for
     * another txnNumber until it releases the existing split sessions.
     *
     * The requesterIds is a sorted list of requesters (usually writer threads from the
     * oplog applier), each of whom wants to acquire a split session.
     *
     * Asserts if the given session is already split.
     */
    const std::vector<SplitSessionInfo>& splitSession(const LogicalSessionId& sessionId,
                                                      TxnNumber txnNumber,
                                                      const std::vector<uint32_t>& requesterIds);

    /**
     * Returns a vector of split sessions for the given top-level session, or nothing if
     * the given session has not been split.
     *
     * The txnNumber servers a sanity check to make sure it is the same as the one being
     * used for splitSession().
     */
    boost::optional<const std::vector<SplitSessionInfo>&> getSplitSessions(
        const LogicalSessionId& sessionId, TxnNumber txnNumber) const;

    /**
     * Returns true if the given session has been split, or false otherwise. This can be
     * used as an alternative to getSplitSessionIds() when the result is not needed.
     *
     * The txnNumber servers a sanity check to make sure it is the same as the one being
     * used for splitSession().
     */
    bool isSessionSplit(const LogicalSessionId& sessionId, TxnNumber txnNumber) const;

    /**
     * Releases all the split sessions of the give top-level session into the session pool
     * and stops tracking their mapping.
     *
     * The txnNumber servers a sanity check to make sure it is the same as the one being
     * used for splitSession().
     *
     * Asserts if the given session is not split.
     */
    void releaseSplitSessions(const LogicalSessionId& sessionId, TxnNumber txnNumber);

    /**
     * Release and remove all the tracked split sessions.
     */
    void releaseAllSplitSessions();

private:
    // Guards access to member variables.
    mutable std::mutex _mutex;

    // The global session pool storing reusable sessions, from which split sessions are acquired.
    InternalSessionPool* _sessionPool;

    // A map to track top-level sessions and their splits.
    LogicalSessionIdMap<std::pair<TxnNumber, std::vector<SplitSessionInfo>>> _splitSessionMap;
};

}  // namespace repl
}  // namespace mongo
