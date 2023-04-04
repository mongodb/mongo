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

#include <boost/optional.hpp>
#include <functional>
#include <queue>

#include "mongo/db/session/logical_session_id.h"
#include "mongo/s/query/cluster_client_cursor.h"

namespace mongo {

class ClusterClientCursorMock final : public ClusterClientCursor {
    ClusterClientCursorMock(const ClusterClientCursorMock&) = delete;
    ClusterClientCursorMock& operator=(const ClusterClientCursorMock&) = delete;

public:
    ClusterClientCursorMock(boost::optional<LogicalSessionId> lsid,
                            boost::optional<TxnNumber> txnNumber,
                            std::function<void(void)> killCallback = {});

    ~ClusterClientCursorMock();

    StatusWith<ClusterQueryResult> next() final;

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

    bool partialResultsReturned() const final;

    std::size_t getNumRemotes() const final;

    BSONObj getPostBatchResumeToken() const final;

    long long getNumReturnedSoFar() const final;

    void queueResult(const ClusterQueryResult& result) final;

    Status setAwaitDataTimeout(Milliseconds awaitDataTimeout) final;

    boost::optional<LogicalSessionId> getLsid() const final;

    boost::optional<TxnNumber> getTxnNumber() const final;

    void setAPIParameters(APIParameters& apiParameters);

    APIParameters getAPIParameters() const final;

    boost::optional<ReadPreferenceSetting> getReadPreference() const final;

    boost::optional<repl::ReadConcernArgs> getReadConcern() const final;

    Date_t getCreatedDate() const final;

    Date_t getLastUseDate() const final;

    void setLastUseDate(Date_t now) final;

    boost::optional<uint32_t> getQueryHash() const final;

    /**
     * Returns false unless the mock cursor has been fully iterated.
     */
    bool remotesExhausted() final;

    /**
     * Queues an error response.
     */
    void queueError(Status status);

    bool shouldOmitDiagnosticInformation() const final;

private:
    bool _killed = false;
    std::queue<StatusWith<ClusterQueryResult>> _resultsQueue;
    std::function<void(void)> _killCallback;

    // Originating command object.
    BSONObj _originatingCommand;

    // Privileges of originating command
    PrivilegeVector _originatingPrivileges;

    // Number of returned documents.
    long long _numReturnedSoFar = 0;

    bool _remotesExhausted = false;

    boost::optional<LogicalSessionId> _lsid;

    boost::optional<TxnNumber> _txnNumber;

    OperationContext* _opCtx = nullptr;

    Date_t _createdDate;

    Date_t _lastUseDate;

    std::uint64_t _nBatchesReturned = 0;

    APIParameters _apiParameters = APIParameters();
};

}  // namespace mongo
