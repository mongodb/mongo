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
#include "mongo/db/s/create_database_coordinator.h"
#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/db/s/create_database_util.h"
#include "mongo/s/routing_information_cache.h"

namespace mongo {

void CreateDatabaseCoordinator::_checkPreconditions() {
    auto opCtxHolder = cc().makeOperationContext();
    auto* opCtx = opCtxHolder.get();
    const auto& dbName = nss().dbName();
    if (const auto& existingDatabase =
            create_database_util::checkForExistingDatabaseWithDifferentOptions(
                opCtx, dbName, _primaryShard)) {
        _result = ConfigsvrCreateDatabaseResponse(existingDatabase->getVersion());
        // Launches an exception to directly jump to the end of the continuation chain.
        uasserted(ErrorCodes::RequestAlreadyFulfilled,
                  str::stream() << "The database" << dbName.toStringForErrorMsg()
                                << "is already created from a past request");
    }
}

ExecutorFuture<void> CreateDatabaseCoordinator::_cleanupOnAbort(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token,
    const Status& status) noexcept {
    return ExecutorFuture<void>(**executor);
}

ExecutorFuture<void> CreateDatabaseCoordinator::_runImpl(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token) noexcept {
    return ExecutorFuture<void>(**executor)
        .then([this, anchor = shared_from_this()] {
            if (_doc.getPhase() < Phase::kCommitOnShardingCatalog) {
                _checkPreconditions();
            }
        })
        .then(_buildPhaseHandler(
            Phase::kCommitOnShardingCatalog,
            [this, anchor = shared_from_this()] {
                auto opCtxHolder = cc().makeOperationContext();
                auto* opCtx = opCtxHolder.get();
                const auto& dbName = nss().dbName();
                const auto dbNameStr =
                    DatabaseNameUtil::serialize(dbName, SerializationContext::stateDefault());
                if (const auto& existingDatabase = create_database_util::findDatabaseExactMatch(
                        opCtx, dbNameStr, _primaryShard)) {
                    // This means the database was created in a previous run of the same create
                    // database coordinator instance. Just forces a refresh on the primary.
                    create_database_util::refreshDbVersionOnPrimaryShard(
                        opCtx, dbNameStr, existingDatabase->getPrimary());
                    _result = ConfigsvrCreateDatabaseResponse(existingDatabase->getVersion());
                } else {
                    const auto candidatePrimaryShardId =
                        create_database_util::getCandidatePrimaryShard(opCtx, _primaryShard);
                    auto createdDatabase = ShardingCatalogManager::get(opCtx)->commitCreateDatabase(
                        opCtx, dbName, candidatePrimaryShardId);

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
                    create_database_util::refreshDbVersionOnPrimaryShard(
                        opCtx, dbNameStr, createdDatabase.getPrimary());

                    _result = ConfigsvrCreateDatabaseResponse(createdDatabase.getVersion());
                }
            }))
        .onCompletion([this, anchor = shared_from_this()](const Status& status) {
            auto opCtxHolder = cc().makeOperationContext();
            auto* opCtx = opCtxHolder.get();
            const auto& dbName = nss().dbName();
            RoutingInformationCache::get(opCtx)->purgeDatabase(dbName);
            return status;
        })
        .onError([this, anchor = shared_from_this()](const Status& status) {
            if (status == ErrorCodes::RequestAlreadyFulfilled) {
                return Status::OK();
            }

            if (_doc.getPhase() < Phase::kCommitOnShardingCatalog) {
                // Early exit to not trigger the clean up procedure because the commit phase hasn't
                // started yet.
                return status;
            }

            const auto opCtxHolder = cc().makeOperationContext();
            auto* opCtx = opCtxHolder.get();
            getForwardableOpMetadata().setOn(opCtx);
            const auto& dbName = nss().dbName();

            // The proposed primary shard was found to not exist or be draining when attempting to
            // commit.
            if (status == ErrorCodes::ShardNotFound) {
                create_database_util::logCommitCreateDatabaseFailed(dbName, status.reason());
                if (_primaryShard) {
                    // If a primary shard was explicitly selected by the caller, then fail the
                    // create operation.
                    triggerCleanup(opCtx, status);
                    MONGO_UNREACHABLE;
                } else {
                    // If no primary shard was explicitly selected by the caller, then retry to
                    // choose a new one.
                    if (--_availableRetries > 0) {
                        StateDoc newDoc(_doc);
                        newDoc.setAvailableRetries(_availableRetries);
                        _updateStateDocument(opCtx, std::move(newDoc));
                    } else {
                        create_database_util::logCreateDatabaseRetryExhausted(dbName);
                        triggerCleanup(opCtx, status);
                        MONGO_UNREACHABLE;
                    }
                }
            }

            return status;
        });
}

ConfigsvrCreateDatabaseResponse CreateDatabaseCoordinator::getResult(OperationContext* opCtx) {
    getCompletionFuture().get(opCtx);
    invariant(_result.is_initialized());
    return *_result;
}

}  // namespace mongo
