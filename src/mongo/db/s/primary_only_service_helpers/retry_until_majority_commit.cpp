/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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


#include "mongo/db/s/primary_only_service_helpers/retry_until_majority_commit.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/wait_for_majority_service.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace primary_only_service_helpers {

RetryUntilMajorityCommit::RetryUntilMajorityCommit(
    StringData serviceName,
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
        operationName, [cancelToken, this](const auto& factory) {
            auto opCtx = factory.makeOperationContext(&cc());
            auto client = opCtx->getClient();
            repl::ReplClientInfo::forClient(client).setLastOpToSystemLastOpTime(opCtx.get());
            auto opTime = repl::ReplClientInfo::forClient(client).getLastOp();
            return WaitForMajorityService::get(client->getServiceContext())
                .waitUntilMajorityForWrite(client->getServiceContext(), opTime, cancelToken);
        });
}


}  // namespace primary_only_service_helpers
}  // namespace mongo
