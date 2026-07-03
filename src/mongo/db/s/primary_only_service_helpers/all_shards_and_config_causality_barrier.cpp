/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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
