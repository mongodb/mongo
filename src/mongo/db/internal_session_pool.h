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

#include "mongo/db/operation_context.h"

#include <stack>

namespace mongo {

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
        void setSessionId(LogicalSessionId lsid) {
            _lsid = std::move(lsid);
        }
        void setTxnNumber(TxnNumber txnNumber) {
            _txnNumber = txnNumber;
        }

        LogicalSessionId _lsid;
        TxnNumber _txnNumber;
    };

    InternalSessionPool() = default;

    static InternalSessionPool* get(ServiceContext* serviceContext);
    static InternalSessionPool* get(OperationContext* opCtx);

    Session acquire(OperationContext* opCtx);
    Session acquire(OperationContext* opCtx, const LogicalSessionId& parentLsid);

    void release(Session);

protected:
    // Used for associating parent lsids with existing Sessions of the form <id, uid, txnUUID>
    LogicalSessionIdMap<Session> _childSessions;

    // Used for standalone Sessions
    std::stack<Session> _nonChildSessions;

private:
    // Protects the internal data structures
    mutable Mutex _mutex = MONGO_MAKE_LATCH("InternalSessionPool::_mutex");
};

}  // namespace mongo
