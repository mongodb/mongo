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


#include "mongo/db/s/resharding/resharding_donor_post_cloning_delta_collector.h"

#include "mongo/db/s/resharding/resharding_future_util.h"
#include "mongo/db/s/resharding/resharding_util.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kResharding

namespace mongo {

ReshardingDonorPostCloningDeltaCollector::ReshardingDonorPostCloningDeltaCollector(
    ReshardingCoordinatorDocument coordinatorDoc,
    std::shared_ptr<ReshardingCoordinatorExternalState> externalState,
    CancellationToken abortToken,
    std::unique_ptr<HierarchicalCancelableOperationContextFactory> cancelableOpCtxFactory)
    : _coordinatorDoc(std::move(coordinatorDoc)),
      _externalState(std::move(externalState)),
      _abortToken(std::move(abortToken)),
      _cancelableOpCtxFactory(std::move(cancelableOpCtxFactory)) {}

SharedSemiFuture<std::map<ShardId, int64_t>> ReshardingDonorPostCloningDeltaCollector::launch(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor, otel::traces::Span span) {
    _run(executor).getAsync([self = shared_from_this(), span = std::move(span)](
                                StatusWith<std::map<ShardId, int64_t>> sw) mutable {
        auto localSpan = std::move(span);
        self->_completionPromise.setFrom(std::move(sw));
    });

    return _completionPromise.getFuture();
}

ExecutorFuture<std::map<ShardId, int64_t>> ReshardingDonorPostCloningDeltaCollector::_run(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    // Don't re-fetch if the delta was already persisted (e.g. after a resume). We only need to
    // check 'documentsFinal' on the first entry because it will either be set on all entries or
    // on none of them.
    bool needToFetch = (_coordinatorDoc.getState() == CoordinatorStateEnum::kBlockingWrites) &&
        !_coordinatorDoc.getDonorShards().front().getDocumentsFinal().has_value();
    if (!needToFetch) {
        return ExecutorFuture<std::map<ShardId, int64_t>>(**executor, std::map<ShardId, int64_t>{});
    }

    LOGV2(1003581,
          "Start fetching the change in the number of documents from all donor shards",
          "reshardingUUID"_attr = _coordinatorDoc.getReshardingUUID());

    return resharding::WithAutomaticRetry([this, executor] {
               return ExecutorFuture<void>(**executor)
                   .then([this, anchor = shared_from_this(), executor] {
                       // Operations at or past kBlockingWrites are non-deprioritizable to avoid
                       // stalling while holding the critical section.
                       auto opCtx = resharding::makeReshardingOperationContext(
                           *_cancelableOpCtxFactory, true /* nonDeprioritizable */);

                       return _externalState->getDocumentsDeltaFromDonors(
                           opCtx.get(),
                           **executor,
                           _abortToken,
                           _coordinatorDoc.getReshardingUUID(),
                           _coordinatorDoc.getSourceNss(),
                           resharding::extractShardIdsFromParticipantEntries(
                               _coordinatorDoc.getDonorShards()));
                   });
           })
        .onTransientError([](const Status& status) {
            LOGV2(1003584,
                  "Resharding coordinator encountered transient error while fetching the final "
                  "number of documents from donor shards",
                  "error"_attr = status);
        })
        .onUnrecoverableError([](const Status& status) {
            LOGV2(10494620,
                  "Resharding coordinator encountered unrecoverable error while fetching the final "
                  "number of documents from donor shards",
                  "error"_attr = status);
        })
        .runOn(**executor, _abortToken);
}

}  // namespace mongo
