// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/shard_role/shard_catalog/critical_section_signal.h"

#include "mongo/db/sharding_environment/sharding_statistics.h"

namespace mongo {

void CriticalSectionSignal::get(OperationContext* opCtx) const {
    auto token = [&] {
        switch (_type) {
            case CriticalSectionType::Database:
                return ShardingStatistics::get(opCtx->getServiceContext())
                    .databaseCriticalSectionStatistics.startWaiter();
            case CriticalSectionType::Collection:
                return ShardingStatistics::get(opCtx->getServiceContext())
                    .collectionCriticalSectionStatistics.startWaiter();
            default:
                MONGO_UNREACHABLE;
        }
    }();
    ON_BLOCK_EXIT([&] {
        switch (_type) {
            case CriticalSectionType::Database:
                ShardingStatistics::get(opCtx->getServiceContext())
                    .databaseCriticalSectionStatistics.finishWaiter(token);
                break;
            case CriticalSectionType::Collection:
                ShardingStatistics::get(opCtx->getServiceContext())
                    .collectionCriticalSectionStatistics.finishWaiter(token);
                break;
            default:
                MONGO_UNREACHABLE;
        }
    });
    _signal.get(opCtx);
}
}  // namespace mongo
