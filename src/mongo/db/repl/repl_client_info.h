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

#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/client.h"
#include "mongo/db/repl/optime.h"

namespace mongo {

class BSONObjBuilder;
class Client;
class OperationContext;

namespace repl {

class ReplClientInfo {
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
     * Same as setLastOpToSystemLastOpTime but ignores interruption errors.
     */
    void setLastOpToSystemLastOpTimeIgnoringInterrupt(OperationContext* opCtx);

private:
    static const long long kUninitializedTerm = -1;

    OpTime _lastOp = OpTime();

    Timestamp _lastProxyWriteTimestamp;
};

}  // namespace repl
}  // namespace mongo
