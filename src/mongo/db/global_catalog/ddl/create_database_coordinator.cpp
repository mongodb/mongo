/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/global_catalog/ddl/create_database_coordinator.h"

#include "mongo/db/generic_argument_util.h"
#include "mongo/db/global_catalog/ddl/create_database_util.h"
#include "mongo/db/global_catalog/ddl/sharding_catalog_manager.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_util.h"
#include "mongo/db/global_catalog/ddl/sharding_util.h"
#include "mongo/db/local_catalog/shard_role_catalog/participant_block_gen.h"
#include "mongo/executor/async_rpc.h"

namespace mongo {

namespace {
void checkIfDropPendingDB(OperationContext* opCtx, const DatabaseName& dbName) {
    // Fails and retries if a previous dropDatabase is not complete yet.
    uassert(ErrorCodes::DatabaseDropPending,
            fmt::format("Database {} is being dropped. Will automatically retry",
                        dbName.toStringForErrorMsg()),
            create_database_util::checkIfDropPendingDB(opCtx, dbName));
}

void refreshDatabaseCache(OperationContext* opCtx,
                          const DatabaseName& dbName,
                          const ShardId& primaryShard) {
    const auto dbNameStr =
        DatabaseNameUtil::serialize(dbName, SerializationContext::stateDefault());
    // Note, making the primary shard refresh its databaseVersion here is not
    // required for correctness, since either:
    // 1) This is the first time this database is being created. The primary shard
    //    will not have a databaseVersion already cached.
    // 2) The database was dropped and is being re-created. Since dropping a
    //    database also sends _flushDatabaseCacheUpdates to all shards, the primary
    //    shard should not have a database version cached. (Note, it is possible
    //    that dropping a database will skip sending _flushDatabaseCacheUpdates if
    //    the config server fails over while dropping the database.)
    // However, routers don't support retrying internally on StaleDbVersion in
    // transactions (SERVER-39704), so if the first operation run against the
    // database is in a transaction, it would fail with StaleDbVersion. Making the
    // primary shard refresh here allows that first transaction to succeed. This
    // allows our transaction passthrough suites and transaction demos to succeed
    // without additional special logic.
    create_database_util::refreshDbVersionOnPrimaryShard(opCtx, dbNameStr, primaryShard);
}
}  // namespace

void CreateDatabaseCoordinator::_checkPreconditions() {
    auto opCtxHolder = makeOperationContext();
    auto* opCtx = opCtxHolder.get();
    const auto& dbName = nss().dbName();

    const auto& primaryShard = _doc.getPrimaryShard();
    if (const auto& existingDatabase =
            create_database_util::checkForExistingDatabaseWithDifferentOptions(
                opCtx, dbName, primaryShard)) {
        _result = ConfigsvrCreateDatabaseResponse(existingDatabase->getVersion());
        // Launches an exception to directly jump to the end of the continuation chain.
        uasserted(ErrorCodes::RequestAlreadyFulfilled,
                  str::stream() << "The database" << dbName.toStringForErrorMsg()
                                << "is already created from a past request");
    }
    // Checks if the user-specified primary shard exists.
    if (primaryShard) {
        uassertStatusOK(Grid::get(opCtx)->shardRegistry()->getShard(opCtx, primaryShard.get()));
    }
}

ExecutorFuture<void> CreateDatabaseCoordinator::_cleanupOnAbort(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token,
    const Status& status) noexcept {
    return ExecutorFuture<void>(**executor)
        .then([this, token, executor = executor, anchor = shared_from_this()] {
            // Exits the critical section if the shard selected by the user is found to be draining
            // when committing the database creation.
            if (_doc.getPhase() == Phase::kCommitOnShardingCatalog) {
                auto opCtxHolder = makeOperationContext();
                auto* opCtx = opCtxHolder.get();

                _exitCriticalSection(opCtx, executor, token, true /* throwIfReasonDiffers */);
            }
        });
}

void CreateDatabaseCoordinator::_setupPrimaryShard(OperationContext* opCtx) {
    if (_doc.getPrimaryShard()) {
        return;
    }
    // Selects the primary candidate shard if not specified by the user or selected by the
    // coordinator already. Persists the primary shard information for later phases.
    StateDoc newDoc(_doc);
    newDoc.setPrimaryShard(sharding_util::selectLeastLoadedNonDrainingShard(opCtx));
    _updateStateDocument(opCtx, std::move(newDoc));
}

void CreateDatabaseCoordinator::_enterCriticalSection(
    OperationContext* opCtx,
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token) {
    ShardsvrParticipantBlock blockCRUDOperationsRequest(
        NamespaceString::makeCollectionlessShardsvrParticipantBlockNSS(nss().dbName()));
    blockCRUDOperationsRequest.setBlockType(mongo::CriticalSectionBlockTypeEnum::kReadsAndWrites);
    blockCRUDOperationsRequest.setReason(_critSecReason);
    blockCRUDOperationsRequest.setClearDbInfo(_doc.getAuthoritativeMetadataAccessLevel() ==
                                              AuthoritativeMetadataAccessLevelEnum::kNone);

    generic_argument_util::setMajorityWriteConcern(blockCRUDOperationsRequest);
    generic_argument_util::setOperationSessionInfo(blockCRUDOperationsRequest,
                                                   getNewSession(opCtx));
    auto opts = std::make_shared<async_rpc::AsyncRPCOptions<ShardsvrParticipantBlock>>(
        **executor, token, blockCRUDOperationsRequest);
    sharding_ddl_util::sendAuthenticatedCommandToShards(
        opCtx, opts, {_doc.getPrimaryShard().get()});
}

void CreateDatabaseCoordinator::_storeDBVersion(OperationContext* opCtx, const DatabaseType& db) {
    if (_doc.getDatabaseVersion()) {
        return;
    }

    StateDoc newDoc(_doc);
    newDoc.setDatabaseVersion(db.getVersion());
    _updateStateDocument(opCtx, std::move(newDoc));
}

void CreateDatabaseCoordinator::_exitCriticalSection(
    OperationContext* opCtx,
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token,
    bool throwIfReasonDiffers) {
    ShardsvrParticipantBlock unblockCRUDOperationsRequest(
        NamespaceString::makeCollectionlessShardsvrParticipantBlockNSS(nss().dbName()));
    unblockCRUDOperationsRequest.setBlockType(CriticalSectionBlockTypeEnum::kUnblock);
    unblockCRUDOperationsRequest.setReason(_critSecReason);
    unblockCRUDOperationsRequest.setThrowIfReasonDiffers(throwIfReasonDiffers);
    unblockCRUDOperationsRequest.setClearDbInfo(_doc.getAuthoritativeMetadataAccessLevel() ==
                                                AuthoritativeMetadataAccessLevelEnum::kNone);

    generic_argument_util::setMajorityWriteConcern(unblockCRUDOperationsRequest);
    generic_argument_util::setOperationSessionInfo(unblockCRUDOperationsRequest,
                                                   getNewSession(opCtx));
    auto opts = std::make_shared<async_rpc::AsyncRPCOptions<ShardsvrParticipantBlock>>(
        **executor, token, unblockCRUDOperationsRequest);
    sharding_ddl_util::sendAuthenticatedCommandToShards(
        opCtx, opts, {_doc.getPrimaryShard().get()});
}

DatabaseType CreateDatabaseCoordinator::_commitClusterCatalog(OperationContext* opCtx) {
    const auto& dbName = nss().dbName();
    const auto dbNameStr =
        DatabaseNameUtil::serialize(dbName, SerializationContext::stateDefault());

    if (const auto& existingDatabase = create_database_util::findDatabaseExactMatch(
            opCtx, dbNameStr, _doc.getPrimaryShard())) {
        // This means the database was created in a previous run of the same create
        // database coordinator instance.
        return existingDatabase.get();
    }
    return ShardingCatalogManager::get(opCtx)->commitCreateDatabase(
        opCtx, dbName, _doc.getPrimaryShard().get(), _doc.getUserSelectedPrimary());
}

ExecutorFuture<void> CreateDatabaseCoordinator::_runImpl(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token) noexcept {
    return ExecutorFuture<void>(**executor)
        .then([this, anchor = shared_from_this()] {
            if (_doc.getPhase() < Phase::kEnterCriticalSectionOnPrimary) {
                _checkPreconditions();
            }
        })
        .then(_buildPhaseHandler(
            Phase::kEnterCriticalSectionOnPrimary,
            [this, token, executor = executor, anchor = shared_from_this()](auto* opCtx) {
                checkIfDropPendingDB(opCtx, nss().dbName());
                _setupPrimaryShard(opCtx);
                _enterCriticalSection(opCtx, executor, token);
            }))
        .then(_buildPhaseHandler(
            Phase::kCommitOnShardingCatalog,
            [this, token, executor = executor, anchor = shared_from_this()](auto* opCtx) {
                auto db = _commitClusterCatalog(opCtx);

                if (_doc.getAuthoritativeMetadataAccessLevel() >=
                    AuthoritativeMetadataAccessLevelEnum::kWritesAllowed) {
                    const auto& session = getNewSession(opCtx);
                    sharding_ddl_util::commitCreateDatabaseMetadataToShardCatalog(
                        opCtx, db, session, executor, token);
                }

                // Persists the metadata of the created database on the coordinator doc.
                _storeDBVersion(opCtx, db);
                _result = ConfigsvrCreateDatabaseResponse(db.getVersion());
            }))
        .then(_buildPhaseHandler(
            Phase::kExitCriticalSectionOnPrimary,
            [this, token, executor = executor, anchor = shared_from_this()](auto* opCtx) {
                const auto& dbName = nss().dbName();
                // Populates the result if the coordinator was rebuilt after the commit phase.
                if (!_result.is_initialized()) {
                    const auto& dbVersion = getDatabaseVersion();
                    tassert(10644531,
                            "Expected databaseVersion to be initialized",
                            dbVersion.is_initialized());
                    _result = ConfigsvrCreateDatabaseResponse(dbVersion.get());
                }
                _exitCriticalSection(opCtx, executor, token, false /* throwIfReasonDiffers */);
                if (_doc.getAuthoritativeMetadataAccessLevel() ==
                    AuthoritativeMetadataAccessLevelEnum::kNone) {
                    refreshDatabaseCache(opCtx, dbName, _doc.getPrimaryShard().get());
                }
            }))
        .onError([this, anchor = shared_from_this()](const Status& status) {
            if (status == ErrorCodes::RequestAlreadyFulfilled) {
                return Status::OK();
            }

            if (_doc.getPhase() < Phase::kEnterCriticalSectionOnPrimary) {
                // Early exit to not trigger the clean up procedure because the coordinator has
                // not entered to any critical section.
                return status;
            }

            const auto opCtxHolder = makeOperationContext();
            auto* opCtx = opCtxHolder.get();
            const auto& dbName = nss().dbName();

            // We may get 'ShardNotFound' in two scenarios:
            // 1. The coordinator tries to select a primary shard but finds no shards.
            // 2. The user-selected primary shard is draining.
            // For both cases, we'll fail the operation.
            if (status == ErrorCodes::ShardNotFound) {
                create_database_util::logCommitCreateDatabaseFailed(dbName, status.reason());
                triggerCleanup(opCtx, status);
                MONGO_UNREACHABLE_TASSERT(10083522);
            }

            return status;
        });
}

ConfigsvrCreateDatabaseResponse CreateDatabaseCoordinator::getResult(OperationContext* opCtx) {
    getCompletionFuture().get(opCtx);
    tassert(10644532, "Expected _result to be initialized", _result.is_initialized());
    return *_result;
}

}  // namespace mongo
