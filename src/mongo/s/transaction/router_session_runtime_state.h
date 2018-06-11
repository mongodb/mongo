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
#include "mongo/s/shard_id.h"
#include "mongo/util/string_map.h"

namespace mongo {

class TransactionParticipant {
public:
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
     * Mark this participant as a node that has been successfully sent a command.
     */
    void markAsCommandSent();

private:
    State _state{State::kMustStart};
};

/**
 * Keeps track of the transaction state. A session is in use when it is being used by a request.
 */
class RouterSessionRuntimeState {
public:
    RouterSessionRuntimeState(LogicalSessionId sessionId);

    /**
     * Starts a fresh transaction in this session. Also cleans up the previous transaction state.
     */
    void beginOrContinueTxn(TxnNumber txnNumber, bool startTransaction);

    /**
     * Returns the participant for this transaction. Creates a new one if it doesn't exist.
     */
    TransactionParticipant& getOrCreateParticipant(const ShardId& shard);

    void checkIn();
    void checkOut();

    bool isCheckedOut();

    const LogicalSessionId& getSessionId() const;

    /**
     * Extract the runtimne state attached to the operation context. Returns nullptr if none is
     * attached.
     */
    static RouterSessionRuntimeState* get(OperationContext* opCtx);

private:
    const LogicalSessionId _sessionId;
    TxnNumber _txnNumber{kUninitializedTxnNumber};

    // True if this is currently being used by a request.
    bool _isCheckedOut{false};

    // Map of current participants of the current transaction.
    StringMap<TransactionParticipant> _participants;
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
