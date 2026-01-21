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
