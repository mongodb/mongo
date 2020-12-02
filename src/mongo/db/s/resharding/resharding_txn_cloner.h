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

#include <boost/optional.hpp>
#include <memory>
#include <utility>

#include "mongo/bson/timestamp.h"
#include "mongo/db/logical_session_id_gen.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session_catalog_mongod.h"
#include "mongo/db/session_txn_record_gen.h"
#include "mongo/s/resharding/common_types_gen.h"
#include "mongo/util/future.h"

namespace mongo {

namespace executor {

class TaskExecutor;

}  // namespace executor

class OperationContext;

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
     *      {$match: {"lastWriteOpTime.ts": {$lt: <fetchTimestamp>}}},
     * ]
     */
    std::unique_ptr<Pipeline, PipelineDeleter> makePipeline(
        OperationContext* opCtx,
        std::shared_ptr<MongoProcessInterface> mongoProcessInterface,
        const boost::optional<LogicalSessionId>& startAfter);

    ExecutorFuture<void> run(
        ServiceContext* serviceContext,
        std::shared_ptr<executor::TaskExecutor> executor,
        std::shared_ptr<MongoProcessInterface> mongoProcessInterface_forTest = nullptr);

    void updateProgressDocument_forTest(OperationContext* opCtx, const LogicalSessionId& progress) {
        _updateProgressDocument(opCtx, progress);
    }

private:
    boost::optional<LogicalSessionId> _fetchProgressLsid(OperationContext* opCtx);

    std::unique_ptr<Pipeline, PipelineDeleter> _targetAggregationRequest(OperationContext* opCtx,
                                                                         const Pipeline& pipeline);

    ServiceContext::UniqueOperationContext _makeOperationContext(ServiceContext* serviceContext);

    ExecutorFuture<std::pair<ServiceContext::UniqueOperationContext,
                             std::unique_ptr<MongoDOperationContextSession>>>
    _checkOutSession(ServiceContext* serviceContext,
                     std::shared_ptr<executor::TaskExecutor> executor,
                     SessionTxnRecord donorRecord);

    void _updateSessionRecord(OperationContext* opCtx);

    void _updateProgressDocument(OperationContext* opCtx, const LogicalSessionId& progress);

    template <typename Callable>
    auto _withTemporaryOperationContext(ServiceContext* serviceContext, Callable&& callable);

    ExecutorFuture<void> _updateSessionRecordsUntilPipelineExhausted(
        ServiceContext* serviceContext,
        std::shared_ptr<executor::TaskExecutor> executor,
        std::unique_ptr<Pipeline, PipelineDeleter> pipeline,
        int progressCounter);

    const ReshardingSourceId _sourceId;
    const Timestamp _fetchTimestamp;
};

/**
 * Create pipeline stages for iterating donor config.transactions.  The pipeline has these stages:
 * pipeline: [
 *      {$match: {_id: {$gt: <startAfter>}}},
 *      {$sort: {_id: 1}},
 *      {$match: {"lastWriteOpTime.ts": {$lt: <fetchTimestamp>}}},
 * ],
 * Note that the caller is responsible for making sure that the transactions ns is set in the
 * expCtx.
 *
 * fetchTimestamp never isNull()
 */
std::unique_ptr<Pipeline, PipelineDeleter> createConfigTxnCloningPipelineForResharding(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    Timestamp fetchTimestamp,
    boost::optional<LogicalSessionId> startAfter);

}  // namespace mongo
