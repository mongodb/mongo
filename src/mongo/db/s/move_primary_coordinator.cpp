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
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <fmt/format.h>
#include <iterator>
#include <list>
#include <tuple>
#include <utility>

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bson_field.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/list_collections_filter.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/concurrency/locker.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/repl/change_stream_oplog_notification.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/s/database_sharding_state.h"
#include "mongo/db/s/forwardable_operation_metadata.h"
#include "mongo/db/s/sharding_ddl_util.h"
#include "mongo/db/s/sharding_logging.h"
#include "mongo/db/s/sharding_recovery_service.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/server_options.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/db/shard_id.h"
#include "mongo/db/vector_clock_mutable.h"
#include "mongo/db/write_block_bypass.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/redaction.h"
#include "mongo/platform/compiler.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/type_database_gen.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/move_primary_gen.h"
#include "mongo/util/database_name_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/namespace_string_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {

MONGO_FAIL_POINT_DEFINE(hangBeforeCloningData);

MovePrimaryCoordinator::MovePrimaryCoordinator(ShardingDDLCoordinatorService* service,
                                               const BSONObj& initialState)
    : RecoverableShardingDDLCoordinator(service, "MovePrimaryCoordinator", initialState),
      _dbName(nss().dbName()),
      _csReason([&] {
          BSONObjBuilder builder;
          builder.append("command", "movePrimary");
          builder.append(
              "db", DatabaseNameUtil::serialize(_dbName, SerializationContext::stateDefault()));
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

bool MovePrimaryCoordinator::_mustAlwaysMakeProgress() {
    stdx::lock_guard lk(_docMutex);

    // Any non-retryable errors while checking the preconditions should cause the operation to be
    // terminated. Instead, in any of the subsequent phases, any non-retryable errors that do not
    // trigger the cleanup procedure should cause the operation to be retried from the failed phase.
    return _doc.getPhase() > Phase::kUnset;
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
    return ExecutorFuture<void>(**executor).then([this, executor, anchor = shared_from_this()] {
        const auto opCtxHolder = cc().makeOperationContext();
        auto* opCtx = opCtxHolder.get();
        getForwardableOpMetadata().setOn(opCtx);

        const auto& toShardId = _doc.getToShardId();

        if (toShardId == ShardingState::get(opCtx)->shardId()) {
            LOGV2(7120200,
                  "Database already on requested primary shard",
                  logAttrs(_dbName),
                  "to"_attr = toShardId);

            return ExecutorFuture<void>(**executor);
        }

        const auto toShardEntry = [&] {
            const auto config = Grid::get(opCtx)->shardRegistry()->getConfigShard();
            const auto findResponse = uassertStatusOK(
                config->exhaustiveFindOnConfig(opCtx,
                                               ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                               repl::ReadConcernLevel::kMajorityReadConcern,
                                               NamespaceString::kConfigsvrShardsNamespace,
                                               BSON(ShardType::name() << toShardId),
                                               BSONObj() /* No sorting */,
                                               1 /* Limit */));

            uassert(ErrorCodes::ShardNotFound,
                    "Requested primary shard {} does not exist"_format(toShardId.toString()),
                    !findResponse.docs.empty());

            return uassertStatusOK(ShardType::fromBSON(findResponse.docs.front()));
        }();

        uassert(ErrorCodes::ShardNotFound,
                "Requested primary shard {} is draining"_format(toShardId.toString()),
                !toShardEntry.getDraining());

        return runMovePrimaryWorkflow(executor);
    });
}

ExecutorFuture<void> MovePrimaryCoordinator::runMovePrimaryWorkflow(
    std::shared_ptr<executor::ScopedTaskExecutor> executor) noexcept {
    return ExecutorFuture<void>(**executor)
        .then(_buildPhaseHandler(
            Phase::kClone,
            [this, anchor = shared_from_this()] {
                const auto opCtxHolder = cc().makeOperationContext();
                auto* opCtx = opCtxHolder.get();
                getForwardableOpMetadata().setOn(opCtx);

                const auto& toShardId = _doc.getToShardId();

                if (!_firstExecution) {
                    // The `_shardsvrCloneCatalogData` command to request the recipient to clone the
                    // catalog data for the given database is not idempotent. Therefore, if the
                    // recipient already started cloning data before the coordinator encounters an
                    // error, the movePrimary operation must be aborted.

                    uasserted(
                        7120202,
                        "movePrimary operation on database {} failed cloning data to recipient {}"_format(
                            _dbName.toStringForErrorMsg(), toShardId.toString()));
                }

                LOGV2(7120201,
                      "Running movePrimary operation",
                      logAttrs(_dbName),
                      "to"_attr = toShardId);

                logChange(opCtx, "start");

                ScopeGuard unblockWritesLegacyOnExit([&] {
                    // TODO (SERVER-71444): Fix to be interruptible or document exception.
                    UninterruptibleLockGuard noInterrupt(opCtx->lockState());  // NOLINT
                    unblockWritesLegacy(opCtx);
                });

                blockWritesLegacy(opCtx);

                if (MONGO_unlikely(hangBeforeCloningData.shouldFail())) {
                    LOGV2(7120203, "Hit hangBeforeCloningData");
                    hangBeforeCloningData.pauseWhileSet(opCtx);
                }

                cloneData(opCtx);

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
                                 [this, executor, anchor = shared_from_this()] {
                                     const auto opCtxHolder = cc().makeOperationContext();
                                     auto* opCtx = opCtxHolder.get();
                                     getForwardableOpMetadata().setOn(opCtx);

                                     if (!_firstExecution) {
                                         // Perform a noop write on the recipient in order to
                                         // advance the txnNumber for this coordinator's logical
                                         // session. This prevents requests with older txnNumbers
                                         // from being processed.
                                         _performNoopRetryableWriteOnAllShardsAndConfigsvr(
                                             opCtx, getNewSession(opCtx), **executor);
                                     }

                                     blockReads(opCtx);
                                     enterCriticalSectionOnRecipient(opCtx);
                                 }))
        .then(_buildPhaseHandler(
            Phase::kCommit,
            [this, anchor = shared_from_this()] {
                const auto opCtxHolder = cc().makeOperationContext();
                auto* opCtx = opCtxHolder.get();
                getForwardableOpMetadata().setOn(opCtx);

                invariant(_doc.getDatabaseVersion());
                const auto& preCommitDbVersion = *_doc.getDatabaseVersion();

                commitMetadataToConfig(opCtx, preCommitDbVersion);
                assertChangedMetadataOnConfig(opCtx, preCommitDbVersion);

                notifyChangeStreamsOnMovePrimary(
                    opCtx, _dbName, ShardingState::get(opCtx)->shardId(), _doc.getToShardId());

                // Checkpoint the vector clock to ensure causality in the event of a crash or
                // shutdown.
                VectorClockMutable::get(opCtx)->waitForDurableConfigTime().get(opCtx);

                logChange(opCtx, "commit");
            }))
        .then(_buildPhaseHandler(Phase::kClean,
                                 [this, anchor = shared_from_this()] {
                                     const auto opCtxHolder = cc().makeOperationContext();
                                     auto* opCtx = opCtxHolder.get();
                                     getForwardableOpMetadata().setOn(opCtx);

                                     dropStaleDataOnDonor(opCtx);
                                 }))
        .then(_buildPhaseHandler(Phase::kExitCriticalSection,
                                 [this, executor, anchor = shared_from_this()] {
                                     const auto opCtxHolder = cc().makeOperationContext();
                                     auto* opCtx = opCtxHolder.get();
                                     getForwardableOpMetadata().setOn(opCtx);

                                     if (!_firstExecution) {
                                         // Perform a noop write on the recipient in order to
                                         // advance the txnNumber for this coordinator's logical
                                         // session. This prevents requests with older txnNumbers
                                         // from being processed.
                                         _performNoopRetryableWriteOnAllShardsAndConfigsvr(
                                             opCtx, getNewSession(opCtx), **executor);
                                     }

                                     unblockReadsAndWrites(opCtx);
                                     exitCriticalSectionOnRecipient(opCtx);

                                     LOGV2(7120206,
                                           "Completed movePrimary operation",
                                           logAttrs(_dbName),
                                           "to"_attr = _doc.getToShardId());

                                     logChange(opCtx, "end");
                                 }))
        .onError([this, anchor = shared_from_this()](const Status& status) {
            const auto opCtxHolder = cc().makeOperationContext();
            auto* opCtx = opCtxHolder.get();
            getForwardableOpMetadata().setOn(opCtx);

            const auto& failedPhase = _doc.getPhase();
            if (failedPhase == Phase::kClone || status == ErrorCodes::ShardNotFound) {
                LOGV2_DEBUG(7392900,
                            1,
                            "Triggering movePrimary cleanup",
                            logAttrs(_dbName),
                            "to"_attr = _doc.getToShardId(),
                            "phase"_attr = serializePhase(failedPhase),
                            "error"_attr = redact(status));

                triggerCleanup(opCtx, status);
            }

            return status;
        });
}

void MovePrimaryCoordinator::cloneData(OperationContext* opCtx) {
    const auto& collectionsToClone = getUnshardedCollections(opCtx);
    assertNoOrphanedDataOnRecipient(opCtx, collectionsToClone);

    _doc.setCollectionsToClone(collectionsToClone);
    _updateStateDocument(opCtx, StateDoc(_doc));

    const auto& clonedCollections = cloneDataToRecipient(opCtx);
    assertClonedData(clonedCollections);
}

ExecutorFuture<void> MovePrimaryCoordinator::_cleanupOnAbort(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token,
    const Status& status) noexcept {
    return ExecutorFuture<void>(**executor)
        .then([this, executor, status, anchor = shared_from_this()] {
            const auto opCtxHolder = cc().makeOperationContext();
            auto* opCtx = opCtxHolder.get();
            getForwardableOpMetadata().setOn(opCtx);

            _performNoopRetryableWriteOnAllShardsAndConfigsvr(
                opCtx, getNewSession(opCtx), **executor);

            const auto& failedPhase = _doc.getPhase();
            const auto& toShardId = _doc.getToShardId();

            if (failedPhase <= Phase::kCommit) {
                // A non-retryable error occurred before the new primary shard was actually
                // committed, so any cloned data on the recipient must be dropped.

                try {
                    // Even if the error is `ShardNotFound`, the recipient may still be in draining
                    // mode, so try to drop any orphaned data anyway.
                    dropOrphanedDataOnRecipient(opCtx, executor);
                } catch (const ExceptionFor<ErrorCodes::ShardNotFound>&) {
                    LOGV2_INFO(7392901,
                               "Failed to remove orphaned data on recipient as it has been removed",
                               logAttrs(_dbName),
                               "to"_attr = toShardId);
                }
            }

            unblockReadsAndWrites(opCtx);
            try {
                // Even if the error is `ShardNotFound`, the recipient may still be in draining
                // mode, so try to exit the critical section anyway.
                exitCriticalSectionOnRecipient(opCtx);
            } catch (const ExceptionFor<ErrorCodes::ShardNotFound>&) {
                LOGV2_INFO(7392902,
                           "Failed to exit critical section on recipient as it has been removed",
                           logAttrs(_dbName),
                           "to"_attr = toShardId);
            }

            LOGV2_ERROR(7392903,
                        "Failed movePrimary operation",
                        logAttrs(_dbName),
                        "to"_attr = _doc.getToShardId(),
                        "phase"_attr = serializePhase(_doc.getPhase()),
                        "error"_attr = redact(status));

            logChange(opCtx, "error", status);
        });
}

void MovePrimaryCoordinator::logChange(OperationContext* opCtx,
                                       const std::string& what,
                                       const Status& status) const {
    BSONObjBuilder details;
    details.append("from", ShardingState::get(opCtx)->shardId());
    details.append("to", _doc.getToShardId());
    if (!status.isOK()) {
        details.append("error", status.toString());
    }
    ShardingLogging::get(opCtx)->logChange(
        opCtx, "movePrimary.{}"_format(what), NamespaceString(_dbName), details.obj());
}

std::vector<NamespaceString> MovePrimaryCoordinator::getUnshardedCollections(
    OperationContext* opCtx) const {
    const auto allCollections = [&] {
        DBDirectClient dbClient(opCtx);
        const auto collInfos =
            dbClient.getCollectionInfos(_dbName, ListCollectionsFilter::makeTypeCollectionFilter());

        std::vector<NamespaceString> colls;
        for (const auto& collInfo : collInfos) {
            std::string collName;
            uassertStatusOK(bsonExtractStringField(collInfo, "name", &collName));

            const NamespaceString nss(NamespaceStringUtil::deserialize(_dbName, collName));
            if (!nss.isSystem() ||
                nss.isLegalClientSystemNS(serverGlobalParams.featureCompatibility)) {
                colls.push_back(nss);
            }
        }

        std::sort(colls.begin(), colls.end());
        return colls;
    }();

    const auto shardedCollections = [&] {
        auto colls = Grid::get(opCtx)->catalogClient()->getCollectionNamespacesForDb(
            opCtx, _dbName, repl::ReadConcernLevel::kMajorityReadConcern, {});

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
    const auto& toShardId = _doc.getToShardId();

    auto allCollections = [&] {
        const auto shardRegistry = Grid::get(opCtx)->shardRegistry();
        const auto toShard = uassertStatusOK(shardRegistry->getShard(opCtx, toShardId));

        const auto listCommand = [&] {
            BSONObjBuilder commandBuilder;
            commandBuilder.append("listCollections", 1);
            commandBuilder.append("filter", ListCollectionsFilter::makeTypeCollectionFilter());
            return commandBuilder.obj();
        }();

        const auto listResponse = uassertStatusOK(
            toShard->runExhaustiveCursorCommand(opCtx,
                                                ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                                                _dbName,
                                                listCommand,
                                                Milliseconds(-1)));

        std::vector<NamespaceString> colls;
        for (const auto& bsonColl : listResponse.docs) {
            std::string collName;
            uassertStatusOK(bsonExtractStringField(bsonColl, "name", &collName));
            colls.push_back(NamespaceStringUtil::deserialize(_dbName, collName));
        }

        std::sort(colls.begin(), colls.end());
        return colls;
    }();

    for (const auto& nss : collectionsToClone) {
        uassert(ErrorCodes::NamespaceExists,
                "Found orphaned collection {} on recipient {}"_format(nss.toStringForErrorMsg(),
                                                                      toShardId.toString()),
                !std::binary_search(allCollections.cbegin(), allCollections.cend(), nss));
    };
}

std::vector<NamespaceString> MovePrimaryCoordinator::cloneDataToRecipient(
    OperationContext* opCtx) const {
    // Enable write blocking bypass to allow cloning of catalog data even if writes are disallowed.
    WriteBlockBypass::get(opCtx).set(true);

    const auto& toShardId = _doc.getToShardId();

    const auto shardRegistry = Grid::get(opCtx)->shardRegistry();
    const auto fromShard =
        uassertStatusOK(shardRegistry->getShard(opCtx, ShardingState::get(opCtx)->shardId()));
    const auto toShard = uassertStatusOK(shardRegistry->getShard(opCtx, toShardId));

    const auto cloneCommand = [&] {
        BSONObjBuilder commandBuilder;
        commandBuilder.append(
            "_shardsvrCloneCatalogData",
            DatabaseNameUtil::serialize(_dbName, SerializationContext::stateDefault()));
        commandBuilder.append("from", fromShard->getConnString().toString());
        return CommandHelpers::appendMajorityWriteConcern(commandBuilder.obj());
    }();

    const auto cloneResponse =
        toShard->runCommand(opCtx,
                            ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                            DatabaseName::kAdmin,
                            cloneCommand,
                            Shard::RetryPolicy::kNoRetry);

    uassertStatusOKWithContext(
        Shard::CommandResponse::getEffectiveStatus(cloneResponse),
        "movePrimary operation on database {} failed to clone data to recipient {}"_format(
            _dbName.toStringForErrorMsg(), toShardId.toString()));

    auto clonedCollections = [&] {
        std::vector<NamespaceString> colls;
        for (const auto& bsonElem : cloneResponse.getValue().response["clonedColls"].Obj()) {
            if (bsonElem.type() == String) {
                colls.push_back(NamespaceStringUtil::deserialize(
                    boost::none, bsonElem.String(), SerializationContext::stateDefault()));
            }
        }

        std::sort(colls.begin(), colls.end());
        return colls;
    }();
    return clonedCollections;
}

void MovePrimaryCoordinator::assertClonedData(
    const std::vector<NamespaceString>& clonedCollections) const {
    invariant(_doc.getCollectionsToClone());
    const auto& collectionToClone = *_doc.getCollectionsToClone();

    uassert(7118501,
            "Error cloning data in movePrimary: the list of actually cloned collections doesn't "
            "match the list of collections to close",
            collectionToClone.size() == clonedCollections.size() &&
                std::equal(collectionToClone.cbegin(),
                           collectionToClone.cend(),
                           clonedCollections.cbegin()));
}

void MovePrimaryCoordinator::commitMetadataToConfig(
    OperationContext* opCtx, const DatabaseVersion& preCommitDbVersion) const {
    const auto commitCommand = [&] {
        ConfigsvrCommitMovePrimary request(_dbName, preCommitDbVersion, _doc.getToShardId());
        request.setDbName(DatabaseName::kAdmin);
        return CommandHelpers::appendMajorityWriteConcern(request.toBSON({}));
    }();

    const auto config = Grid::get(opCtx)->shardRegistry()->getConfigShard();
    const auto commitResponse =
        config->runCommandWithFixedRetryAttempts(opCtx,
                                                 ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                                                 DatabaseName::kAdmin,
                                                 commitCommand,
                                                 Shard::RetryPolicy::kIdempotent);

    uassertStatusOKWithContext(
        Shard::CommandResponse::getEffectiveStatus(commitResponse),
        "movePrimary operation on database {} failed to commit metadata changes"_format(
            _dbName.toStringForErrorMsg()));
}

void MovePrimaryCoordinator::assertChangedMetadataOnConfig(
    OperationContext* opCtx, const DatabaseVersion& preCommitDbVersion) const {
    const auto postCommitDbType = [&]() {
        const auto config = Grid::get(opCtx)->shardRegistry()->getConfigShard();
        auto findResponse = uassertStatusOK(config->exhaustiveFindOnConfig(
            opCtx,
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            repl::ReadConcernLevel::kMajorityReadConcern,
            NamespaceString::kConfigDatabasesNamespace,
            BSON(DatabaseType::kDbNameFieldName
                 << DatabaseNameUtil::serialize(_dbName, SerializationContext::stateDefault())),
            BSONObj(),
            1));

        const auto databases = std::move(findResponse.docs);
        uassert(ErrorCodes::IncompatibleShardingMetadata,
                "Tried to find version for database {}, but found no databases"_format(
                    _dbName.toStringForErrorMsg()),
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
    AutoGetDb autoDb(opCtx, _dbName, MODE_IX);
    auto scopedDss = DatabaseShardingState::assertDbLockedAndAcquireExclusive(opCtx, _dbName);
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
                          logAttrs(nss),
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

    // Make a copy of this container since `getNewSession` changes the coordinator document.
    const auto collectionsToClone = *_doc.getCollectionsToClone();
    for (const auto& nss : collectionsToClone) {
        sharding_ddl_util::sendDropCollectionParticipantCommandToShards(opCtx,
                                                                        nss,
                                                                        {_doc.getToShardId()},
                                                                        **executor,
                                                                        getNewSession(opCtx),
                                                                        false /* fromMigrate */);
    }
}

void MovePrimaryCoordinator::blockWritesLegacy(OperationContext* opCtx) const {
    AutoGetDb autoDb(opCtx, _dbName, MODE_X);
    auto scopedDss = DatabaseShardingState::assertDbLockedAndAcquireExclusive(opCtx, _dbName);
    scopedDss->setMovePrimaryInProgress(opCtx);
}

void MovePrimaryCoordinator::unblockWritesLegacy(OperationContext* opCtx) const {
    AutoGetDb autoDb(opCtx, _dbName, MODE_IX);
    auto scopedDss = DatabaseShardingState::assertDbLockedAndAcquireExclusive(opCtx, _dbName);
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
    // The release of the critical section will clear db metadata on secondaries
    clearDbMetadataOnPrimary(opCtx);
    // In case of step-down, this operation could be re-executed and trigger the invariant in case
    // the new primary runs a DDL that acquires the critical section in the old primary shard
    ShardingRecoveryService::get(opCtx)->releaseRecoverableCriticalSection(
        opCtx,
        NamespaceString(_dbName),
        _csReason,
        ShardingCatalogClient::kLocalWriteConcern,
        false /*throwIfReasonDiffers*/);
}

void MovePrimaryCoordinator::enterCriticalSectionOnRecipient(OperationContext* opCtx) {
    const auto enterCriticalSectionCommand = [&] {
        ShardsvrMovePrimaryEnterCriticalSection request(_dbName);
        request.setDbName(DatabaseName::kAdmin);
        request.setReason(_csReason);

        auto command = CommandHelpers::appendMajorityWriteConcern(request.toBSON({}));
        return command.addFields(getNewSession(opCtx).toBSON());
    }();

    const auto& toShardId = _doc.getToShardId();

    const auto enterCriticalSectionResponse = [&] {
        const auto shardRegistry = Grid::get(opCtx)->shardRegistry();
        const auto toShard = uassertStatusOK(shardRegistry->getShard(opCtx, toShardId));

        return toShard->runCommandWithFixedRetryAttempts(
            opCtx,
            ReadPreferenceSetting(ReadPreference::PrimaryOnly),
            DatabaseName::kAdmin,
            enterCriticalSectionCommand,
            Shard::RetryPolicy::kIdempotent);
    }();

    uassertStatusOKWithContext(
        Shard::CommandResponse::getEffectiveStatus(enterCriticalSectionResponse),
        "movePrimary operation on database {} failed to block read/write operations on recipient {}"_format(
            _dbName.toStringForErrorMsg(), toShardId.toString()));
}

void MovePrimaryCoordinator::exitCriticalSectionOnRecipient(OperationContext* opCtx) {
    const auto exitCriticalSectionCommand = [&] {
        ShardsvrMovePrimaryExitCriticalSection request(_dbName);
        request.setDbName(DatabaseName::kAdmin);
        request.setReason(_csReason);

        auto command = CommandHelpers::appendMajorityWriteConcern(request.toBSON({}));
        return command.addFields(getNewSession(opCtx).toBSON());
    }();

    const auto& toShardId = _doc.getToShardId();

    const auto exitCriticalSectionResponse = [&] {
        const auto shardRegistry = Grid::get(opCtx)->shardRegistry();
        const auto toShard = uassertStatusOK(shardRegistry->getShard(opCtx, toShardId));

        return toShard->runCommandWithFixedRetryAttempts(
            opCtx,
            ReadPreferenceSetting(ReadPreference::PrimaryOnly),
            DatabaseName::kAdmin,
            exitCriticalSectionCommand,
            Shard::RetryPolicy::kIdempotent);
    }();

    uassertStatusOKWithContext(
        Shard::CommandResponse::getEffectiveStatus(exitCriticalSectionResponse),
        "movePrimary operation on database {} failed to unblock read/write operations on recipient {}"_format(
            _dbName.toStringForErrorMsg(), toShardId.toString()));
}

}  // namespace mongo
