// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/s/primary_only_service_helpers/operation_session_tracker.h"
#include "mongo/executor/task_executor.h"

namespace mongo {

/**
 * CausalityBarrier implementation that performs a no-op retryable write on a specified set of
 * shards.
 */
class [[MONGO_MOD_PUBLIC]] ParticipantCausalityBarrier : public CausalityBarrier {
public:
    ParticipantCausalityBarrier(std::vector<ShardId> participants,
                                std::shared_ptr<executor::TaskExecutor> executor,
                                CancellationToken token);

    void perform(OperationContext* opCtx, const OperationSessionInfo& osi) override;

private:
    void _performNoopRetryableWriteOnShards(OperationContext* opCtx,
                                            const std::vector<ShardId>& shardIds,
                                            const OperationSessionInfo& osi,
                                            const std::shared_ptr<executor::TaskExecutor>& executor,
                                            const CancellationToken& token);

    std::vector<ShardId> _participants;
    std::shared_ptr<executor::TaskExecutor> _executor;
    CancellationToken _token;
};

}  // namespace mongo

