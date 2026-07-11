// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#pragma once

#include "mongo/db/hierarchical_cancelable_operation_context_factory.h"
#include "mongo/db/s/resharding/coordinator_document_gen.h"
#include "mongo/db/s/resharding/resharding_coordinator_service_external_state.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/executor/scoped_task_executor.h"
#include "mongo/otel/traces/span/span.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/future.h"

#include <functional>
#include <map>
#include <memory>

namespace mongo {

/**
 * Fetches the change in document count from all recipient shards after the cloning phase of a
 * resharding operation. This delta is needed final collection verification and represents the
 * change in the number of documents on each recipient since cloning started.
 */
class ReshardingRecipientPostCloningDeltaCollector
    : public std::enable_shared_from_this<ReshardingRecipientPostCloningDeltaCollector> {
public:
    ReshardingRecipientPostCloningDeltaCollector(
        ReshardingCoordinatorDocument coordinatorDoc,
        std::shared_ptr<ReshardingCoordinatorExternalState> externalState,
        CancellationToken abortToken,
        std::unique_ptr<HierarchicalCancelableOperationContextFactory> cancelableOpCtxFactory);

    /**
     * Starts the asynchronous task to fetch the document count delta from each recipient shard.
     * The provided span will be kept alive for the duration of the async operation. Returns a
     * future that resolves with the per-shard document delta map once the fetch completes, or
     * with an empty map if no fetch was needed.
     */
    SharedSemiFuture<std::map<ShardId, int64_t>> launch(
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
        otel::traces::Span span,
        std::function<void()> onRetry);

private:
    ExecutorFuture<std::map<ShardId, int64_t>> _run(
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
        std::function<void()> onRetry);

    const ReshardingCoordinatorDocument _coordinatorDoc;
    const std::shared_ptr<ReshardingCoordinatorExternalState> _externalState;
    const CancellationToken _abortToken;
    const std::unique_ptr<HierarchicalCancelableOperationContextFactory> _cancelableOpCtxFactory;

    SharedPromise<std::map<ShardId, int64_t>> _completionPromise;
};

}  // namespace mongo
