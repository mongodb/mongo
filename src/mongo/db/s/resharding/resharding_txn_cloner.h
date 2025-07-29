/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/bson/timestamp.h"
#include "mongo/db/cancelable_operation_context.h"
#include "mongo/db/exec/agg/exec_pipeline.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/process_interface/mongo_process_interface.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/db/session/session_txn_record_gen.h"
#include "mongo/executor/task_executor.h"
#include "mongo/s/resharding/common_types_gen.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/future.h"

#include <memory>
#include <utility>

#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace executor {

class TaskExecutor;

}  // namespace executor

/**
 * Transfer config.transaction information from a given source shard to this shard.
 */
class ReshardingTxnCloner {
public:
    ReshardingTxnCloner(ReshardingSourceId sourceId, Timestamp fetchTimestamp);

    /**
     * Returns a pipeline for iterating the donor shard's config.transactions collection.
     *
     * The pipeline itself looks like:
     * [
     *      {$match: {_id: {$gt: <startAfter>}}},
     *      {$sort: {_id: 1}},
     * ]
     */
    std::unique_ptr<Pipeline> makePipeline(
        OperationContext* opCtx,
        std::shared_ptr<MongoProcessInterface> mongoProcessInterface,
        const boost::optional<LogicalSessionId>& startAfter);

    /**
     * Schedules work to repeatedly fetch and update config.transactions records.
     *
     * Returns a future that becomes ready when either:
     *   (a) all config.transactions records have been fetched and updated, or
     *   (b) the cancellation token was canceled due to a stepdown or abort.
     */
    SemiFuture<void> run(
        std::shared_ptr<executor::TaskExecutor> executor,
        std::shared_ptr<executor::TaskExecutor> cleanupExecutor,
        CancellationToken cancelToken,
        CancelableOperationContextFactory factory,
        std::shared_ptr<MongoProcessInterface> mongoProcessInterface_forTest = nullptr);

    /**
     * Updates this shard's config.transactions table based on a retryable write or multi-statement
     * transaction that already executed on the donor shard.
     *
     * Returns boost::none unless this shard has an active prepared transaction for the
     * corresponding config.transactions record. It otherwise returns a future that becomes ready
     * once the active prepared transaction on this shard commits or aborts.
     */
    boost::optional<SharedSemiFuture<void>> doOneRecord(OperationContext* opCtx,
                                                        const SessionTxnRecord& donorRecord);

    void updateProgressDocument_forTest(OperationContext* opCtx, const LogicalSessionId& progress) {
        _updateProgressDocument(opCtx, progress);
    }

private:
    boost::optional<LogicalSessionId> _fetchProgressLsid(OperationContext* opCtx);

    std::unique_ptr<Pipeline> _targetAggregationRequest(OperationContext* opCtx,
                                                        const Pipeline& pipeline);

    std::unique_ptr<Pipeline> _restartPipeline(
        OperationContext* opCtx, std::shared_ptr<MongoProcessInterface> mongoProcessInterface);

    boost::optional<SessionTxnRecord> _getNextRecord(OperationContext* opCtx,
                                                     Pipeline& pipeline,
                                                     exec::agg::Pipeline& execPipeline);

    void _updateProgressDocument(OperationContext* opCtx, const LogicalSessionId& progress);

    const ReshardingSourceId _sourceId;
    const Timestamp _fetchTimestamp;
};

/**
 * Create pipeline stages for iterating donor config.transactions.  The pipeline has these stages:
 * pipeline: [
 *      {$match: {_id: {$gt: <startAfter>}}},
 *      {$sort: {_id: 1}},
 * ],
 * Note that the caller is responsible for making sure that the transactions ns is set in the
 * expCtx.
 *
 * fetchTimestamp never isNull()
 */
std::unique_ptr<Pipeline> createConfigTxnCloningPipelineForResharding(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    Timestamp fetchTimestamp,
    boost::optional<LogicalSessionId> startAfter);

}  // namespace mongo
