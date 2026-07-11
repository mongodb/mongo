// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


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
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
    otel::traces::Span span,
    std::function<void()> onRetry) {
    // unsafeToInlineFuture() ensures the callback runs even if the executor is shut down; otherwise
    // getAsync skips it, leaving _completionPromise unset and hanging waitForCompletion().
    _run(executor, std::move(onRetry))
        .unsafeToInlineFuture()
        .getAsync([self = shared_from_this(),
                   span = std::move(span)](StatusWith<std::map<ShardId, int64_t>> sw) mutable {
            auto localSpan = std::move(span);
            self->_completionPromise.setFrom(std::move(sw));
        });

    return _completionPromise.getFuture();
}

ExecutorFuture<std::map<ShardId, int64_t>> ReshardingDonorPostCloningDeltaCollector::_run(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor, std::function<void()> onRetry) {
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
        .onTransientError([onRetry](const Status& status) {
            onRetry();
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
