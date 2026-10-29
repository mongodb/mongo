// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/executor/task_executor_cursor_options.h"

#include "mongo/db/query/getmore_command_gen.h"

namespace mongo {
namespace executor {
BSONObj DefaultTaskExecutorCursorGetMoreStrategy::createGetMoreRequest(
    const CursorId& cursorId,
    const NamespaceString& nss,
    long long prevBatchNumReceived,
    long long totalNumReceived) {
    GetMoreCommandRequest getMoreRequest(cursorId, std::string{nss.coll()});
    getMoreRequest.setBatchSize(_batchSize);
    return getMoreRequest.toBSON();
}

TaskExecutorCursorOptions::TaskExecutorCursorOptions(bool pinConn,
                                                     boost::optional<int64_t> batchSize,
                                                     bool preFetchNextBatch,
                                                     std::shared_ptr<PlanYieldPolicy> yieldPolicy)
    : pinConnection(pinConn),
      getMoreStrategy(
          std::make_shared<DefaultTaskExecutorCursorGetMoreStrategy>(batchSize, preFetchNextBatch)),
      yieldPolicy(std::move(yieldPolicy)) {}

TaskExecutorCursorOptions::TaskExecutorCursorOptions(
    bool pinConn,
    std::shared_ptr<TaskExecutorCursorGetMoreStrategy> getMoreStrategy,
    std::shared_ptr<PlanYieldPolicy> yieldPolicy)
    : pinConnection(pinConn),
      getMoreStrategy(std::move(getMoreStrategy)),
      yieldPolicy(std::move(yieldPolicy)) {}
}  // namespace executor
}  // namespace mongo
