/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/s/move_primary_coordinator.h"

#include <algorithm>

#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/connpool.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/commands/list_collections_filter.h"
#include "mongo/db/repl/change_stream_oplog_notification.h"
#include "mongo/db/s/database_sharding_state.h"
#include "mongo/db/s/sharding_ddl_util.h"
#include "mongo/db/s/sharding_logging.h"
#include "mongo/db/s/sharding_recovery_service.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/vector_clock_mutable.h"
#include "mongo/db/write_block_bypass.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/move_primary_gen.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {

MONGO_FAIL_POINT_DEFINE(hangBeforeCloningCatalogData);
MONGO_FAIL_POINT_DEFINE(hangBeforeCleaningStaleData);

MovePrimaryCoordinator::MovePrimaryCoordinator(ShardingDDLCoordinatorService* service,
                                               const BSONObj& initialState)
    : RecoverableShardingDDLCoordinator(service, "MovePrimaryCoordinator", initialState),
      _dbName(nss().dbName()),
      _csReason([&] {
          BSONObjBuilder builder;
          builder.append("command", "movePrimary");
          builder.append("db", _dbName.toString());
          builder.append("to", _doc.getToShardId());
          return builder.obj();
      }()) {}

bool MovePrimaryCoordinator::canAlwaysStartWhenUserWritesAreDisabled() const {
    return true;
}

StringData MovePrimaryCoordinator::serializePhase(const Phase& phase) const {
    return MovePrimaryCoordinatorPhase_serializer(phase);
}

void MovePrimaryCoordinator::appendCommandInfo(BSONObjBuilder* cmdInfoBuilder) const {
    stdx::lock_guard lk(_docMutex);
    cmdInfoBuilder->append(
        "request",
        BSON(MovePrimaryCoordinatorDocument::kToShardIdFieldName << _doc.getToShardId()));
};

void MovePrimaryCoordinator::checkIfOptionsConflict(const BSONObj& doc) const {
    const auto otherDoc = MovePrimaryCoordinatorDocument::parse(
        IDLParserContext("MovePrimaryCoordinatorDocument"), doc);
    uassert(ErrorCodes::ConflictingOperationInProgress,
            "Another movePrimary operation with different arguments is already running ont the "
            "same database",
            _doc.getToShardId() == otherDoc.getToShardId());
}

ExecutorFuture<void> MovePrimaryCoordinator::_runImpl(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token) noexcept {
    return ExecutorFuture<void>(**executor)
        .then([this, executor, token, anchor = shared_from_this()] {
            const auto opCtxHolder = cc().makeOperationContext();
            auto* opCtx = opCtxHolder.get();
            getForwardableOpMetadata().setOn(opCtx);

            if (_doc.getToShardId() == ShardingState::get(opCtx)->shardId()) {
                LOGV2(7120200,
                      "Database already on requested primary shard",
                      "db"_attr = _dbName,
                      "to"_attr = _doc.getToShardId());

                return ExecutorFuture<void>(**executor);
            }

            return runMovePrimaryWorkflow(executor, token);
        });
}

ExecutorFuture<void> MovePrimaryCoordinator::runMovePrimaryWorkflow(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token) noexcept {
    return ExecutorFuture<void>(**executor)
        .then(_buildPhaseHandler(
            Phase::kClone,
            [this, executor = executor, anchor = shared_from_this()] {
                const auto opCtxHolder = cc().makeOperationContext();
                auto* opCtx = opCtxHolder.get();
                getForwardableOpMetadata().setOn(opCtx);

                LOGV2(7120201,
                      "Running movePrimary operation",
                      "db"_attr = _dbName,
                      "to"_attr = _doc.getToShardId());

                logChange(opCtx, "start");

                ScopeGuard unblockWritesLegacyOnExit([&] {
                    // TODO (SERVER-71444): Fix to be interruptible or document exception.
                    UninterruptibleLockGuard noInterrupt(opCtx->lockState());  // NOLINT
                    unblockWritesLegacy(opCtx);
                });

                if (!_firstExecution) {
                    // The previous execution failed with a retryable error and the recipient may
                    // have cloned part of the data. Orphaned data on recipient must be dropped and
                    // the `movePrimary` operation must fail in order to delegate to the caller the
                    // decision to retry the operation.
                    dropOrphanedDataOnRecipient(opCtx, executor);

                    uasserted(
                        7120202,
                        "movePrimary operation on database {} failed cloning data to recipient"_format(
                            _dbName.toString()));
                }

                blockWritesLegacy(opCtx);

                if (MONGO_unlikely(hangBeforeCloningCatalogData.shouldFail())) {
                    LOGV2(7120203, "Hit hangBeforeCloningCatalogData");
                    hangBeforeCloningCatalogData.pauseWhileSet(opCtx);
                }

                const auto& collectionsToClone = getUnshardedCollections(opCtx);
                assertNoOrphanedDataOnRecipient(opCtx, collectionsToClone);

                _doc.setCollectionsToClone(collectionsToClone);
                _updateStateDocument(opCtx, StateDoc(_doc));

                const auto cloneResponse = cloneDataToRecipient(opCtx);
                const auto cloneStatus = Shard::CommandResponse::getEffectiveStatus(cloneResponse);
                if (!cloneStatus.isOK() || !checkClonedData(cloneResponse.getValue())) {
                    dropOrphanedDataOnRecipient(opCtx, executor);

                    uasserted(
                        cloneStatus.isOK() ? 7120204 : cloneStatus.code(),
                        "movePrimary operation on database {} failed cloning data to recipient"_format(
                            _dbName.toString()));
                }

                // TODO (SERVER-71566): Temporary solution to cover the case of stepping down before
                // actually entering the `kCatchup` phase.
                blockWrites(opCtx);
            }))
        .then(_buildPhaseHandler(Phase::kCatchup,
                                 [this, anchor = shared_from_this()] {
                                     const auto opCtxHolder = cc().makeOperationContext();
                                     auto* opCtx = opCtxHolder.get();
                                     getForwardableOpMetadata().setOn(opCtx);

                                     blockWrites(opCtx);
                                 }))
        .then(_buildPhaseHandler(Phase::kEnterCriticalSection,
                                 [this, anchor = shared_from_this()] {
                                     const auto opCtxHolder = cc().makeOperationContext();
                                     auto* opCtx = opCtxHolder.get();
                                     getForwardableOpMetadata().setOn(opCtx);

                                     blockReads(opCtx);
                                 }))
        .then(_buildPhaseHandler(
            Phase::kCommit,
            [this, anchor = shared_from_this()] {
                const auto opCtxHolder = cc().makeOperationContext();
                auto* opCtx = opCtxHolder.get();
                getForwardableOpMetadata().setOn(opCtx);

                invariant(_doc.getDatabaseVersion());
                const auto& preCommitDbVersion = *_doc.getDatabaseVersion();

                const auto commitResponse = commitMetadataToConfig(opCtx, preCommitDbVersion);
                if (commitResponse == ErrorCodes::ShardNotFound) {
                    unblockReadsAndWrites(opCtx);
                }
                uassertStatusOKWithContext(
                    Shard::CommandResponse::getEffectiveStatus(commitResponse),
                    "movePrimary operation on database {} failed to commit metadata changes"_format(
                        _dbName.toString()));

                assertChangedMetadataOnConfig(opCtx, preCommitDbVersion);

                notifyChangeStreamsOnMovePrimary(
                    opCtx, _dbName, ShardingState::get(opCtx)->shardId(), _doc.getToShardId());

                // Checkpoint the vector clock to ensure causality in the event of a crash or
                // shutdown.
                VectorClockMutable::get(opCtx)->waitForDurableConfigTime().get(opCtx);

                clearDbMetadataOnPrimary(opCtx);

                logChange(opCtx, "commit");
            }))
        .then(_buildPhaseHandler(Phase::kExitCriticalSection,
                                 [this, anchor = shared_from_this()] {
                                     const auto opCtxHolder = cc().makeOperationContext();
                                     auto* opCtx = opCtxHolder.get();
                                     getForwardableOpMetadata().setOn(opCtx);

                                     unblockReadsAndWrites(opCtx);
                                 }))
        .then(_buildPhaseHandler(Phase::kClean,
                                 [this, anchor = shared_from_this()] {
                                     const auto opCtxHolder = cc().makeOperationContext();
                                     auto* opCtx = opCtxHolder.get();
                                     getForwardableOpMetadata().setOn(opCtx);

                                     if (MONGO_unlikely(hangBeforeCleaningStaleData.shouldFail())) {
                                         LOGV2(7120205, "Hit hangBeforeCleaningStaleData");
                                         hangBeforeCleaningStaleData.pauseWhileSet(opCtx);
                                     }

                                     dropStaleDataOnDonor(opCtx);

                                     LOGV2(7120206,
                                           "Completed movePrimary operation",
                                           "db"_attr = _dbName,
                                           "to"_attr = _doc.getToShardId());

                                     logChange(opCtx, "end");
                                 }))
        .onError([this, anchor = shared_from_this()](const Status& status) {
            const auto opCtxHolder = cc().makeOperationContext();
            auto* opCtx = opCtxHolder.get();
            getForwardableOpMetadata().setOn(opCtx);

            LOGV2_ERROR(7120207,
                        "Failed movePrimary operation",
                        "db"_attr = _dbName,
                        "to"_attr = _doc.getToShardId(),
                        "error"_attr = redact(status));

            logChange(opCtx, "error");

            return status;
        });
}

void MovePrimaryCoordinator::logChange(OperationContext* opCtx, const std::string& what) const {
    BSONObjBuilder details;
    details.append("from", ShardingState::get(opCtx)->shardId());
    details.append("to", _doc.getToShardId());
    ShardingLogging::get(opCtx)->logChange(
        opCtx, "movePrimary.{}"_format(what), _dbName.toString(), details.obj());
}

std::vector<NamespaceString> MovePrimaryCoordinator::getUnshardedCollections(
    OperationContext* opCtx) {
    const auto allCollections = [&] {
        DBDirectClient dbClient(opCtx);
        const auto collInfos =
            dbClient.getCollectionInfos(_dbName, ListCollectionsFilter::makeTypeCollectionFilter());

        std::vector<NamespaceString> colls;
        for (const auto& collInfo : collInfos) {
            std::string collName;
            uassertStatusOK(bsonExtractStringField(collInfo, "name", &collName));

            const NamespaceString nss(_dbName, collName);
            if (!nss.isSystem() ||
                nss.isLegalClientSystemNS(serverGlobalParams.featureCompatibility)) {
                colls.push_back(nss);
            }
        }

        std::sort(colls.begin(), colls.end());
        return colls;
    }();

    const auto shardedCollections = [&] {
        auto colls = Grid::get(opCtx)->catalogClient()->getAllShardedCollectionsForDb(
            opCtx, _dbName.toString(), repl::ReadConcernLevel::kMajorityReadConcern);

        std::sort(colls.begin(), colls.end());
        return colls;
    }();

    std::vector<NamespaceString> unshardedCollections;
    std::set_difference(allCollections.cbegin(),
                        allCollections.cend(),
                        shardedCollections.cbegin(),
                        shardedCollections.cend(),
                        std::back_inserter(unshardedCollections));

    return unshardedCollections;
}

void MovePrimaryCoordinator::assertNoOrphanedDataOnRecipient(
    OperationContext* opCtx, const std::vector<NamespaceString>& collectionsToClone) const {
    auto allCollections = [&] {
        const auto shardRegistry = Grid::get(opCtx)->shardRegistry();
        const auto toShard = uassertStatusOK(shardRegistry->getShard(opCtx, _doc.getToShardId()));

        const auto listCommand = [&] {
            BSONObjBuilder commandBuilder;
            commandBuilder.append("listCollections", 1);
            commandBuilder.append("filter", ListCollectionsFilter::makeTypeCollectionFilter());
            return commandBuilder.obj();
        }();

        const auto listResponse = uassertStatusOK(
            toShard->runExhaustiveCursorCommand(opCtx,
                                                ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                                                _dbName.toString(),
                                                listCommand,
                                                Milliseconds(-1)));

        std::vector<NamespaceString> colls;
        for (const auto& bsonColl : listResponse.docs) {
            std::string collName;
            uassertStatusOK(bsonExtractStringField(bsonColl, "name", &collName));
            colls.push_back({_dbName, collName});
        }

        std::sort(colls.begin(), colls.end());
        return colls;
    }();

    for (const auto& nss : collectionsToClone) {
        uassert(ErrorCodes::NamespaceExists,
                "Found orphaned collection {} on recipient"_format(nss.toString()),
                !std::binary_search(allCollections.cbegin(), allCollections.cend(), nss));
    };
}

StatusWith<Shard::CommandResponse> MovePrimaryCoordinator::cloneDataToRecipient(
    OperationContext* opCtx) const {
    // Enable write blocking bypass to allow cloning of catalog data even if writes are disallowed.
    WriteBlockBypass::get(opCtx).set(true);

    const auto shardRegistry = Grid::get(opCtx)->shardRegistry();
    const auto fromShard =
        uassertStatusOK(shardRegistry->getShard(opCtx, ShardingState::get(opCtx)->shardId()));
    const auto toShard = uassertStatusOK(shardRegistry->getShard(opCtx, _doc.getToShardId()));

    const auto cloneCommand = [&] {
        BSONObjBuilder commandBuilder;
        commandBuilder.append("_shardsvrCloneCatalogData", _dbName.toString());
        commandBuilder.append("from", fromShard->getConnString().toString());
        return commandBuilder.obj();
    }();

    return toShard->runCommandWithFixedRetryAttempts(
        opCtx,
        ReadPreferenceSetting(ReadPreference::PrimaryOnly),
        NamespaceString::kAdminDb.toString(),
        CommandHelpers::appendMajorityWriteConcern(cloneCommand),
        Shard::RetryPolicy::kNotIdempotent);
}

bool MovePrimaryCoordinator::checkClonedData(Shard::CommandResponse cloneResponse) const {
    invariant(_doc.getCollectionsToClone());
    const auto& collectionToClone = *_doc.getCollectionsToClone();

    const auto clonedCollections = [&] {
        std::vector<NamespaceString> colls;
        for (const auto& bsonElem : cloneResponse.response["clonedColls"].Obj()) {
            if (bsonElem.type() == String) {
                colls.push_back(NamespaceString(bsonElem.String()));
            }
        }

        std::sort(colls.begin(), colls.end());
        return colls;
    }();

    return collectionToClone.size() == clonedCollections.size() &&
        std::equal(
               collectionToClone.cbegin(), collectionToClone.cend(), clonedCollections.cbegin());
}

StatusWith<Shard::CommandResponse> MovePrimaryCoordinator::commitMetadataToConfig(
    OperationContext* opCtx, const DatabaseVersion& preCommitDbVersion) const {
    const auto commitCommand = [&] {
        ConfigsvrCommitMovePrimary commitRequest(_dbName, preCommitDbVersion, _doc.getToShardId());
        commitRequest.setDbName(NamespaceString::kAdminDb);
        return commitRequest.toBSON({});
    }();

    const auto config = Grid::get(opCtx)->shardRegistry()->getConfigShard();
    return config->runCommandWithFixedRetryAttempts(
        opCtx,
        ReadPreferenceSetting(ReadPreference::PrimaryOnly),
        NamespaceString::kAdminDb.toString(),
        CommandHelpers::appendMajorityWriteConcern(commitCommand),
        Shard::RetryPolicy::kIdempotent);
}

void MovePrimaryCoordinator::assertChangedMetadataOnConfig(
    OperationContext* opCtx, const DatabaseVersion& preCommitDbVersion) const {
    const auto postCommitDbType = [&]() {
        const auto config = Grid::get(opCtx)->shardRegistry()->getConfigShard();
        auto findResponse = uassertStatusOK(
            config->exhaustiveFindOnConfig(opCtx,
                                           ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                           repl::ReadConcernLevel::kMajorityReadConcern,
                                           NamespaceString::kConfigDatabasesNamespace,
                                           BSON(DatabaseType::kNameFieldName << _dbName.toString()),
                                           BSONObj(),
                                           1));

        const auto databases = std::move(findResponse.docs);
        uassert(ErrorCodes::IncompatibleShardingMetadata,
                "Tried to find version for database {}, but found no databases"_format(
                    _dbName.toString()),
                !databases.empty());

        return DatabaseType::parse(IDLParserContext("DatabaseType"), databases.front());
    }();
    tassert(7120208,
            "Error committing movePrimary: database version went backwards",
            postCommitDbType.getVersion() > preCommitDbVersion);
    uassert(7120209,
            "Error committing movePrimary: update of config.databases failed",
            postCommitDbType.getPrimary() != ShardingState::get(opCtx)->shardId());
}

void MovePrimaryCoordinator::clearDbMetadataOnPrimary(OperationContext* opCtx) const {
    AutoGetDb autoDb(opCtx, _dbName, MODE_X);
    auto scopedDss = DatabaseShardingState::assertDbLockedAndAcquire(
        opCtx, _dbName, DSSAcquisitionMode::kExclusive);
    scopedDss->clearDbInfo(opCtx);
}

void MovePrimaryCoordinator::dropStaleDataOnDonor(OperationContext* opCtx) const {
    // Enable write blocking bypass to allow cleaning of stale data even if writes are disallowed.
    WriteBlockBypass::get(opCtx).set(true);

    DBDirectClient dbClient(opCtx);
    invariant(_doc.getCollectionsToClone());
    for (const auto& nss : *_doc.getCollectionsToClone()) {
        const auto dropStatus = [&] {
            BSONObj dropResult;
            dbClient.runCommand(_dbName, BSON("drop" << nss.coll()), dropResult);
            return getStatusFromCommandResult(dropResult);
        }();

        if (!dropStatus.isOK()) {
            LOGV2_WARNING(7120210,
                          "Failed to drop stale collection on donor",
                          "namespace"_attr = nss,
                          "error"_attr = redact(dropStatus));
        }
    }
}

void MovePrimaryCoordinator::dropOrphanedDataOnRecipient(
    OperationContext* opCtx, std::shared_ptr<executor::ScopedTaskExecutor> executor) {
    if (!_doc.getCollectionsToClone()) {
        // A retryable error occurred before to persist the collections to clone, consequently no
        // data has been cloned yet.
        return;
    }

    // Make a copy of this container since `_updateSession` changes the coordinator document.
    const auto collectionsToClone = *_doc.getCollectionsToClone();
    for (const auto& nss : collectionsToClone) {
        _updateSession(opCtx);
        sharding_ddl_util::sendDropCollectionParticipantCommandToShards(opCtx,
                                                                        nss,
                                                                        {_doc.getToShardId()},
                                                                        **executor,
                                                                        getCurrentSession(),
                                                                        false /* fromMigrate */);
    }
}

void MovePrimaryCoordinator::blockWritesLegacy(OperationContext* opCtx) const {
    AutoGetDb autoDb(opCtx, _dbName, MODE_X);
    auto scopedDss = DatabaseShardingState::assertDbLockedAndAcquire(
        opCtx, _dbName, DSSAcquisitionMode::kExclusive);
    scopedDss->setMovePrimaryInProgress(opCtx);
}

void MovePrimaryCoordinator::unblockWritesLegacy(OperationContext* opCtx) const {
    AutoGetDb autoDb(opCtx, _dbName, MODE_IX);
    auto scopedDss = DatabaseShardingState::assertDbLockedAndAcquire(
        opCtx, _dbName, DSSAcquisitionMode::kExclusive);
    scopedDss->unsetMovePrimaryInProgress(opCtx);
}

void MovePrimaryCoordinator::blockWrites(OperationContext* opCtx) const {
    ShardingRecoveryService::get(opCtx)->acquireRecoverableCriticalSectionBlockWrites(
        opCtx, NamespaceString(_dbName), _csReason, ShardingCatalogClient::kLocalWriteConcern);
}

void MovePrimaryCoordinator::blockReads(OperationContext* opCtx) const {
    ShardingRecoveryService::get(opCtx)->promoteRecoverableCriticalSectionToBlockAlsoReads(
        opCtx, NamespaceString(_dbName), _csReason, ShardingCatalogClient::kLocalWriteConcern);
}

void MovePrimaryCoordinator::unblockReadsAndWrites(OperationContext* opCtx) const {
    ShardingRecoveryService::get(opCtx)->releaseRecoverableCriticalSection(
        opCtx, NamespaceString(_dbName), _csReason, ShardingCatalogClient::kLocalWriteConcern);
}

}  // namespace mongo
