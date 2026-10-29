// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/s/primary_only_service_helpers/all_shards_and_config_causality_barrier.h"

#include "mongo/db/s/primary_only_service_helpers/participant_causality_barrier.h"
#include "mongo/db/sharding_environment/grid.h"

namespace mongo {

AllShardsAndConfigCausalityBarrier::AllShardsAndConfigCausalityBarrier(
    std::shared_ptr<executor::TaskExecutor> executor, CancellationToken token)
    : _executor{std::move(executor)}, _token{std::move(token)} {}

void AllShardsAndConfigCausalityBarrier::perform(OperationContext* opCtx,
                                                 const OperationSessionInfo& osi) {
    const auto shardsAndConfigsvr = [&] {
        const auto shardRegistry = Grid::get(opCtx)->shardRegistry();
        auto participants = shardRegistry->getAllShardIds(opCtx);
        if (std::find(participants.begin(), participants.end(), ShardId::kConfigServerId) ==
            participants.end()) {
            // The config server may be a shard, so only add if it isn't already in participants.
            participants.emplace_back(shardRegistry->getConfigShard()->getId());
        }
        return participants;
    }();

    ParticipantCausalityBarrier{shardsAndConfigsvr, _executor, _token}.perform(opCtx, osi);
}

}  // namespace mongo
