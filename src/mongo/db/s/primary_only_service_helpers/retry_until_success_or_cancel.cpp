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

#include "mongo/db/s/primary_only_service_helpers/retry_until_success_or_cancel.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace primary_only_service_helpers {

const Backoff RetryUntilSuccessOrCancel::kBackoff{Seconds(1), Milliseconds::max()};

RetryUntilSuccessOrCancel::RetryUntilSuccessOrCancel(
    StringData serviceName,
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

void RetryUntilSuccessOrCancel::logError(StringData errorKind,
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
