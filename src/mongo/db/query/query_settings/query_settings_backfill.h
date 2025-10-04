/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/query_settings/query_settings_service.h"
#include "mongo/db/query/query_shape/query_shape.h"
#include "mongo/executor/async_rpc_targeter.h"
#include "mongo/executor/task_executor.h"
#include "mongo/stdx/mutex.h"

#include <memory>

namespace mongo::query_settings {

using MakeRepresentativeQueryTargeterFn =
    std::function<std::unique_ptr<async_rpc::Targeter>(OperationContext*)>;
using RepresentativeQueryMap = stdx::unordered_map<query_shape::QueryShapeHash, QueryInstance>;

/**
 * Inserts the given 'representativeQueries' into the "config.queryShapeRepresentativeQueries"
 * collection on the appropriate node. The inserts are performed in at most 'BSONObjMaxUserSize'
 * batches.
 *
 * Returns a future containing the newly inserted and already existing query shape hashes.
 */
ExecutorFuture<std::vector<query_shape::QueryShapeHash>> insertRepresentativeQueriesToCollection(
    OperationContext* opCtx,
    std::vector<QueryShapeRepresentativeQuery> representativeQueries,
    MakeRepresentativeQueryTargeterFn makeRepresentativeQueryTargeterFn,
    std::shared_ptr<executor::TaskExecutor> executor);

/**
 * Class responsible for managing query settings backfill operations.
 */
class BackfillCoordinator {
public:
    using OnCompletionHook = std::function<void(
        std::vector<query_shape::QueryShapeHash>, LogicalTime, boost::optional<TenantId>)>;

    /**
     * Creates the appropriate 'BackfillCoordinator' implementation based on the current
     * deployment configuration.
     */
    static std::unique_ptr<BackfillCoordinator> create(OnCompletionHook onCompletionHook);

    explicit BackfillCoordinator(OnCompletionHook onCompletionHook);
    virtual ~BackfillCoordinator() = default;

    /*
     * Check if a representative query needs to be backfilled. The system doesn't attempt to
     * backfill explain originating commands as it would additionally imply altering the original
     * command object.
     */
    static bool shouldBackfill(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                               bool hasRepresentativeQuery);

    /*
     * Marks the representative query and schedules a future backfill overation asynchronously.
     * Does not throw.
     */
    void markForBackfillAndScheduleIfNeeded(OperationContext* opCtx,
                                            query_shape::QueryShapeHash queryShapeHash,
                                            QueryInstance queryInstance);

    /**
     * Cancels any pending work and clears out the coordinator state.
     */
    void cancel();

protected:
    struct State {
        RepresentativeQueryMap buffer = {};
        size_t memoryUsedBytes = 0;
        bool taskScheduled = false;
        CancellationSource cancellationSource = CancellationSource();
    };

    stdx::mutex _mutex;
    std::unique_ptr<State> _state;

private:
    /**
     * Executes the backfill operation. The procedure first filters out the representative queries
     * which had their query settings removed while waiting in the buffering phase and then performs
     * a batched insert targeting the "config.queryShapeRepresentativeQueries" collection. Finally
     * it invokes the provided OnCompletionHook if any query was succesfully inserted to signal the
     * end of the operation.
     */
    ExecutorFuture<void> execute(RepresentativeQueryMap buffer,
                                 CancellationSource cancellationSource,
                                 std::shared_ptr<executor::TaskExecutor> executor);

    /**
     * Convenience proxy method for inserting the representative queries into the
     * "config.queryShapeRepresentativeQueries" collection. Needed for testing.
     */
    virtual ExecutorFuture<std::vector<query_shape::QueryShapeHash>>
    insertRepresentativeQueriesToCollection(
        OperationContext* opCtx,
        std::vector<QueryShapeRepresentativeQuery> representativeQueries,
        std::shared_ptr<executor::TaskExecutor> executor);

    /**
     * Thread safe utillity to return the inner state and replace it with a new one.
     */
    std::unique_ptr<State> consume();
    std::unique_ptr<State> consume_inlock();

    virtual std::shared_ptr<executor::TaskExecutor> makeExecutor(OperationContext* opCtx);
    virtual std::unique_ptr<async_rpc::Targeter> makeTargeter(OperationContext* opCtx) = 0;

    OnCompletionHook _onCompletionHook;
};

}  // namespace mongo::query_settings
