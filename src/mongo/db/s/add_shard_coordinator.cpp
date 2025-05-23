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

#include "mongo/db/s/add_shard_coordinator.h"

#include <fmt/format.h>

#include "mongo/db/generic_argument_util.h"
#include "mongo/db/s/config/configsvr_coordinator_service.h"
#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/db/s/sharding_logging.h"
#include "mongo/db/s/topology_change_helpers.h"
#include "mongo/db/s/user_writes_critical_section_document_gen.h"
#include "mongo/db/vector_clock_mutable.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/add_shard_gen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/future_util.h"
#include "src/mongo/db/list_collections_gen.h"

namespace mongo {

namespace {
static constexpr size_t kMaxFailedRetryCount = 3;
static const Backoff kExponentialBackoff(Seconds(1), Milliseconds::max());
}  // namespace

ExecutorFuture<void> AddShardCoordinator::_runImpl(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token) noexcept {
    return ExecutorFuture<void>(**executor)
        .then(_buildPhaseHandler(
            Phase::kCheckLocalPreconditions,
            [this, _ = shared_from_this()](auto* opCtx) {
                // TODO(SERVER-97816): Remove this call after old add shard path does not exist
                // anymore
                topology_change_helpers::resetDDLBlockingForTopologyChangeIfNeeded(opCtx);

                _verifyInput();

                const auto existingShard = topology_change_helpers::getExistingShard(
                    opCtx,
                    _doc.getConnectionString(),
                    _doc.getProposedName(),
                    *ShardingCatalogManager::get(opCtx)->localCatalogClient());
                if (existingShard.has_value()) {
                    _doc.setChosenName(existingShard.value().getName());
                    _enterPhase(AddShardCoordinatorPhaseEnum::kFinal);
                }
            }))
        .onError([](const Status& status) { return status; })
        .then(_buildPhaseHandler(
            Phase::kCheckShardPreconditions,
            [this, &token, _ = shared_from_this(), executor](auto* opCtx) {
                auto& targeter = _getTargeter(opCtx);

                try {
                    _runWithRetries(
                        [&]() {
                            topology_change_helpers::validateHostAsShard(opCtx,
                                                                         targeter,
                                                                         _doc.getConnectionString(),
                                                                         _doc.getIsConfigShard(),
                                                                         **executor);
                        },
                        executor,
                        token);
                } catch (const DBException&) {
                    // If we are not able to validate the host as a shard after multiple tries, we
                    // don't want to continue, so we remove the replicaset monitor and give up.
                    topology_change_helpers::removeReplicaSetMonitor(opCtx,
                                                                     _doc.getConnectionString());
                    _completeOnError = true;
                    throw;
                }

                // For the first shard we check if it's a promotion of a populated replicaset or
                // adding a clean state replicaset.
                // If the replicaset is the configserver or it's not the first shard, we always
                // expect the replicaset to be clean.
                // First we do an optimistic data check. If the replicaset is not empty, then it's a
                // promotion. If the replicaset is empty, then we block the user writes to avoid
                // race conditions, and do a second check. If the replicaset has data, then it's a
                // promotion and we restore the user writes.
                if (_isFirstShard(opCtx) && !_doc.getIsConfigShard()) {
                    if (!_isPristineReplicaset(opCtx, targeter, **executor)) {
                        _doc.setIsPromotion(true);
                    } else {
                        _blockUserWrites(opCtx, **executor);
                        if (!_isPristineReplicaset(opCtx, targeter, **executor)) {
                            _doc.setIsPromotion(true);
                            _restoreUserWrites(opCtx, **executor);
                        }
                    }
                } else {
                    if (!_doc.getIsConfigShard()) {
                        _blockUserWrites(opCtx, **executor);
                    }
                    uassert(ErrorCodes::IllegalOperation,
                            fmt::format("can't add shard '{}' because it's not empty.",
                                        _doc.getConnectionString().toString()),
                            _isPristineReplicaset(opCtx, targeter, **executor));
                }

                // (Generic FCV reference): These FCV checks should exist across LTS binary
                // versions.
                const auto currentFCV =
                    serverGlobalParams.featureCompatibility.acquireFCVSnapshot().getVersion();
                invariant(currentFCV == multiversion::GenericFCV::kLatest ||
                          currentFCV == multiversion::GenericFCV::kLastContinuous ||
                          currentFCV == multiversion::GenericFCV::kLastLTS);

                _setFCVOnReplicaSet(opCtx, currentFCV, **executor);

                const auto host = uassertStatusOK(
                    Grid::get(opCtx)->shardRegistry()->getConfigShard()->getTargeter()->findHost(
                        opCtx, ReadPreferenceSetting{ReadPreference::PrimaryOnly}));

                try {
                    _runWithRetries(
                        [&]() {
                            ShardsvrCheckCanConnectToConfigServer cmd(host);
                            cmd.setDbName(DatabaseName::kAdmin);
                            uassertStatusOK(
                                topology_change_helpers::runCommandForAddShard(opCtx,
                                                                               _getTargeter(opCtx),
                                                                               DatabaseName::kAdmin,
                                                                               cmd.toBSON(),
                                                                               **executor)
                                    .commandStatus);
                        },
                        executor,
                        token);
                } catch (const DBException&) {
                    // If the replica set is not able to contact us after multiple tries, we don't
                    // want to continue, so we remove the replicaset monitor and give up.
                    topology_change_helpers::removeReplicaSetMonitor(opCtx,
                                                                     _doc.getConnectionString());
                    _completeOnError = true;
                    throw;
                }

                std::string shardName =
                    topology_change_helpers::createShardName(opCtx,
                                                             _getTargeter(opCtx),
                                                             _doc.getIsConfigShard(),
                                                             _doc.getProposedName(),
                                                             **executor);

                _doc.setChosenName(shardName);
            }))
        .then(_buildPhaseHandler(
            Phase::kPrepareNewShard,
            [this, _ = shared_from_this()] { return !_doc.getIsConfigShard(); },
            [this, _ = shared_from_this(), executor](auto* opCtx) {
                auto& targeter = _getTargeter(opCtx);

                _dropSessionsCollection(opCtx, **executor);

                topology_change_helpers::getClusterTimeKeysFromReplicaSet(
                    opCtx, targeter, **executor);

                boost::optional<APIParameters> apiParameters;
                if (const auto params = _doc.getApiParams(); params.has_value()) {
                    apiParameters = APIParameters::fromBSON(params.value());
                }

                const auto shardIdentity = topology_change_helpers::createShardIdentity(
                    opCtx, _doc.getChosenName()->toString());

                topology_change_helpers::installShardIdentity(
                    opCtx, shardIdentity, targeter, apiParameters, _osiGenerator(), **executor);

                _standardizeClusterParameters(opCtx, executor);
            }))
        .then(_buildPhaseHandler(
            Phase::kCommit,
            [this, _ = shared_from_this(), executor](auto* opCtx) {
                auto& targeter = _getTargeter(opCtx);

                auto dbList = topology_change_helpers::getDBNamesListFromReplicaSet(
                    opCtx, targeter, **executor);

                topology_change_helpers::blockDDLCoordinatorsAndDrain(
                    opCtx, /*persistRecoveryDocument*/ false);

                auto& shardingCatalogManager = *ShardingCatalogManager::get(opCtx);

                auto clusterCardinalityParameterLock =
                    shardingCatalogManager.acquireClusterCardinalityParameterLockForTopologyChange(
                        opCtx);
                auto shardMembershipLock =
                    shardingCatalogManager.acquireShardMembershipLockForTopologyChange(opCtx);


                ShardType shard;
                shard.setName(_doc.getChosenName()->toString());
                shard.setHost(targeter.connectionString().toString());
                shard.setState(ShardType::ShardState::kShardAware);

                auto newTopologyTime = VectorClockMutable::get(opCtx)->tickClusterTime(1);
                shard.setTopologyTime(newTopologyTime.asTimestamp());

                {
                    const auto originalWC = opCtx->getWriteConcern();
                    ScopeGuard resetWCGuard([&] { opCtx->setWriteConcern(originalWC); });

                    opCtx->setWriteConcern(defaultMajorityWriteConcernDoNotUse());
                    try {
                        topology_change_helpers::addShardInTransaction(
                            opCtx,
                            shard,
                            std::move(dbList),
                            std::vector<CollectionType>{},
                            **executor);
                    } catch (const ExceptionFor<ErrorCodes::DuplicateKey>&) {
                        // If this happens, it means something happened in the previous try after
                        // doing the transaction, so it's okay to swallow the duplicate key
                        // exception on the next try.
                    }
                }

                BSONObjBuilder shardDetails;
                shardDetails.append("name", shard.getName());
                shardDetails.append("host", shard.getHost());

                ShardingLogging::get(opCtx)->logChange(opCtx,
                                                       "addShard",
                                                       NamespaceString::kEmpty,
                                                       shardDetails.obj(),
                                                       defaultMajorityWriteConcernDoNotUse(),
                                                       shardingCatalogManager.localConfigShard(),
                                                       shardingCatalogManager.localCatalogClient());

                auto shardRegistry = Grid::get(opCtx)->shardRegistry();
                shardRegistry->reload(opCtx);
                tassert(9870601,
                        "Shard not found in ShardRegistry after committing addShard",
                        shardRegistry->getShard(opCtx, shard.getName()).isOK());

                topology_change_helpers::
                    hangAddShardBeforeUpdatingClusterCardinalityParameterFailpoint(opCtx);

                shardMembershipLock.unlock();

                topology_change_helpers::updateClusterCardinalityParameter(
                    clusterCardinalityParameterLock, opCtx);
            }))
        .then(_buildPhaseHandler(
            Phase::kCleanup,
            [this, _ = shared_from_this(), executor](auto* opCtx) {
                topology_change_helpers::propagateClusterUserWriteBlockToReplicaSet(
                    opCtx, _getTargeter(opCtx), **executor);
                topology_change_helpers::unblockDDLCoordinators(opCtx,
                                                                /*removeRecoveryDocument*/ false);
            }))
        .then(_buildPhaseHandler(Phase::kFinal,
                                 [this, _ = shared_from_this()](auto* opCtx) {
                                     // If we reach the final phase that means we added a shard (or
                                     // was already added).
                                     // If we were not able to add anything then an assert should
                                     // had been thrown earlier.
                                     tassert(ErrorCodes::OperationFailed,
                                             "Final state is reached but no name was chosen.",
                                             _doc.getChosenName().has_value());

                                     repl::ReplClientInfo::forClient(opCtx->getClient())
                                         .setLastOpToSystemLastOpTime(opCtx);

                                     _result = _doc.getChosenName().value().toString();
                                 }))
        .onError([this, _ = shared_from_this()](const Status& status) {
            if (!_mustAlwaysMakeProgress() && !_isRetriableErrorForDDLCoordinator(status)) {
                auto opCtxHolder = makeOperationContext();
                auto* opCtx = opCtxHolder.get();
                triggerCleanup(opCtx, status);
            }

            return status;
        });
}

ExecutorFuture<void> AddShardCoordinator::_cleanupOnAbort(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token,
    const Status& status) noexcept {
    return ExecutorFuture<void>(**executor)
        .then([this, token, status, executor, _ = shared_from_this()] {
            const auto opCtxHolder = makeOperationContext();
            auto* opCtx = opCtxHolder.get();
            if (!_doc.getIsConfigShard() && _doc.getOriginalUserWriteBlockingLevel().has_value()) {
                _restoreUserWrites(opCtx, **executor);
            }
            topology_change_helpers::unblockDDLCoordinators(opCtx,
                                                            /*removeRecoveryDocument*/ false);
            topology_change_helpers::removeReplicaSetMonitor(opCtx, _doc.getConnectionString());
        });
}

void AddShardCoordinator::checkIfOptionsConflict(const BSONObj& stateDoc) const {
    // Only one add shard can run at any time, so all the user supplied parameters must match.
    const auto otherDoc = AddShardCoordinatorDocument::parse(
        IDLParserContext("AddShardCoordinatorDocument"), stateDoc);

    const auto optionsMatch = [&] {
        stdx::lock_guard lk(_docMutex);
        auto apiParamsMatch = [&]() -> bool {
            // Either both initialized or neither
            if (_doc.getApiParams().is_initialized() != otherDoc.getApiParams().is_initialized())
                return false;
            // If neither initialized, they match
            if (!_doc.getApiParams().is_initialized())
                return true;
            // If both are initialized, check the actual params
            return APIParameters::fromBSON(_doc.getApiParams().value()) ==
                APIParameters::fromBSON(otherDoc.getApiParams().value());
        };
        return _doc.getConnectionString() == otherDoc.getConnectionString() &&
            _doc.getProposedName() == otherDoc.getProposedName() && apiParamsMatch();
    }();

    uassert(ErrorCodes::ConflictingOperationInProgress,
            str::stream() << "Another addShard with different arguments is already running with "
                             "different options",
            optionsMatch);
}

const std::string& AddShardCoordinator::getResult(OperationContext* opCtx) const {
    const_cast<AddShardCoordinator*>(this)->getCompletionFuture().get(opCtx);
    invariant(_result.is_initialized());
    return *_result;
}

bool AddShardCoordinator::canAlwaysStartWhenUserWritesAreDisabled() const {
    return true;
}

std::shared_ptr<AddShardCoordinator> AddShardCoordinator::create(
    OperationContext* opCtx,
    const FixedFCVRegion& fcvRegion,
    const mongo::ConnectionString& target,
    boost::optional<std::string> name,
    bool isConfigShard) {
    auto coordinatorDoc = AddShardCoordinatorDocument();
    coordinatorDoc.setConnectionString(target);
    coordinatorDoc.setIsConfigShard(isConfigShard);
    coordinatorDoc.setProposedName(name);
    auto metadata = ShardingDDLCoordinatorMetadata(
        {{NamespaceString::kConfigsvrShardsNamespace, DDLCoordinatorTypeEnum::kAddShard}});
    auto fwdOpCtx = metadata.getForwardableOpMetadata();
    if (!fwdOpCtx) {
        fwdOpCtx = ForwardableOperationMetadata(opCtx);
    }
    fwdOpCtx->setMayBypassWriteBlocking(true);
    metadata.setForwardableOpMetadata(fwdOpCtx);
    coordinatorDoc.setShardingDDLCoordinatorMetadata(metadata);

    const auto apiParameters = APIParameters::get(opCtx);
    if (apiParameters.getParamsPassed()) {
        coordinatorDoc.setApiParams(apiParameters.toBSON());
    }

    return checked_pointer_cast<AddShardCoordinator>(
        ShardingDDLCoordinatorService::getService(opCtx)->getOrCreateInstance(
            opCtx, coordinatorDoc.toBSON(), fcvRegion));
}

bool AddShardCoordinator::_mustAlwaysMakeProgress() {
    return _doc.getPhase() >= Phase::kPrepareNewShard;
}

// TODO (SPM-4017): these changes should be done on the cluster command level.
void AddShardCoordinator::_verifyInput() const {
    uassert(ErrorCodes::BadValue, "Invalid connection string", _doc.getConnectionString());

    if (_doc.getConnectionString().type() != ConnectionString::ConnectionType::kStandalone &&
        _doc.getConnectionString().type() != ConnectionString::ConnectionType::kReplicaSet) {
        uasserted(ErrorCodes::FailedToParse,
                  str::stream() << "Invalid connection string "
                                << _doc.getConnectionString().toString());
    }

    uassert(ErrorCodes::BadValue,
            "shard name cannot be empty",
            !_doc.getProposedName() || !_doc.getProposedName()->empty());
}

bool AddShardCoordinator::_isPristineReplicaset(
    OperationContext* opCtx,
    RemoteCommandTargeter& targeter,
    std::shared_ptr<executor::TaskExecutor> executor) const {
    return topology_change_helpers::getDBNamesListFromReplicaSet(opCtx, targeter, executor).empty();
}

RemoteCommandTargeter& AddShardCoordinator::_getTargeter(OperationContext* opCtx) {
    if (!_shardConnection) {
        const auto shardRegistry = Grid::get(opCtx)->shardRegistry();
        _shardConnection = shardRegistry->createConnection(_doc.getConnectionString());
    }

    return *(_shardConnection->getTargeter());
}

void AddShardCoordinator::_runWithRetries(std::function<void()>&& function,
                                          std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                          const CancellationToken& token) {
    size_t failCounter = 0;

    AsyncTry([&]() {
        try {
            function();
        } catch (const DBException& ex) {
            return ex.toStatus();
        }
        return Status::OK();
    })
        .until([&](const Status& status) {
            if (status.isOK()) {
                return true;
            }
            if (!_isRetriableErrorForDDLCoordinator(status)) {
                return true;
            }
            failCounter++;
            if (failCounter >= kMaxFailedRetryCount) {
                return true;
            }
            return false;
        })
        .withBackoffBetweenIterations(kExponentialBackoff)
        .on(**executor, token)
        .get();
}

boost::optional<std::function<OperationSessionInfo(OperationContext*)>>
AddShardCoordinator::_osiGenerator() {
    return boost::make_optional<std::function<OperationSessionInfo(OperationContext*)>>(
        [this](OperationContext* opCtx) -> OperationSessionInfo { return getNewSession(opCtx); });
}

void AddShardCoordinator::_standardizeClusterParameters(
    OperationContext* opCtx, std::shared_ptr<executor::ScopedTaskExecutor> executor) {
    ConfigsvrCoordinatorService::getService(opCtx)->waitForAllOngoingCoordinatorsOfType(
        opCtx, ConfigsvrCoordinatorTypeEnum::kSetClusterParameter);

    auto configSvrClusterParameterDocs =
        topology_change_helpers::getClusterParametersLocally(opCtx);

    // If this is the first shard being added, and no cluster parameters have been set, then this
    // can be seen as a replica set to shard conversion - absorb all of this shard's cluster
    // parameters. Otherwise, push our cluster parameters to the shard.
    if (_isFirstShard(opCtx)) {
        bool clusterParameterDocsEmpty = std::all_of(
            configSvrClusterParameterDocs.begin(),
            configSvrClusterParameterDocs.end(),
            [&](const std::pair<boost::optional<TenantId>, std::vector<BSONObj>>& tenantParams) {
                return tenantParams.second.empty();
            });
        if (clusterParameterDocsEmpty) {
            auto parameters = topology_change_helpers::getClusterParametersFromReplicaSet(
                opCtx, _getTargeter(opCtx), **executor);
            topology_change_helpers::setClusterParametersLocally(opCtx, parameters);
            return;
        }
    }
    topology_change_helpers::setClusterParametersOnReplicaSet(
        opCtx, _getTargeter(opCtx), configSvrClusterParameterDocs, _osiGenerator(), **executor);
}

bool AddShardCoordinator::_isFirstShard(OperationContext* opCtx) {
    auto shardRegistry = Grid::get(opCtx)->shardRegistry();
    shardRegistry->reload(opCtx);
    return shardRegistry->getNumShards(opCtx) == 0;
}

void AddShardCoordinator::_setFCVOnReplicaSet(OperationContext* opCtx,
                                              mongo::ServerGlobalParams::FCVSnapshot::FCV fcv,
                                              std::shared_ptr<executor::TaskExecutor> executor) {
    if (_doc.getIsConfigShard()) {
        return;
    }
    auto const sessionInfo = getNewSession(opCtx);

    SetFeatureCompatibilityVersion setFcvCmd(fcv);
    setFcvCmd.setDbName(DatabaseName::kAdmin);
    setFcvCmd.setFromConfigServer(true);
    generic_argument_util::setMajorityWriteConcern(setFcvCmd);

    uassertStatusOK(
        topology_change_helpers::runCommandForAddShard(
            opCtx, _getTargeter(opCtx), DatabaseName::kAdmin, setFcvCmd.toBSON(), executor)
            .commandStatus);
}

void AddShardCoordinator::_blockUserWrites(OperationContext* opCtx,
                                           std::shared_ptr<executor::TaskExecutor> executor) {
    auto level = _doc.getOriginalUserWriteBlockingLevel();
    if (!level.has_value()) {
        level = _getUserWritesBlockFromReplicaSet(opCtx, executor);
        _doc.setOriginalUserWriteBlockingLevel(static_cast<int32_t>(*level));
        _updateStateDocument(opCtx, StateDoc(_doc));
    }
    topology_change_helpers::setUserWriteBlockingState(
        opCtx,
        _getTargeter(opCtx),
        topology_change_helpers::UserWriteBlockingLevel::All,
        true, /* block writes */
        _osiGenerator(),
        executor);
}

void AddShardCoordinator::_restoreUserWrites(OperationContext* opCtx,
                                             std::shared_ptr<executor::TaskExecutor> executor) {
    uint8_t level = static_cast<uint8_t>(*_doc.getOriginalUserWriteBlockingLevel());
    if (level & topology_change_helpers::UserWriteBlockingLevel::Writes) {
        level |= topology_change_helpers::UserWriteBlockingLevel::DDLOperations;
    }
    topology_change_helpers::setUserWriteBlockingState(opCtx,
                                                       _getTargeter(opCtx),
                                                       level,
                                                       true, /* block writes */
                                                       _osiGenerator(),
                                                       executor);
}

topology_change_helpers::UserWriteBlockingLevel
AddShardCoordinator::_getUserWritesBlockFromReplicaSet(
    OperationContext* opCtx, std::shared_ptr<executor::TaskExecutor> executor) {
    auto& targeter = _getTargeter(opCtx);
    auto fetcherStatus =
        Status(ErrorCodes::InternalError, "Internal error running cursor callback in command");
    uint8_t level = topology_change_helpers::UserWriteBlockingLevel::None;

    // Retrieve the specific user write block level
    auto fetcher = topology_change_helpers::createFindFetcher(
        opCtx,
        targeter,
        NamespaceString::kUserWritesCriticalSectionsNamespace,
        repl::ReadConcernLevel::kMajorityReadConcern,
        [&](const std::vector<BSONObj>& docs) -> bool {
            for (const BSONObj& doc : docs) {
                const auto parsedDoc = UserWriteBlockingCriticalSectionDocument::parse(
                    IDLParserContext("UserWriteBlockingCriticalSectionDocument"), doc);
                if (parsedDoc.getBlockNewUserShardedDDL()) {
                    level |= topology_change_helpers::UserWriteBlockingLevel::DDLOperations;
                }

                if (parsedDoc.getBlockUserWrites()) {
                    level |= topology_change_helpers::UserWriteBlockingLevel::Writes;
                }
            }
            return true;
        },
        [&](const Status& status) { fetcherStatus = status; },
        executor);
    uassertStatusOK(fetcher->schedule());
    uassertStatusOK(fetcher->join(opCtx));
    uassertStatusOK(fetcherStatus);

    return topology_change_helpers::UserWriteBlockingLevel(level);
}

void AddShardCoordinator::_dropSessionsCollection(
    OperationContext* opCtx, std::shared_ptr<executor::TaskExecutor> executor) {

    ListCollections listCollectionsCmd;
    listCollectionsCmd.setDbName(DatabaseName::kConfig);
    listCollectionsCmd.setFilter(BSON("name" << NamespaceString::kLogicalSessionsNamespace.coll()));

    auto res = topology_change_helpers::runCommandForAddShard(
        opCtx, _getTargeter(opCtx), DatabaseName::kConfig, listCollectionsCmd.toBSON(), executor);
    uassertStatusOK(res.commandStatus);

    auto parsedResponse =
        ListCollectionsReply::parse(IDLParserContext("ListCollectionReply"), res.response);
    tassert(10235201,
            "Found more than one system.session collection on the replica set being added",
            parsedResponse.getCursor().getFirstBatch().size() <= 1);
    if (parsedResponse.getCursor().getFirstBatch().size() == 0) {
        return;
    }

    auto uuid = parsedResponse.getCursor().getFirstBatch()[0].getInfo()->getUuid();

    BSONObjBuilder builder;
    builder.append("drop", NamespaceString::kLogicalSessionsNamespace.coll());
    uuid->appendToBuilder(&builder, "collectionUUID");
    {
        BSONObjBuilder wcBuilder(builder.subobjStart("writeConcern"));
        wcBuilder.append("w", "majority");
    }

    uassertStatusOK(topology_change_helpers::runCommandForAddShard(
                        opCtx,
                        _getTargeter(opCtx),
                        NamespaceString::kLogicalSessionsNamespace.dbName(),
                        builder.done(),
                        executor)
                        .commandStatus);
}

}  // namespace mongo
