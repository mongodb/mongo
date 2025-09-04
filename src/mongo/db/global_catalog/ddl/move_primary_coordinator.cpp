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

#include "mongo/db/global_catalog/ddl/move_primary_coordinator.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bson_field.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/global_catalog/ddl/move_primary_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_util.h"
#include "mongo/db/global_catalog/ddl/sharding_recovery_service.h"
#include "mongo/db/global_catalog/ddl/shardsvr_commit_create_database_metadata_command.h"
#include "mongo/db/global_catalog/sharding_catalog_client.h"
#include "mongo/db/global_catalog/type_shard.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/collection_catalog.h"
#include "mongo/db/local_catalog/ddl/list_collections_filter.h"
#include "mongo/db/local_catalog/ddl/list_collections_gen.h"
#include "mongo/db/local_catalog/drop_collection.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/shard_role_catalog/database_sharding_runtime.h"
#include "mongo/db/local_catalog/shard_role_catalog/participant_block_gen.h"
#include "mongo/db/repl/change_stream_oplog_notification.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/s/forwardable_operation_metadata.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/sharding_environment/sharding_logging.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/user_write_block/write_block_bypass.h"
#include "mongo/db/vector_clock/vector_clock_mutable.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/redaction.h"
#include "mongo/platform/compiler.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/resharding/resharding_feature_flag_gen.h"
#include "mongo/util/database_name_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/namespace_string_util.h"

#include <algorithm>
#include <iterator>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {

namespace {

MONGO_FAIL_POINT_DEFINE(hangBeforeCloningData);
MONGO_FAIL_POINT_DEFINE(hangBeforeMovePrimaryCriticalSection);
MONGO_FAIL_POINT_DEFINE(movePrimaryFailIfNeedToCloneMovableCollections);

/**
 * Returns true if this unsharded collection can be moved by a moveCollection command.
 */
bool isMovableUnshardedCollection(const NamespaceString& nss, bool timeseriesReshardingSupported) {
    if (nss.isFLE2StateCollection()) {
        // TODO (SERVER-83713): Reconsider isFLE2StateCollection check.
        return false;
    }

    if (nss.isTimeseriesBucketsCollection()) {
        return timeseriesReshardingSupported;
    }

    if (nss.isNamespaceAlwaysUntracked()) {
        return false;
    }

    return true;
}

}  // namespace

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

logv2::DynamicAttributes MovePrimaryCoordinator::getCoordinatorLogAttrs() const {
    return logv2::DynamicAttributes{getBasicCoordinatorAttrs(),
                                    "toShardId"_attr = _doc.getToShardId()};
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
        doc, IDLParserContext("MovePrimaryCoordinatorDocument"));

    const auto toShardIdAreEqual = [&] {
        stdx::lock_guard lk(_docMutex);
        return _doc.getToShardId() == otherDoc.getToShardId();
    }();

    uassert(ErrorCodes::ConflictingOperationInProgress,
            "Another movePrimary operation with different arguments is already running ont the "
            "same database",
            toShardIdAreEqual);
}

ExecutorFuture<void> MovePrimaryCoordinator::_runImpl(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token) noexcept {
    return ExecutorFuture<void>(**executor)
        .then([this, token, executor, anchor = shared_from_this()] {
            const auto opCtxHolder = makeOperationContext();
            auto* opCtx = opCtxHolder.get();

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
                const auto findResponse = uassertStatusOK(config->exhaustiveFindOnConfig(
                    opCtx,
                    ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                    repl::ReadConcernLevel::kMajorityReadConcern,
                    NamespaceString::kConfigsvrShardsNamespace,
                    BSON(ShardType::name() << toShardId),
                    BSONObj() /* No sorting */,
                    1 /* Limit */));

                uassert(
                    ErrorCodes::ShardNotFound,
                    fmt::format("Requested primary shard {} does not exist", toShardId.toString()),
                    !findResponse.docs.empty());

                return uassertStatusOK(ShardType::fromBSON(findResponse.docs.front()));
            }();

            uassert(ErrorCodes::ShardNotFound,
                    fmt::format("Requested primary shard {} is draining", toShardId.toString()),
                    !toShardEntry.getDraining());

            return runMovePrimaryWorkflow(executor, token);
        });
}

ExecutorFuture<void> MovePrimaryCoordinator::runMovePrimaryWorkflow(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token) noexcept {
    return ExecutorFuture<void>(**executor)
        .then(_buildPhaseHandler(  //
            Phase::kClone,
            [this, anchor = shared_from_this()](auto* opCtx) {
                const auto& toShardId = _doc.getToShardId();

                if (!_firstExecution) {
                    // The `_shardsvrCloneCatalogData` command to request the recipient to clone the
                    // catalog data for the given database is not idempotent. Therefore, if the
                    // recipient already started cloning data before the coordinator encounters an
                    // error, the movePrimary operation must be aborted.

                    uasserted(
                        7120202,
                        fmt::format("movePrimary operation on database {} failed cloning data to "
                                    "recipient {}",
                                    _dbName.toStringForErrorMsg(),
                                    toShardId.toString()));
                }

                LOGV2(7120201,
                      "Running movePrimary operation",
                      logAttrs(_dbName),
                      "to"_attr = toShardId);

                logChange(opCtx, "start");

                if (_doc.getAuthoritativeMetadataAccessLevel() ==
                    AuthoritativeMetadataAccessLevelEnum::kWritesAllowed) {
                    cloneAuthoritativeDatabaseMetadata(opCtx);
                }

                ScopeGuard unblockWritesLegacyOnExit([&] {
                    // TODO (SERVER-71444): Fix to be interruptible or document exception.
                    UninterruptibleLockGuard noInterrupt(opCtx);  // NOLINT
                    unblockWritesLegacy(opCtx);
                });

                blockWritesLegacy(opCtx);

                if (MONGO_unlikely(hangBeforeCloningData.shouldFail())) {
                    LOGV2(7120203, "Hit hangBeforeCloningData");
                    hangBeforeCloningData.pauseWhileSet(opCtx);
                }

                cloneData(opCtx);

                // Hack to cover the case of stepping down before actually entering the `kCatchup`
                // phase. Once the time required by the `kClone` phase will be reduced, this
                // synchronization mechanism can be replaced using a critical section.
                blockWrites(opCtx);
            }))
        .then(_buildPhaseHandler(
            Phase::kCatchup,
            [this, anchor = shared_from_this()](auto* opCtx) { blockWrites(opCtx); }))
        .then(_buildPhaseHandler(Phase::kEnterCriticalSection,
                                 [this, token, executor, anchor = shared_from_this()](auto* opCtx) {
                                     if (!_firstExecution) {
                                         // Perform a noop write on the recipient in order to
                                         // advance the txnNumber for this coordinator's logical
                                         // session. This prevents requests with older txnNumbers
                                         // from being processed.
                                         _performNoopRetryableWriteOnAllShardsAndConfigsvr(
                                             opCtx, getNewSession(opCtx), **executor);
                                     }


                                     if (MONGO_unlikely(
                                             hangBeforeMovePrimaryCriticalSection.shouldFail())) {
                                         LOGV2(9031700, "Hit hangBeforeMovePrimaryCriticalSection");
                                         hangBeforeMovePrimaryCriticalSection.pauseWhileSet(opCtx);
                                     }

                                     blockReads(opCtx);
                                     enterCriticalSectionOnRecipient(opCtx, executor, token);
                                 }))
        .then(_buildPhaseHandler(
            Phase::kCommit,
            [this, token, executor = executor, anchor = shared_from_this()](auto* opCtx) {
                tassert(10644515,
                        "Expected databaseVersion to be set on the coordinator document",
                        _doc.getDatabaseVersion());
                const auto& preCommitDbVersion = *_doc.getDatabaseVersion();

                commitMetadataToConfig(opCtx, preCommitDbVersion);

                auto dbMetadata = getPostCommitDatabaseMetadata(opCtx);
                assertChangedMetadataOnConfig(opCtx, dbMetadata, preCommitDbVersion);

                if (_doc.getAuthoritativeMetadataAccessLevel() >=
                    AuthoritativeMetadataAccessLevelEnum::kWritesAllowed) {
                    commitMetadataToShards(opCtx, dbMetadata.getVersion(), executor, token);
                }

                notifyChangeStreamsOnMovePrimary(
                    opCtx, _dbName, ShardingState::get(opCtx)->shardId(), _doc.getToShardId());

                // Checkpoint the vector clock to ensure causality in the event of a crash or
                // shutdown.
                VectorClockMutable::get(opCtx)->waitForDurableConfigTime().get(opCtx);

                logChange(opCtx, "commit");
            }))
        .then(_buildPhaseHandler(
            Phase::kClean,
            [this, anchor = shared_from_this()](auto* opCtx) { dropStaleDataOnDonor(opCtx); }))
        .then(_buildPhaseHandler(Phase::kExitCriticalSection,
                                 [this, token, executor, anchor = shared_from_this()](auto* opCtx) {
                                     if (!_firstExecution) {
                                         // Perform a noop write on the recipient in order to
                                         // advance the txnNumber for this coordinator's logical
                                         // session. This prevents requests with older txnNumbers
                                         // from being processed.
                                         _performNoopRetryableWriteOnAllShardsAndConfigsvr(
                                             opCtx, getNewSession(opCtx), **executor);
                                     }

                                     unblockReadsAndWrites(opCtx);
                                     exitCriticalSectionOnRecipient(opCtx, executor, token);

                                     LOGV2(7120206,
                                           "Completed movePrimary operation",
                                           logAttrs(_dbName),
                                           "to"_attr = _doc.getToShardId());

                                     logChange(opCtx, "end");
                                 }))
        .onError([this, anchor = shared_from_this()](const Status& status) {
            const auto opCtxHolder = makeOperationContext();
            auto* opCtx = opCtxHolder.get();

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
    const auto& collectionsToClone = getCollectionsToClone(opCtx);
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
        .then([this, token, executor, status, anchor = shared_from_this()] {
            const auto opCtxHolder = makeOperationContext();
            auto* opCtx = opCtxHolder.get();

            _performNoopRetryableWriteOnAllShardsAndConfigsvr(
                opCtx, getNewSession(opCtx), **executor);

            const auto& failedPhase = _doc.getPhase();
            const auto& toShardId = _doc.getToShardId();

            unblockReadsAndWrites(opCtx);
            try {
                // Even if the error is `ShardNotFound`, the recipient may still be in draining
                // mode, so try to exit the critical section anyway.
                exitCriticalSectionOnRecipient(opCtx, executor, token);
            } catch (const ExceptionFor<ErrorCodes::ShardNotFound>&) {
                LOGV2_INFO(7392902,
                           "Failed to exit critical section on recipient as it has been removed",
                           logAttrs(_dbName),
                           "to"_attr = toShardId);
            }

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
        opCtx, fmt::format("movePrimary.{}", what), NamespaceString(_dbName), details.obj());
}

std::vector<NamespaceString> MovePrimaryCoordinator::getCollectionsToClone(
    OperationContext* opCtx) const {
    const auto allCollections = [&] {
        std::vector<NamespaceString> colls;

        auto catalog = CollectionCatalog::get(opCtx);
        for (auto&& coll : catalog->range(_dbName)) {
            const auto& nss = coll->ns();
            if (!nss.isSystem() || nss.isLegalClientSystemNS()) {
                colls.push_back(nss);
            }
        }

        std::sort(colls.begin(), colls.end());
        return colls;
    }();

    const auto collectionsToIgnore = [&] {
        auto catalogClient = Grid::get(opCtx)->catalogClient();
        auto colls = catalogClient->getShardedCollectionNamespacesForDb(
            opCtx, _dbName, repl::ReadConcernLevel::kMajorityReadConcern, {});
        auto unshardedTrackedColls = catalogClient->getUnsplittableCollectionNamespacesForDb(
            opCtx, _dbName, repl::ReadConcernLevel::kMajorityReadConcern, {});

        std::move(
            unshardedTrackedColls.begin(), unshardedTrackedColls.end(), std::back_inserter(colls));

        std::sort(colls.begin(), colls.end());

        return colls;
    }();

    std::vector<NamespaceString> collectionsToClone;
    std::set_difference(allCollections.cbegin(),
                        allCollections.cend(),
                        collectionsToIgnore.cbegin(),
                        collectionsToIgnore.cend(),
                        std::back_inserter(collectionsToClone));

    const auto fcvSnapshot = serverGlobalParams.featureCompatibility.acquireFCVSnapshot();
    bool timeseriesReshardingSupported = fcvSnapshot.isVersionInitialized() &&
        resharding::gFeatureFlagReshardingForTimeseries.isEnabled(fcvSnapshot);

    for (const auto& nss : collectionsToClone) {
        movePrimaryFailIfNeedToCloneMovableCollections.executeIf(
            [&](const BSONObj& data) {
                if (isMovableUnshardedCollection(nss, timeseriesReshardingSupported)) {
                    AutoGetCollection autoColl(opCtx, nss, MODE_IS);
                    uassert(9046501,
                            str::stream() << "Found a user collection to clone: "
                                          << nss.toStringForErrorMsg(),
                            !autoColl);
                }
            },
            [&](const BSONObj& data) {
                if (!data.hasField("comment")) {
                    return true;
                }
                // If this failpoint is configured with a "comment", only fail the command if
                // its "comment" matches the failpoint's "comment".
                if (!opCtx->getComment()) {
                    return false;
                }
                return opCtx->getComment()->checkAndGetStringData() ==
                    data.getStringField("comment");
            });
    }

    return collectionsToClone;
}

void MovePrimaryCoordinator::assertNoOrphanedDataOnRecipient(
    OperationContext* opCtx, const std::vector<NamespaceString>& collectionsToClone) const {
    const auto& toShardId = _doc.getToShardId();

    auto allCollections = [&] {
        const auto shardRegistry = Grid::get(opCtx)->shardRegistry();
        const auto toShard = uassertStatusOK(shardRegistry->getShard(opCtx, toShardId));

        const auto listCommand = [&] {
            ListCollections listCollectionsCmd;
            listCollectionsCmd.setDbName(_dbName);
            listCollectionsCmd.setFilter(ListCollectionsFilter::makeTypeCollectionFilter());
            if (gFeatureFlagAllBinariesSupportRawDataOperations.isEnabled(
                    VersionContext::getDecoration(opCtx),
                    serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
                listCollectionsCmd.setRawData(true);
            }
            return listCollectionsCmd.toBSON();
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
                fmt::format("Found orphaned collection {} on recipient {}",
                            nss.toStringForErrorMsg(),
                            toShardId.toString()),
                !std::binary_search(allCollections.cbegin(), allCollections.cend(), nss));
    };
}

std::vector<NamespaceString> MovePrimaryCoordinator::cloneDataToRecipient(OperationContext* opCtx) {
    // Enable write blocking bypass to allow cloning of catalog data even if writes are disallowed.
    WriteBlockBypass::get(opCtx).set(true);

    const auto& toShardId = _doc.getToShardId();

    const auto shardRegistry = Grid::get(opCtx)->shardRegistry();
    const auto fromShard =
        uassertStatusOK(shardRegistry->getShard(opCtx, ShardingState::get(opCtx)->shardId()));
    const auto toShard = uassertStatusOK(shardRegistry->getShard(opCtx, toShardId));

    auto cloneCommand = [&](boost::optional<OperationSessionInfo> osi) {
        BSONObjBuilder commandBuilder;
        commandBuilder.append(
            "_shardsvrCloneCatalogData",
            DatabaseNameUtil::serialize(_dbName, SerializationContext::stateDefault()));
        commandBuilder.append("from", fromShard->getConnString().toString());
        if (osi.is_initialized()) {
            commandBuilder.appendElements(osi->toBSON());
        }
        commandBuilder.append(WriteConcernOptions::kWriteConcernField,
                              defaultMajorityWriteConcernDoNotUse().toBSON());
        return commandBuilder.obj();
    };

    auto clonedCollections = [&](const BSONObj& command) {
        const auto cloneResponse = toShard->runCommandWithIndefiniteRetries(
            opCtx,
            ReadPreferenceSetting(ReadPreference::PrimaryOnly),
            DatabaseName::kAdmin,
            command,
            Shard::RetryPolicy::kNoRetry);

        uassertStatusOKWithContext(
            Shard::CommandResponse::getEffectiveStatus(cloneResponse),
            fmt::format("movePrimary operation on database {} failed to clone data to recipient {}",
                        _dbName.toStringForErrorMsg(),
                        toShardId.toString()));

        std::vector<NamespaceString> colls;
        for (const auto& bsonElem : cloneResponse.getValue().response["clonedColls"].Obj()) {
            if (bsonElem.type() == BSONType::string) {
                colls.push_back(NamespaceStringUtil::deserialize(
                    boost::none, bsonElem.String(), SerializationContext::stateDefault()));
            }
        }

        std::sort(colls.begin(), colls.end());
        return colls;
    };

    return clonedCollections(cloneCommand(getNewSession(opCtx)));
}

void MovePrimaryCoordinator::assertClonedData(
    const std::vector<NamespaceString>& clonedCollections) const {
    tassert(10644516,
            "Expected collectionsToClone to be set on the coordinator document",
            _doc.getCollectionsToClone());
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
        generic_argument_util::setMajorityWriteConcern(request);
        request.setDbName(DatabaseName::kAdmin);
        return request.toBSON();
    }();

    const auto config = Grid::get(opCtx)->shardRegistry()->getConfigShard();
    const auto commitResponse =
        config->runCommand(opCtx,
                           ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                           DatabaseName::kAdmin,
                           commitCommand,
                           Shard::RetryPolicy::kIdempotent);

    uassertStatusOKWithContext(
        Shard::CommandResponse::getEffectiveStatus(commitResponse),
        fmt::format("movePrimary operation on database {} failed to commit metadata changes",
                    _dbName.toStringForErrorMsg()));
}

DatabaseType MovePrimaryCoordinator::getPostCommitDatabaseMetadata(OperationContext* opCtx) const {
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
            fmt::format("Tried to find version for database {}, but found no databases",
                        _dbName.toStringForErrorMsg()),
            !databases.empty());

    return DatabaseType::parse(databases.front(), IDLParserContext("DatabaseType"));
}

void MovePrimaryCoordinator::assertChangedMetadataOnConfig(
    OperationContext* opCtx,
    const DatabaseType& postCommitDbType,
    const DatabaseVersion& preCommitDbVersion) const {
    tassert(7120208,
            "Error committing movePrimary: database version went backwards",
            postCommitDbType.getVersion() > preCommitDbVersion);
    uassert(7120209,
            "Error committing movePrimary: update of config.databases failed",
            postCommitDbType.getPrimary() != ShardingState::get(opCtx)->shardId());
}

void MovePrimaryCoordinator::commitMetadataToShards(
    OperationContext* opCtx,
    const DatabaseVersion& preCommitDbVersion,
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
    const CancellationToken& token) {
    const auto& sessionForDrop = getNewSession(opCtx);
    const auto thisShardId = ShardingState::get(opCtx)->shardId();
    sharding_ddl_util::commitDropDatabaseMetadataToShardCatalog(
        opCtx, _dbName, thisShardId, sessionForDrop, executor, token);

    const auto& sessionForCreate = getNewSession(opCtx);
    sharding_ddl_util::commitCreateDatabaseMetadataToShardCatalog(
        opCtx,
        {_dbName, _doc.getToShardId(), preCommitDbVersion},
        sessionForCreate,
        executor,
        token);
}

void MovePrimaryCoordinator::dropStaleDataOnDonor(OperationContext* opCtx) const {
    // Enable write blocking bypass to allow cleaning of stale data even if writes are disallowed.
    WriteBlockBypass::get(opCtx).set(true);

    tassert(10644517,
            "Expected collectionsToClone to be set on the coordinator document",
            _doc.getCollectionsToClone());

    const auto dropColl = [&](const NamespaceString& nssToDrop) {
        DropReply unusedDropReply;
        try {
            uassertStatusOK(
                dropCollection(opCtx,
                               nssToDrop,
                               &unusedDropReply,
                               DropCollectionSystemCollectionMode::kAllowSystemCollectionDrops,
                               true /* fromMigrate */));
        } catch (const DBException& e) {
            LOGV2_WARNING(7120210,
                          "Failed to drop stale collection on donor",
                          logv2::DynamicAttributes{getCoordinatorLogAttrs(),
                                                   logAttrs(nssToDrop),
                                                   "error"_attr = redact(e)});
        }
    };

    for (const auto& nss : *_doc.getCollectionsToClone()) {
        dropColl(nss);
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
        const auto session = getNewSession(opCtx);
        sharding_ddl_util::sendDropCollectionParticipantCommandToShards(
            opCtx,
            nss,
            {_doc.getToShardId()},
            **executor,
            session,
            true /* fromMigrate */,
            true /* dropSystemCollections */);
    }
}

void MovePrimaryCoordinator::cloneAuthoritativeDatabaseMetadata(OperationContext* opCtx) const {
    auto recoveryService = ShardingRecoveryService::get(opCtx);
    recoveryService->acquireRecoverableCriticalSectionBlockWrites(
        opCtx,
        NamespaceString(_dbName),
        _csReason,
        ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter(),
        false /* clearDbMetadata */);
    recoveryService->promoteRecoverableCriticalSectionToBlockAlsoReads(
        opCtx,
        NamespaceString(_dbName),
        _csReason,
        ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter());

    auto catalogClient = Grid::get(opCtx)->catalogClient();
    auto dbMetadata =
        catalogClient->getDatabase(opCtx, _dbName, repl::ReadConcernLevel::kMajorityReadConcern);

    const auto thisShardId = ShardingState::get(opCtx)->shardId();

    tassert(10162501,
            fmt::format("Expecting to have fetched database metadata from a database which "
                        "this shard owns. DatabaseName: {}. Database primary shard: {}. "
                        "This shard: {}",
                        _dbName.toStringForErrorMsg(),
                        dbMetadata.getPrimary().toString(),
                        thisShardId.toString()),
            thisShardId == dbMetadata.getPrimary());

    commitCreateDatabaseMetadataLocally(opCtx, dbMetadata);

    recoveryService->releaseRecoverableCriticalSection(
        opCtx,
        NamespaceString(_dbName),
        _csReason,
        ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter(),
        ShardingRecoveryService::NoCustomAction(),
        false /* throwIfReasonDiffers */);
}

void MovePrimaryCoordinator::blockWritesLegacy(OperationContext* opCtx) const {
    AutoGetDb autoDb(opCtx, _dbName, MODE_X);
    auto scopedDsr = DatabaseShardingRuntime::assertDbLockedAndAcquireExclusive(opCtx, _dbName);
    scopedDsr->setMovePrimaryInProgress(opCtx);
}

void MovePrimaryCoordinator::unblockWritesLegacy(OperationContext* opCtx) const {
    // We need to be able to unset movePrimaryInProgress in the event of shutdown.
    AutoGetDb autoDb(opCtx,
                     _dbName,
                     MODE_IX,
                     boost::none,
                     Date_t::max(),
                     Lock::DBLockSkipOptions{
                         false, false, false, rss::consensus::IntentRegistry::Intent::LocalWrite});
    auto scopedDsr = DatabaseShardingRuntime::assertDbLockedAndAcquireExclusive(opCtx, _dbName);
    scopedDsr->unsetMovePrimaryInProgress(opCtx);
}

void MovePrimaryCoordinator::blockWrites(OperationContext* opCtx) const {
    const bool clearDbMetadata =
        _doc.getAuthoritativeMetadataAccessLevel() == AuthoritativeMetadataAccessLevelEnum::kNone;
    ShardingRecoveryService::get(opCtx)->acquireRecoverableCriticalSectionBlockWrites(
        opCtx,
        NamespaceString(_dbName),
        _csReason,
        ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter(),
        clearDbMetadata);
}

void MovePrimaryCoordinator::blockReads(OperationContext* opCtx) const {
    ShardingRecoveryService::get(opCtx)->promoteRecoverableCriticalSectionToBlockAlsoReads(
        opCtx,
        NamespaceString(_dbName),
        _csReason,
        ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter());
}

void MovePrimaryCoordinator::unblockReadsAndWrites(OperationContext* opCtx) const {
    const bool clearDbMetadata = _doc.getPhase() >= Phase::kCommit &&
        _doc.getAuthoritativeMetadataAccessLevel() == AuthoritativeMetadataAccessLevelEnum::kNone;

    std::unique_ptr<ShardingRecoveryService::BeforeReleasingCustomAction> actionPtr;
    if (clearDbMetadata) {
        actionPtr = std::make_unique<ShardingRecoveryService::FilteringMetadataClearer>();
    } else {
        actionPtr = std::make_unique<ShardingRecoveryService::NoCustomAction>();
    }

    ShardingRecoveryService::get(opCtx)->releaseRecoverableCriticalSection(
        opCtx,
        NamespaceString(_dbName),
        _csReason,
        ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter(),
        *actionPtr,
        // In case of step-down, this operation could be re-executed and trigger a tassert if
        // the new primary runs a DDL that acquires the critical section in the old primary shard
        false /*throwIfReasonDiffers*/);
}

void MovePrimaryCoordinator::enterCriticalSectionOnRecipient(
    OperationContext* opCtx,
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
    const CancellationToken& token) {
    if (_doc.getAuthoritativeMetadataAccessLevel() > AuthoritativeMetadataAccessLevelEnum::kNone) {
        ShardsvrParticipantBlock request(
            NamespaceString::makeCollectionlessShardsvrParticipantBlockNSS(_dbName));
        request.setBlockType(mongo::CriticalSectionBlockTypeEnum::kReadsAndWrites);
        request.setReason(_csReason);
        request.setClearDbInfo(false);

        generic_argument_util::setMajorityWriteConcern(request);
        generic_argument_util::setOperationSessionInfo(request, getNewSession(opCtx));
        auto opts = std::make_shared<async_rpc::AsyncRPCOptions<ShardsvrParticipantBlock>>(
            **executor, token, request);
        sharding_ddl_util::sendAuthenticatedCommandToShards(opCtx, opts, {_doc.getToShardId()});
    } else {
        const auto enterCriticalSectionCommand = [&] {
            ShardsvrMovePrimaryEnterCriticalSection request(_dbName);
            request.setDbName(DatabaseName::kAdmin);
            request.setReason(_csReason);
            generic_argument_util::setMajorityWriteConcern(request);
            generic_argument_util::setOperationSessionInfo(request, getNewSession(opCtx));

            return request.toBSON();
        }();

        const auto& toShardId = _doc.getToShardId();

        const auto enterCriticalSectionResponse = [&] {
            const auto shardRegistry = Grid::get(opCtx)->shardRegistry();
            const auto toShard = uassertStatusOK(shardRegistry->getShard(opCtx, toShardId));

            return toShard->runCommand(opCtx,
                                       ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                                       DatabaseName::kAdmin,
                                       enterCriticalSectionCommand,
                                       Shard::RetryPolicy::kIdempotent);
        }();

        uassertStatusOKWithContext(
            Shard::CommandResponse::getEffectiveStatus(enterCriticalSectionResponse),
            fmt::format(
                "movePrimary operation on database {} failed to block read/write operations on "
                "recipient {}",
                _dbName.toStringForErrorMsg(),
                toShardId.toString()));
    }
}

void MovePrimaryCoordinator::exitCriticalSectionOnRecipient(
    OperationContext* opCtx,
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
    const CancellationToken& token) {
    if (_doc.getAuthoritativeMetadataAccessLevel() > AuthoritativeMetadataAccessLevelEnum::kNone) {
        ShardsvrParticipantBlock request(
            NamespaceString::makeCollectionlessShardsvrParticipantBlockNSS(_dbName));
        request.setBlockType(CriticalSectionBlockTypeEnum::kUnblock);
        request.setReason(_csReason);
        request.setThrowIfReasonDiffers(false);
        request.setClearDbInfo(false);

        generic_argument_util::setMajorityWriteConcern(request);
        generic_argument_util::setOperationSessionInfo(request, getNewSession(opCtx));

        auto opts = std::make_shared<async_rpc::AsyncRPCOptions<ShardsvrParticipantBlock>>(
            **executor, token, request);
        sharding_ddl_util::sendAuthenticatedCommandToShards(opCtx, opts, {_doc.getToShardId()});
    } else {
        const auto exitCriticalSectionCommand = [&] {
            ShardsvrMovePrimaryExitCriticalSection request(_dbName);
            request.setDbName(DatabaseName::kAdmin);
            request.setReason(_csReason);
            generic_argument_util::setMajorityWriteConcern(request);
            generic_argument_util::setOperationSessionInfo(request, getNewSession(opCtx));

            return request.toBSON();
        }();

        const auto& toShardId = _doc.getToShardId();

        const auto exitCriticalSectionResponse = [&] {
            const auto shardRegistry = Grid::get(opCtx)->shardRegistry();
            const auto toShard = uassertStatusOK(shardRegistry->getShard(opCtx, toShardId));

            return toShard->runCommand(opCtx,
                                       ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                                       DatabaseName::kAdmin,
                                       exitCriticalSectionCommand,
                                       Shard::RetryPolicy::kIdempotent);
        }();

        uassertStatusOKWithContext(
            Shard::CommandResponse::getEffectiveStatus(exitCriticalSectionResponse),
            fmt::format(
                "movePrimary operation on database {} failed to unblock read/write operations "
                "on recipient {}",
                _dbName.toStringForErrorMsg(),
                toShardId.toString()));
    }
}

}  // namespace mongo
