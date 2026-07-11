// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/s/primary_only_service_helpers/operation_session_tracker.h"
#include "mongo/executor/task_executor.h"

namespace mongo {

/**
 * CausalityBarrier implementation that performs a noop retryable write on all shards and the
 * config server.
 */
class [[MONGO_MOD_PUBLIC]] AllShardsAndConfigCausalityBarrier : public CausalityBarrier {
public:
    AllShardsAndConfigCausalityBarrier(std::shared_ptr<executor::TaskExecutor> executor,
                                       CancellationToken token);

    void perform(OperationContext* opCtx, const OperationSessionInfo& osi) override;

private:
    std::shared_ptr<executor::TaskExecutor> _executor;
    CancellationToken _token;
};

}  // namespace mongo
