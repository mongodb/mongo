/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/sharding_ddl_coordinator.h"

#include "mongo/db/s/database_sharding_state.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/s/grid.h"

namespace mongo {

ShardingDDLCoordinator_NORESILIENT::ShardingDDLCoordinator_NORESILIENT(OperationContext* opCtx,
                                                                       const NamespaceString& ns)
    : _nss(ns), _forwardableOpMetadata(opCtx){};

SemiFuture<void> ShardingDDLCoordinator_NORESILIENT::run(OperationContext* opCtx) {
    if (!_nss.isConfigDB()) {
        // Check that the operation context has a database version for this namespace
        const auto clientDbVersion = OperationShardingState::get(opCtx).getDbVersion(_nss.db());
        uassert(ErrorCodes::IllegalOperation,
                str::stream() << "Request sent without attaching database version",
                clientDbVersion);

        // Checks that this is the primary shard for the namespace's db
        const auto dbPrimaryShardId = [&]() {
            Lock::DBLock dbWriteLock(opCtx, _nss.db(), MODE_IS);
            auto dss = DatabaseShardingState::get(opCtx, _nss.db());
            auto dssLock = DatabaseShardingState::DSSLock::lockShared(opCtx, dss);
            // The following call will also ensure that the database version matches
            return dss->getDatabaseInfo(opCtx, dssLock).getPrimary();
        }();

        const auto thisShardId = ShardingState::get(opCtx)->shardId();

        uassert(ErrorCodes::IllegalOperation,
                str::stream() << "This is not the primary shard for db " << _nss.db()
                              << " expected: " << dbPrimaryShardId << " shardId: " << thisShardId,
                dbPrimaryShardId == thisShardId);
    }
    return runImpl(Grid::get(opCtx)->getExecutorPool()->getFixedExecutor());
}

}  // namespace mongo
