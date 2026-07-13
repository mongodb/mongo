// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/api_parameters.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/query_shape/query_shape.h"
#include "mongo/db/query/query_stats/data_bearing_node_metrics.h"
#include "mongo/db/query/query_stats/key.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/s/query/exec/cluster_client_cursor.h"
#include "mongo/s/query/exec/cluster_query_result.h"
#include "mongo/util/duration.h"
#include "mongo/util/modules.h"
#include "mongo/util/time_support.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <queue>

#include <boost/move/utility_core.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

class ClusterClientCursorMock final : public ClusterClientCursor {
    ClusterClientCursorMock(const ClusterClientCursorMock&) = delete;
    ClusterClientCursorMock& operator=(const ClusterClientCursorMock&) = delete;

public:
    ClusterClientCursorMock(boost::optional<LogicalSessionId> lsid,
                            boost::optional<TxnNumber> txnNumber,
                            std::function<void(void)> killCallback = {},
                            bool isChangeStreamCursor = false);

    ~ClusterClientCursorMock() override;

    StatusWith<ClusterQueryResult> next() final;

    void kill(OperationContext* opCtx) final;

    Status releaseMemory() final;

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

    void recordChangeStreamThroughputMetricsForBatch() final {}

    void queueResult(ClusterQueryResult&& result) final;

    Status setAwaitDataTimeout(Milliseconds awaitDataTimeout) final;

    boost::optional<LogicalSessionId> getLsid() const final;

    boost::optional<TxnNumber> getTxnNumber() const final;

    void setAPIParameters(APIParameters& apiParameters);

    APIParameters getAPIParameters() const final;

    boost::optional<ReadPreferenceSetting> getReadPreference() const final;

    boost::optional<repl::ReadConcernArgs> getReadConcern() const final;

    bool getRawData() const final {
        return false;
    }

    Date_t getCreatedDate() const final;

    Date_t getLastUseDate() const final;

    bool isChangeStreamCursor() const final;

    bool usesChangeStreamV2ShardTargeting() const final;

    void setLastUseDate(Date_t now) final;

    boost::optional<uint32_t> getPlanCacheShapeHash() const final;

    boost::optional<query_shape::QueryShapeHash> getQueryShapeHash() const final;

    boost::optional<std::size_t> getQueryStatsKeyHash() const final;

    /**
     * Returns false unless the mock cursor has been fully iterated.
     */
    bool remotesExhausted() const final;

    bool isEOF() const final;

    bool hasBeenKilled() const final;

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

    bool _isChangeStreamCursor;

    std::uint64_t _nBatchesReturned = 0;

    APIParameters _apiParameters = APIParameters();
};

}  // namespace mongo
