// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/s/primary_only_service_helpers/participant_causality_barrier.h"

#include "mongo/db/generic_argument_util.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_util.h"

namespace mongo {

ParticipantCausalityBarrier::ParticipantCausalityBarrier(
    std::vector<ShardId> participants,
    std::shared_ptr<executor::TaskExecutor> executor,
    CancellationToken token)
    : _participants{std::move(participants)},
      _executor{std::move(executor)},
      _token{std::move(token)} {}

void ParticipantCausalityBarrier::perform(OperationContext* opCtx,
                                          const OperationSessionInfo& osi) {
    _performNoopRetryableWriteOnShards(opCtx, _participants, osi, _executor, _token);
}

void ParticipantCausalityBarrier::_performNoopRetryableWriteOnShards(
    OperationContext* opCtx,
    const std::vector<ShardId>& shardIds,
    const OperationSessionInfo& osi,
    const std::shared_ptr<executor::TaskExecutor>& executor,
    const CancellationToken& token) {
    auto updateOp = sharding_ddl_util::buildNoopWriteRequestCommand();
    generic_argument_util::setOperationSessionInfo(updateOp, osi);
    generic_argument_util::setMajorityWriteConcern(updateOp);
    auto opts = std::make_shared<async_rpc::AsyncRPCOptions<write_ops::UpdateCommandRequest>>(
        executor, token, updateOp);
    sharding_ddl_util::sendAuthenticatedCommandToShards(opCtx, opts, shardIds);
}


}  // namespace mongo
