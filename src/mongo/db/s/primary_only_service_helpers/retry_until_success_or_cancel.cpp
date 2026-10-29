// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/s/primary_only_service_helpers/retry_until_success_or_cancel.h"

#include <string_view>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace primary_only_service_helpers {

RetryUntilSuccessOrCancel::RetryUntilSuccessOrCancel(
    std::string_view serviceName,
    std::shared_ptr<executor::ScopedTaskExecutor> taskExecutor,
    std::shared_ptr<ThreadPool> markKilledExecutor,
    CancellationToken token,
    BSONObj metadata,
    RetryabilityPredicate isRetryable)
    : _serviceName{serviceName},
      _taskExecutor{std::move(taskExecutor)},
      _token{token},
      _retryFactory(token, markKilledExecutor, isRetryable),
      _metadata{std::move(metadata)} {}

void RetryUntilSuccessOrCancel::logError(std::string_view errorKind,
                                         const std::string& operationName,
                                         const Status& status) const {
    LOGV2(8126400,
          "Service encountered an error",
          "service"_attr = _serviceName,
          "errorKind"_attr = errorKind,
          "operation"_attr = operationName,
          "status"_attr = redact(status),
          "metadata"_attr = _metadata);
}

}  // namespace primary_only_service_helpers
}  // namespace mongo
