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

#include <boost/move/utility_core.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <queue>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/api_parameters.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/query_stats/data_bearing_node_metrics.h"
#include "mongo/db/query/query_stats/key.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/s/query/exec/cluster_client_cursor.h"
#include "mongo/s/query/exec/cluster_query_result.h"
#include "mongo/util/duration.h"
#include "mongo/util/time_support.h"

namespace mongo {

class ClusterClientCursorMock final : public ClusterClientCursor {
    ClusterClientCursorMock(const ClusterClientCursorMock&) = delete;
    ClusterClientCursorMock& operator=(const ClusterClientCursorMock&) = delete;

public:
    ClusterClientCursorMock(boost::optional<LogicalSessionId> lsid,
                            boost::optional<TxnNumber> txnNumber,
                            std::function<void(void)> killCallback = {});

    ~ClusterClientCursorMock() override;

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

    boost::optional<uint32_t> getPlanCacheShapeHash() const final;

    boost::optional<std::size_t> getQueryStatsKeyHash() const final;

    bool getQueryStatsWillNeverExhaust() const final;

    /**
     * Returns false unless the mock cursor has been fully iterated.
     */
    bool remotesExhausted() final;

    bool hasBeenKilled() final;

    /**
     * Queues an error response.
     */
    void queueError(Status status);

    bool shouldOmitDiagnosticInformation() const final;

    std::unique_ptr<query_stats::Key> takeKey() final;

    boost::optional<query_stats::DataBearingNodeMetrics> takeRemoteMetrics() final {
        return boost::none;
    }

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
