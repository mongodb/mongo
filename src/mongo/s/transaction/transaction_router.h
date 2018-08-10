/**
 *    Copyright (C) 2018 MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <boost/optional.hpp>
#include <map>
#include <memory>

#include "mongo/base/disallow_copying.h"
#include "mongo/db/logical_session_id.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/s/shard_id.h"
#include "mongo/util/string_map.h"

namespace mongo {

/**
 * Keeps track of the transaction state. A session is in use when it is being used by a request.
 */
class TransactionRouter {
public:
    /**
     * Represents a shard participant in a distributed transaction. Lives only for the duration of
     * the
     * transaction that created it.
     */
    class Participant {
    public:
        explicit Participant(bool isCoordinator,
                             TxnNumber txnNumber,
                             repl::ReadConcernArgs readConcernArgs);

        enum class State {
            // Next transaction should include startTransaction.
            kMustStart,
            // startTransaction already sent to this participant.
            kStarted,
        };

        /**
         * Attaches necessary fields if this is participating in a multi statement transaction.
         */
        BSONObj attachTxnFieldsIfNeeded(BSONObj cmd);

        State getState();

        /**
         * True if the participant has been chosen as the coordinator for its transaction.
         */
        bool isCoordinator();

        /**
         * Mark this participant as a node that has been successfully sent a command.
         */
        void markAsCommandSent();

        void setAtClusterTime(const LogicalTime atClusterTime);

    private:
        State _state{State::kMustStart};
        const bool _isCoordinator{false};
        const TxnNumber _txnNumber;
        const repl::ReadConcernArgs _readConcernArgs;
        boost::optional<LogicalTime> _atClusterTime;
    };

    TransactionRouter(LogicalSessionId sessionId);

    /**
     * Starts a fresh transaction in this session. Also cleans up the previous transaction state.
     */
    void beginOrContinueTxn(OperationContext* opCtx, TxnNumber txnNumber, bool startTransaction);

    /**
     * Returns the participant for this transaction. Creates a new one if it doesn't exist.
     */
    Participant& getOrCreateParticipant(const ShardId& shard);

    void checkIn();
    void checkOut();

    /**
     * Computes and sets the atClusterTime for the current transaction. Does nothing if the given
     * query is not the first statement that this transaction runs (i.e. if the atClusterTime
     * has already been set).
     */
    void computeAtClusterTime(OperationContext* opCtx,
                              bool mustRunOnAll,
                              const std::set<ShardId>& shardIds,
                              const NamespaceString& nss,
                              const BSONObj query,
                              const BSONObj collation);

    /**
     * Computes and sets the atClusterTime for the current transaction if it targets the
     * given shard during its first statement. Does nothing if the atClusterTime has already
     * been set.
     */
    void computeAtClusterTimeForOneShard(OperationContext* opCtx, const ShardId& shardId);

    bool isCheckedOut();

    const LogicalSessionId& getSessionId() const;

    boost::optional<ShardId> getCoordinatorId() const;

    /**
     * Extract the runtimne state attached to the operation context. Returns nullptr if none is
     * attached.
     */
    static TransactionRouter* get(OperationContext* opCtx);

private:
    const LogicalSessionId _sessionId;
    TxnNumber _txnNumber{kUninitializedTxnNumber};

    // True if this is currently being used by a request.
    bool _isCheckedOut{false};

    // Map of current participants of the current transaction.
    StringMap<Participant> _participants;

    // The id of coordinator participant, used to construct prepare requests.
    boost::optional<ShardId> _coordinatorId;

    // The read concern the current transaction was started with.
    repl::ReadConcernArgs _readConcernArgs;

    // The cluster time of the timestamp all participant shards in the current transaction with
    // snapshot level read concern must read from. Selected during the first statement of the
    // transaction.
    boost::optional<LogicalTime> _atClusterTime;
};

/**
 * Scoped object, which checks out the session specified in the passed operation context and stores
 * it for later access by the command. The session is installed at construction time and is removed
 * at destruction. This can only be used for multi-statement transactions.
 */
class ScopedRouterSession {
    MONGO_DISALLOW_COPYING(ScopedRouterSession);

public:
    ScopedRouterSession(OperationContext* opCtx);
    ~ScopedRouterSession();

private:
    OperationContext* const _opCtx;
};

}  // namespace mongo
