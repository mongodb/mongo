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
#include "mongo/executor/task_executor.h"
#include "mongo/s/query/exec/cluster_client_cursor.h"
#include "mongo/s/query/exec/cluster_client_cursor_guard.h"
#include "mongo/s/query/exec/cluster_client_cursor_params.h"
#include "mongo/s/query/exec/cluster_query_result.h"
#include "mongo/s/query/exec/router_exec_stage.h"
#include "mongo/util/duration.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <queue>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

class RouterStageMock;

class ClusterClientCursorImpl final : public ClusterClientCursor {
    ClusterClientCursorImpl(const ClusterClientCursorImpl&) = delete;
    ClusterClientCursorImpl& operator=(const ClusterClientCursorImpl&) = delete;

public:
    /**
     * Constructs a cluster query plan and CCC from the given parameters whose safe cleanup is
     * ensured by an RAII object.
     */
    static ClusterClientCursorGuard make(OperationContext* opCtx,
                                         std::shared_ptr<executor::TaskExecutor> executor,
                                         ClusterClientCursorParams&& params);

    /**
     * Constructs a CCC from the given execution tree 'root'. The CCC's safe cleanup is ensured by
     * an RAII object.
     */
    static ClusterClientCursorGuard make(OperationContext* opCtx,
                                         std::unique_ptr<RouterExecStage> root,
                                         ClusterClientCursorParams&& params);

    StatusWith<ClusterQueryResult> next() final;

    Status releaseMemory() final;

    void kill(OperationContext* opCtx) final;

    void reattachToOperationContext(OperationContext* opCtx) final;

    void detachFromOperationContext() final;

    OperationContext* getCurrentOperationContext() const final;

    bool isTailable() const final;

    bool isTailableAndAwaitData() const final;

    BSONObj getOriginatingCommand() const final;

    const PrivilegeVector& getOriginatingPrivileges() const& final;
    void getOriginatingPrivileges() && = delete;

    bool partialResultsReturned() const final;

    std::size_t getNumRemotes() const final;

    BSONObj getPostBatchResumeToken() const final;

    long long getNumReturnedSoFar() const final;

    void queueResult(ClusterQueryResult&& result) final;

    bool remotesExhausted() const final;

    bool hasBeenKilled() const final;

    Status setAwaitDataTimeout(Milliseconds awaitDataTimeout) final;

    boost::optional<LogicalSessionId> getLsid() const final;

    boost::optional<TxnNumber> getTxnNumber() const final;

    APIParameters getAPIParameters() const final;

    boost::optional<ReadPreferenceSetting> getReadPreference() const final;

    boost::optional<repl::ReadConcernArgs> getReadConcern() const final;

    Date_t getCreatedDate() const final;

    Date_t getLastUseDate() const final;

    void setLastUseDate(Date_t now) final;

    boost::optional<uint32_t> getPlanCacheShapeHash() const final;

    boost::optional<query_shape::QueryShapeHash> getQueryShapeHash() const final;

    boost::optional<std::size_t> getQueryStatsKeyHash() const final;

    bool getQueryStatsWillNeverExhaust() const final;

    bool shouldOmitDiagnosticInformation() const final;

    std::unique_ptr<query_stats::Key> takeKey() final;

    boost::optional<query_stats::DataBearingNodeMetrics> takeRemoteMetrics() final {
        return _root->takeRemoteMetrics();
    }

public:
    /**
     * Constructs a CCC whose result set is generated by a mock execution stage.
     */
    ClusterClientCursorImpl(OperationContext* opCtx,
                            std::unique_ptr<RouterExecStage> root,
                            ClusterClientCursorParams&& params,
                            boost::optional<LogicalSessionId> lsid);

    /**
     * Constructs a cluster client cursor.
     */
    ClusterClientCursorImpl(OperationContext* opCtx,
                            std::shared_ptr<executor::TaskExecutor> executor,
                            ClusterClientCursorParams&& params,
                            boost::optional<LogicalSessionId> lsid);

    ~ClusterClientCursorImpl() final;

private:
    /**
     * Constructs the pipeline of MergerPlanStages which will be used to answer the query.
     */
    std::unique_ptr<RouterExecStage> buildMergerPlan(
        OperationContext* opCtx,
        std::shared_ptr<executor::TaskExecutor> executor,
        ClusterClientCursorParams* params);

    ClusterClientCursorParams _params;

    // Number of documents already returned by next().
    long long _numReturnedSoFar = 0;

    // The root stage of the pipeline used to return the result set, merged from the remote nodes.
    std::unique_ptr<RouterExecStage> _root;

    // Stores documents queued by queueResult(). BSONObjs within the stashed results must be owned.
    std::queue<ClusterQueryResult> _stash;

    // Stores the logical session id for this cursor.
    boost::optional<LogicalSessionId> _lsid;

    // The OperationContext that we're executing within. This can be updated if necessary by using
    // detachFromOperationContext() and reattachToOperationContext().
    OperationContext* _opCtx = nullptr;

    // The time the cursor was created.
    Date_t _createdDate;

    // The time when the cursor was last unpinned, i.e. the end of the last getMore.
    Date_t _lastUseDate;

    // The hash of the canonical query encoding CanonicalQuery::QueryShapeString. To be used for
    // slow query logging.
    boost::optional<uint32_t> _planCacheShapeHash;

    // The hash of the query_shape::QueryShape.
    boost::optional<query_shape::QueryShapeHash> _queryShapeHash;

    // Whether ClusterClientCursor::next() was interrupted due to MaxTimeMSExpired.
    bool _maxTimeMSExpired = false;

    // Whether to omit information about the getmore that uses this cursor from currentop and the
    // profiler.
    bool _shouldOmitDiagnosticInformation = false;

    // If boost::none, queryStats should not be collected for this cursor.
    boost::optional<std::size_t> _queryStatsKeyHash;

    // The Key used by query stats to generate the query stats store key.
    std::unique_ptr<query_stats::Key> _queryStatsKey;

    bool _queryStatsWillNeverExhaust = false;

    bool _isChangeStreamQuery = false;

    // Tracks if kill() has been called on the cursor. Multiple calls to kill() is an error.
    bool _hasBeenKilled = false;
};

}  // namespace mongo
