// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/timestamp.h"
#include "mongo/db/client.h"
#include "mongo/db/repl/optime.h"
#include "mongo/util/modules.h"

namespace mongo {

class BSONObjBuilder;
class Client;
class OperationContext;

namespace repl {

class [[MONGO_MOD_PUBLIC]] ReplClientInfo {
public:
    static const Client::Decoration<ReplClientInfo> forClient;

    /**
     * Sets the LastOp to the provided op, which MUST be greater than or equal to the current value
     * of the LastOp. This also marks that the LastOp was set explicitly on the client so we wait
     * for write concern.
     */
    void setLastOp(OperationContext* opCtx, const OpTime& op);

    OpTime getLastOp() const {
        return _lastOp;
    }

    /**
     * Stores the operation time of the latest proxy write, that is, a write that was forwarded
     * to and executed on a different node instead of being executed locally.
     */
    void setLastProxyWriteTimestampForward(const Timestamp& timestamp) {
        // Only advance the operation time of the latest proxy write if it is greater than the one
        // currently stored.
        if (timestamp > _lastProxyWriteTimestamp) {
            _lastProxyWriteTimestamp = timestamp;
        }
    }

    /**
     * Returns the greater of the timestamps set by 'setLastOp()' and
     * 'setLastProxyWriteTimestampForward()'.
     */
    Timestamp getMaxKnownOperationTime() const {
        auto lastOpTimestamp = _lastOp.getTimestamp();
        return lastOpTimestamp > _lastProxyWriteTimestamp ? lastOpTimestamp
                                                          : _lastProxyWriteTimestamp;
    }

    /**
     * Returns true when either setLastOp() or setLastOpToSystemLastOpTime() was called to set the
     * opTime under the current OperationContext.
     */
    bool lastOpWasSetExplicitlyByClientForCurrentOperation(OperationContext* opCtx) const;

    /**
     * Resets the last op on this client.
     * WARNING: This should only be used when the lastOp is no longer needed for the client.
     */
    void clearLastOp() {
        _lastOp = OpTime();
    }

    /**
     * Resets the last op set explicitly flag on this client.
     * Used for tests only.
     */
    [[MONGO_MOD_NEEDS_REPLACEMENT]] void clearLastOpSetFlag_forTest(OperationContext* opCtx);

    /**
     * Use this to set the LastOp to the latest known OpTime in the oplog. On primary, The OpTime
     * used consists of the timestamp of the latest oplog entry on disk and the current term. On
     * secondaries, lastAppliedOpTime is used. Using lastAppliedOpTime on secondaries is the desired
     * behavior, since secondaries do timestamped reads at the lastApplied.
     *
     * Setting the lastOp to the latest OpTime is necessary when doing no-op writes, as we need to
     * set the client's lastOp to a proper value for write concern wait to work.
     *
     * An exception to this are multi-document transactions, which do a noop write at commit time
     * and advance the client's lastOp in case the transaction resulted in a no-op.
     */
    void setLastOpToSystemLastOpTime(OperationContext* opCtx);

    /**
     * Same as setLastOpToSystemLastOpTime but ignores errors if the OperationContext is
     * interrupted.
     */
    void setLastOpToSystemLastOpTimeIgnoringCtxInterrupted(OperationContext* opCtx);

private:
    static const long long kUninitializedTerm = -1;

    OpTime _lastOp = OpTime();

    Timestamp _lastProxyWriteTimestamp;
};

}  // namespace repl
}  // namespace mongo
