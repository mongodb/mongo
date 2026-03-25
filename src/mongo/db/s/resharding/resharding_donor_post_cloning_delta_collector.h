/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/hierarchical_cancelable_operation_context_factory.h"
#include "mongo/db/s/resharding/coordinator_document_gen.h"
#include "mongo/db/s/resharding/resharding_coordinator_service_external_state.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/executor/scoped_task_executor.h"
#include "mongo/otel/traces/span/span.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/future.h"

#include <map>
#include <memory>

namespace mongo {

/**
 * Fetches the change in document count from all donor shards after the cloning phase of a
 * resharding operation. This delta is needed for post-cloning verification and represents the
 * difference between the clone timestamp and the blocking-writes timestamp on each donor.
 */
class ReshardingDonorPostCloningDeltaCollector
    : public std::enable_shared_from_this<ReshardingDonorPostCloningDeltaCollector> {
public:
    ReshardingDonorPostCloningDeltaCollector(
        ReshardingCoordinatorDocument coordinatorDoc,
        std::shared_ptr<ReshardingCoordinatorExternalState> externalState,
        CancellationToken abortToken,
        std::unique_ptr<HierarchicalCancelableOperationContextFactory> cancelableOpCtxFactory);

    /**
     * Starts the asynchronous task to fetch the document count delta from each donor shard.
     * The provided span will be kept alive for the duration of the async operation. Returns a
     * future that resolves with the per-shard document delta map once the fetch completes, or
     * with an empty map if no fetch was needed.
     */
    SharedSemiFuture<std::map<ShardId, int64_t>> launch(
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor, otel::traces::Span span);

private:
    ExecutorFuture<std::map<ShardId, int64_t>> _run(
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor);

    const ReshardingCoordinatorDocument _coordinatorDoc;
    const std::shared_ptr<ReshardingCoordinatorExternalState> _externalState;
    const CancellationToken _abortToken;
    const std::unique_ptr<HierarchicalCancelableOperationContextFactory> _cancelableOpCtxFactory;

    SharedPromise<std::map<ShardId, int64_t>> _completionPromise;
};

}  // namespace mongo
