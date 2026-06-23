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

#include "mongo/db/generic_argument_util.h"
#include "mongo/db/global_catalog/ddl/convert_shard_refs_in_namespace_metadata_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_util.h"
#include "mongo/db/global_catalog/ddl/shardsvr_commit_create_database_metadata_command.h"
#include "mongo/db/namespace_string_util.h"
#include "mongo/db/shard_role/ddl/ddl_lock_manager.h"
#include "mongo/db/shard_role/shard_catalog/database_sharding_runtime.h"
#include "mongo/db/shard_role/shard_catalog/operation_sharding_state.h"
#include "mongo/db/shard_role/shard_catalog/shard_filtering_metadata_refresh.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/topology/vector_clock/vector_clock_mutable.h"
#include "mongo/executor/scoped_task_executor.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
using namespace std::literals::string_view_literals;

MONGO_FAIL_POINT_DEFINE(hangAfterEnterInShardRoleCloneAuthoritativeMetadataDDL);

namespace {

std::vector<NamespaceString> getTrackedNamespaces(OperationContext* opCtx,
                                                  const DatabaseName& dbName) {
    auto collections = Grid::get(opCtx)->catalogClient()->getCollections(
        opCtx, dbName, repl::ReadConcernLevel::kMajorityReadConcern);
    std::vector<NamespaceString> nssList;
    nssList.reserve(collections.size());
    for (const auto& coll : collections) {
        nssList.push_back(coll.getNss());
    }
    return nssList;
}

}  // namespace

ExecutorFuture<void> CloneAuthoritativeMetadataCoordinator::_runImpl(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token) noexcept {
    return ExecutorFuture<void>(**executor)
        .then(_buildPhaseHandler(
            Phase::kGetDatabasesToClone,
            [this, anchor = shared_from_this()](auto* opCtx) { _prepareDbsToClone(opCtx); }))
        .then(_buildPhaseHandler(Phase::kClone,
                                 [this, token, anchor = shared_from_this(), executor](auto* opCtx) {
                                     _clone(opCtx, executor, token);
                                 }));
}

void CloneAuthoritativeMetadataCoordinator::_prepareDbsToClone(OperationContext* opCtx) {
    // Snapshot databases whose primary shard is the current shard; these are the databases this
    // coordinator will make authoritative.
    // This snapshot is taken before acquiring the DDL lock for these databases, so concurrent DDLs
    // (e.g. movePrimary, dropDatabase) may run between the snapshot and the lock acquisition.
    // This is safe because those DDLs are also designed to make those databases authoritative.
    auto dbs = uassertStatusOK(Grid::get(opCtx)->catalogClient()->getDatabasesForShard(
        opCtx, ShardingState::get(opCtx)->shardId()));

    _doc.setDbsToClone(std::move(dbs));
}

void CloneAuthoritativeMetadataCoordinator::_clone(
    OperationContext* opCtx,
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
    const CancellationToken& token) {
    tassert(10644513,
            "Expected dbsToClone to be set on the coordinator document",
            _doc.getDbsToClone());
    const auto databasesToClone = *_doc.getDbsToClone();

    for (const auto& dbName : databasesToClone) {
        try {
            _cloneSingleDatabaseWithShardRole(opCtx, dbName, executor, token);
            _removeDbFromCloningList(opCtx, dbName);
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
        }
    }
}

void CloneAuthoritativeMetadataCoordinator::_convertShardRefsInNamespaceMetadataInGlobalCatalog(
    OperationContext* opCtx, const NamespaceString& ns) {
    if (_doc.getShardIdentificationType() == ShardIdentificationTypeEnum::kShardId) {
        // The command is currently running under an FCV version that does not support the new
        // ShardRef format. Skip the conversion.
        return;
    }

    ConfigsvrConvertShardRefsInNamespaceMetadata cmd(
        NamespaceStringUtil::serialize(ns, SerializationContext::stateDefault()));
    cmd.setDbName(DatabaseName::kAdmin);
    generic_argument_util::setMajorityWriteConcern(cmd);

    uassertStatusOK(Shard::CommandResponse::getEffectiveStatus(
        Grid::get(opCtx)->shardRegistry()->getConfigShard()->runCommand(
            opCtx,
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            DatabaseName::kAdmin,
            cmd.toBSON(),
            Shard::RetryPolicy::kIdempotent)));
}

void CloneAuthoritativeMetadataCoordinator::_cloneSingleDatabaseWithShardRole(
    OperationContext* opCtx,
    const DatabaseName& dbName,
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
    const CancellationToken& token) {
    auto catalogClient = Grid::get(opCtx)->catalogClient();
    DatabaseType dbMetadata;
    try {
        dbMetadata =
            catalogClient->getDatabase(opCtx, dbName, repl::ReadConcernLevel::kMajorityReadConcern);
    } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
        // If the database has been dropped, we remove it from the cloning list. If any
        // concurrent operations are attempting to recreate it, they will handle the cloning.
        _removeDbFromCloningList(opCtx, dbName);
        return;
    }

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
        opCtx, dbName, "cloneAuthoritativeMetadata"sv, MODE_IX);

    {
        _convertShardRefsInNamespaceMetadataInGlobalCatalog(opCtx, NamespaceString(dbName));
        // CloneAuthoritativeMetadata can bypass the critical section when writing database metadata
        // because 1) we hold the DDL lock, which guarantees that no other conflicting DDL
        // operations are in progress and 2) the clone serves as a refresh from the config server,
        // which does not need to serialize with CRUD operations at the critical section level, but
        // instead synchronizes using the DSS mutex.
        BypassDatabaseMetadataAccess bypassDbMetadataAccess(
            opCtx, BypassDatabaseMetadataAccess::Type::kWriteOnly);  // NOLINT

        commitCreateDatabaseMetadataLocally(opCtx, dbMetadata, true /* fromClone */);
    }

    // Now that the database metadata is cloned, clone the metadata of its tracked collections by
    // instructing the shards owning data to persist it into their local shard catalog.
    for (const auto& nss : getTrackedNamespaces(opCtx, dbName)) {
        try {
            DDLLockManager::ScopedCollectionDDLLock collLock(
                opCtx, nss, "cloneAuthoritativeMetadata"sv, MODE_X);

            _convertShardRefsInNamespaceMetadataInGlobalCatalog(opCtx, nss);

            sharding_ddl_util::cloneAuthoritativeCollectionMetadataToShards(
                opCtx,
                nss,
                dbMetadata.getPrimary(),
                [&] { return getNewSession(opCtx); },
                _doc.getAuthoritativeMetadataAccessLevel(),
                executor,
                token);
        } catch (const ExceptionFor<ErrorCodes::RequestAlreadyFulfilled>&) {
            // The collection is no longer tracked (e.g. it was dropped after we listed it but
            // before taking the DDL lock), so there is nothing to clone. If any concurrent
            // operations are attempting to recreate it, they will handle the cloning.
        }
    }
}

void CloneAuthoritativeMetadataCoordinator::_removeDbFromCloningList(OperationContext* opCtx,
                                                                     const DatabaseName& dbName) {
    auto dbs = *_doc.getDbsToClone();
    dbs.erase(std::remove(dbs.begin(), dbs.end(), dbName), dbs.end());
    _doc.setDbsToClone(std::move(dbs));
    _updateStateDocument(opCtx, StateDoc(_doc));
}

bool CloneAuthoritativeMetadataCoordinator::isInCriticalSection(Phase phase) const {
    // No critical section is taken
    return false;
}

}  // namespace mongo
