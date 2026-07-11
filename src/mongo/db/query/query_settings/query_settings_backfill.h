// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/query_settings/query_settings_service.h"
#include "mongo/executor/async_rpc_targeter.h"
#include "mongo/executor/task_executor.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/modules.h"

#include <memory>
#include <mutex>

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

    std::mutex _mutex;
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
    std::unique_ptr<State> consume(WithLock);

    virtual std::shared_ptr<executor::TaskExecutor> makeExecutor(OperationContext* opCtx) = 0;
    virtual std::unique_ptr<async_rpc::Targeter> makeTargeter(OperationContext* opCtx) = 0;

    OnCompletionHook _onCompletionHook;
};

}  // namespace mongo::query_settings
