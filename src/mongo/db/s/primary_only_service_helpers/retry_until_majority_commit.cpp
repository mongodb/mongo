// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/s/primary_only_service_helpers/retry_until_majority_commit.h"

#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/wait_for_majority_service.h"

#include <string_view>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace primary_only_service_helpers {

RetryUntilMajorityCommit::RetryUntilMajorityCommit(
    std::string_view serviceName,
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancelState* cancelState,
    BSONObj metadata)
    : _serviceName{serviceName},
      _cancelState{cancelState},
      _metadata{std::move(metadata)},
      _taskExecutor{executor},
      _markKilledExecutor{std::make_shared<ThreadPool>([serviceName] {
          ThreadPool::Options options;
          options.poolName = fmt::format("{}MarkKilledExecutorThreadPool", serviceName);
          options.minThreads = 0;
          options.maxThreads = 1;
          return options;
      }())},
      _retryUntilStepdown{
          _createRetryHelper(_cancelState->getStepdownToken(), kAlwaysRetryPredicate)},
      _retryUntilAbort{_createRetryHelper(_cancelState->getAbortOrStepdownToken(),
                                          kDefaultRetryabilityPredicate)} {}

RetryUntilSuccessOrCancel RetryUntilMajorityCommit::_createRetryHelper(
    CancellationToken token, RetryabilityPredicate predicate) {
    return RetryUntilSuccessOrCancel{_serviceName,
                                     _taskExecutor,
                                     _markKilledExecutor,
                                     std::move(token),
                                     _metadata,
                                     std::move(predicate)};
}

ExecutorFuture<void> RetryUntilMajorityCommit::_waitForMajorityOrStepdown(
    const std::string& operationName) {
    auto cancelToken = _cancelState->getStepdownToken();
    return _retryUntilStepdown.untilSuccessOrCancel(
        operationName, [cancelToken, this](auto factory) {
            auto opCtx = factory->makeOperationContext(&cc());
            auto client = opCtx->getClient();
            repl::ReplClientInfo::forClient(client).setLastOpToSystemLastOpTime(opCtx.get());
            auto opTime = repl::ReplClientInfo::forClient(client).getLastOp();
            return WaitForMajorityService::get(client->getServiceContext())
                .waitUntilMajorityForWrite(opTime, cancelToken);
        });
}


}  // namespace primary_only_service_helpers
}  // namespace mongo
