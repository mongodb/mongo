/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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


#include "mongo/db/global_catalog/ddl/clone_authoritative_metadata_coordinator.h"

#include "mongo/db/global_catalog/ddl/ddl_lock_manager.h"
#include "mongo/db/global_catalog/ddl/sharding_recovery_service.h"
#include "mongo/db/global_catalog/ddl/shardsvr_commit_create_database_metadata_command.h"
#include "mongo/db/local_catalog/shard_role_catalog/operation_sharding_state.h"
#include "mongo/db/local_catalog/shard_role_catalog/shard_filtering_metadata_refresh.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/executor/scoped_task_executor.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {

MONGO_FAIL_POINT_DEFINE(hangAfterEnterInShardRoleCloneAuthoritativeMetadataDDL);

ExecutorFuture<void> CloneAuthoritativeMetadataCoordinator::_runImpl(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token) noexcept {
    return ExecutorFuture<void>(**executor)
        .then(_buildPhaseHandler(
            Phase::kGetDatabasesToClone,
            [this, anchor = shared_from_this()](auto* opCtx) { _prepareDbsToClone(opCtx); }))
        .then(_buildPhaseHandler(
            Phase::kClone, [this, anchor = shared_from_this()](auto* opCtx) { _clone(opCtx); }));
}

void CloneAuthoritativeMetadataCoordinator::_prepareDbsToClone(OperationContext* opCtx) {
    // At this point, any new DDL operations first clone the metadata into the shard catalog
    // before starting the DDL. This means that we only need to clone metadata for DDL operations
    // that committed before the most recent config time.
    auto dbs = uassertStatusOK(Grid::get(opCtx)->catalogClient()->getDatabasesForShard(
        opCtx, ShardingState::get(opCtx)->shardId()));

    _doc.setDbsToClone(std::move(dbs));
}

void CloneAuthoritativeMetadataCoordinator::_clone(OperationContext* opCtx) {
    tassert(10644513,
            "Expected dbsToClone to be set on the coordinator document",
            _doc.getDbsToClone());
    const auto databasesToClone = *_doc.getDbsToClone();

    for (const auto& dbName : databasesToClone) {
        try {
            _cloneSingleDatabaseWithShardRole(opCtx, dbName);
        } catch (const ExceptionFor<ErrorCodes::StaleDbVersion>& ex) {
            auto extraInfo = ex.extraInfo<StaleDbRoutingVersion>();
            tassert(10050303, "StaleDbVersion must have extraInfo", extraInfo);

            auto wantedVersion = extraInfo->getVersionWanted();
            if (wantedVersion && *wantedVersion > extraInfo->getVersionReceived()) {
                // If there is a wanted version and this is newer than the received one, it
                // indicates that the routing information is stale. The database has either been
                // moved or dropped and recreated. In such cases, another DDL operation was
                // responsible for cloning, so we do not need to clone it anymore. This is because
                // any concurrent DDLs is already supposed to be authoritative at this stage.
                _removeDbFromCloningList(opCtx, dbName);
            } else {
                // If the shard is unaware of the database, perform a metadata refresh and retry.
                (void)FilteringMetadataCache::get(opCtx)->onDbVersionMismatch(
                    opCtx, extraInfo->getDb(), extraInfo->getVersionReceived());
                throw ex;
            }
        } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
            // If the database has been dropped, we remove it from the cloning list. If any
            // concurrent operations are attempting to recreate it, they will handle the cloning.
            _removeDbFromCloningList(opCtx, dbName);
        }
    }
}

void CloneAuthoritativeMetadataCoordinator::_cloneSingleDatabaseWithShardRole(
    OperationContext* opCtx, const DatabaseName& dbName) {
    auto csReason = BSON("cloneAuthoritativeMetadata" << DatabaseNameUtil::serialize(
                             dbName, SerializationContext::stateCommandRequest()));

    if (!_firstExecution) {
        ShardingRecoveryService::get(opCtx)->releaseRecoverableCriticalSection(
            opCtx,
            NamespaceString(dbName),
            csReason,
            ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter(),
            ShardingRecoveryService::NoCustomAction(),
            false /*throwIfReasonDiffers*/);
    }

    auto catalogClient = Grid::get(opCtx)->catalogClient();
    auto dbMetadata =
        catalogClient->getDatabase(opCtx, dbName, repl::ReadConcernLevel::kMajorityReadConcern);

    if (dbMetadata.getPrimary() != ShardingState::get(opCtx)->shardId()) {
        // If this shard is no longer the primary at the time of fetching metadata, we skip cloning
        // as it is now managed by another shard.
        _removeDbFromCloningList(opCtx, dbName);
        return;
    }

    ScopedSetShardRole scopedShardRole(
        opCtx, NamespaceString{dbName}, boost::none /* shardVersion */, dbMetadata.getVersion());

    if (MONGO_unlikely(hangAfterEnterInShardRoleCloneAuthoritativeMetadataDDL.shouldFail())) {
        hangAfterEnterInShardRoleCloneAuthoritativeMetadataDDL.executeIf(
            [&](const BSONObj&) {
                LOGV2(10050302,
                      "Hanging after entering shard role for cloning authoritative metadata DDL",
                      "dbName"_attr = dbName.toString_forTest());
                hangAfterEnterInShardRoleCloneAuthoritativeMetadataDDL.pauseWhileSet();
            },
            [&](const BSONObj& data) {
                const auto fpDbName = DatabaseNameUtil::parseFailPointData(data, "dbName");
                return dbName == fpDbName;
            });
    }

    DDLLockManager::ScopedDatabaseDDLLock dbLock(
        opCtx, dbName, "cloneAuthoritativeMetadata"_sd, MODE_IX);

    ShardingRecoveryService::get(opCtx)->acquireRecoverableCriticalSectionBlockWrites(
        opCtx,
        NamespaceString(dbName),
        csReason,
        ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter(),
        false /*clearDbMetadata*/);

    ShardingRecoveryService::get(opCtx)->promoteRecoverableCriticalSectionToBlockAlsoReads(
        opCtx,
        NamespaceString(dbName),
        csReason,
        ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter());

    commitCreateDatabaseMetadataLocally(opCtx, dbMetadata);

    ShardingRecoveryService::get(opCtx)->releaseRecoverableCriticalSection(
        opCtx,
        NamespaceString(dbName),
        csReason,
        ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter(),
        ShardingRecoveryService::NoCustomAction(),
        true /*throwIfReasonDiffers*/);
}

void CloneAuthoritativeMetadataCoordinator::_removeDbFromCloningList(OperationContext* opCtx,
                                                                     const DatabaseName& dbName) {
    {
        auto csReason = BSON("cloneAuthoritativeMetadata" << DatabaseNameUtil::serialize(
                                 dbName, SerializationContext::stateCommandRequest()));

        ShardingRecoveryService::get(opCtx)->releaseRecoverableCriticalSection(
            opCtx,
            NamespaceString(dbName),
            csReason,
            ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter(),
            ShardingRecoveryService::NoCustomAction(),
            false /*throwIfReasonDiffers*/);
    }

    auto dbs = *_doc.getDbsToClone();
    dbs.erase(std::remove(dbs.begin(), dbs.end(), dbName), dbs.end());
    _doc.setDbsToClone(std::move(dbs));
    _updateStateDocument(opCtx, StateDoc(_doc));
}

}  // namespace mongo
