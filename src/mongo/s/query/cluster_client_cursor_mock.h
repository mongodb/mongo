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

#include <queue>

#include <boost/optional.hpp>

#include "mongo/db/logical_session_id.h"
#include "mongo/s/query/cluster_client_cursor.h"
#include "mongo/stdx/functional.h"

namespace mongo {

class ClusterClientCursorMock final : public ClusterClientCursor {
    MONGO_DISALLOW_COPYING(ClusterClientCursorMock);

public:
    ClusterClientCursorMock(boost::optional<LogicalSessionId> lsid,
                            boost::optional<TxnNumber> txnNumber,
                            stdx::function<void(void)> killCallback = stdx::function<void(void)>());

    ~ClusterClientCursorMock();

    StatusWith<ClusterQueryResult> next(RouterExecStage::ExecContext) final;

    void kill(OperationContext* opCtx) final;

    void reattachToOperationContext(OperationContext* opCtx) final {
        _opCtx = opCtx;
    }

    void detachFromOperationContext() final {
        _opCtx = nullptr;
    }

    OperationContext* getCurrentOperationContext() const final {
        return _opCtx;
    }

    bool isTailable() const final;

    bool isTailableAndAwaitData() const final;

    BSONObj getOriginatingCommand() const final;

    const PrivilegeVector& getOriginatingPrivileges() const& final;
    void getOriginatingPrivileges() && = delete;

    std::size_t getNumRemotes() const final;

    BSONObj getPostBatchResumeToken() const final;

    long long getNumReturnedSoFar() const final;

    void queueResult(const ClusterQueryResult& result) final;

    Status setAwaitDataTimeout(Milliseconds awaitDataTimeout) final;

    boost::optional<LogicalSessionId> getLsid() const final;

    boost::optional<TxnNumber> getTxnNumber() const final;

    boost::optional<ReadPreferenceSetting> getReadPreference() const final;

    Date_t getCreatedDate() const final;

    Date_t getLastUseDate() const final;

    void setLastUseDate(Date_t now) final;

    std::uint64_t getNBatches() const final;

    void incNBatches() final;

    /**
     * Returns true unless marked as having non-exhausted remote cursors via
     * markRemotesNotExhausted().
     */
    bool remotesExhausted() final;

    void markRemotesNotExhausted();

    /**
     * Queues an error response.
     */
    void queueError(Status status);

private:
    bool _killed = false;
    bool _exhausted = false;
    std::queue<StatusWith<ClusterQueryResult>> _resultsQueue;
    stdx::function<void(void)> _killCallback;

    // Originating command object.
    BSONObj _originatingCommand;

    // Privileges of originating command
    PrivilegeVector _originatingPrivileges;

    // Number of returned documents.
    long long _numReturnedSoFar = 0;

    bool _remotesExhausted = true;

    boost::optional<LogicalSessionId> _lsid;

    boost::optional<TxnNumber> _txnNumber;

    OperationContext* _opCtx = nullptr;

    Date_t _createdDate;

    Date_t _lastUseDate;

    std::uint64_t _nBatchesReturned = 0;
};

}  // namespace mongo
