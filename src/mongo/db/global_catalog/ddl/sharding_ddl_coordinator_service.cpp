/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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


#include "mongo/db/global_catalog/ddl/sharding_ddl_coordinator_service.h"

#include "mongo/base/checked_cast.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/client/dbclient_cursor.h"
#include "mongo/db/client.h"
#include "mongo/db/cluster_parameters/sharding_cluster_parameters_gen.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/global_catalog/ddl/cleanup_structured_encryption_data_coordinator.h"
#include "mongo/db/global_catalog/ddl/clone_authoritative_metadata_coordinator.h"
#include "mongo/db/global_catalog/ddl/collmod_coordinator.h"
#include "mongo/db/global_catalog/ddl/compact_structured_encryption_data_coordinator.h"
#include "mongo/db/global_catalog/ddl/convert_to_capped_coordinator.h"
#include "mongo/db/global_catalog/ddl/create_collection_coordinator.h"
#include "mongo/db/global_catalog/ddl/create_database_coordinator.h"
#include "mongo/db/global_catalog/ddl/drop_collection_coordinator.h"
#include "mongo/db/global_catalog/ddl/drop_database_coordinator.h"
#include "mongo/db/global_catalog/ddl/drop_indexes_coordinator.h"
#include "mongo/db/global_catalog/ddl/initialize_placement_history_coordinator.h"
#include "mongo/db/global_catalog/ddl/migration_blocking_operation_coordinator.h"
#include "mongo/db/global_catalog/ddl/move_primary_coordinator.h"
#include "mongo/db/global_catalog/ddl/refine_collection_shard_key_coordinator.h"
#include "mongo/db/global_catalog/ddl/rename_collection_coordinator.h"
#include "mongo/db/global_catalog/ddl/reshard_collection_coordinator.h"
#include "mongo/db/global_catalog/ddl/set_allow_migrations_coordinator.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_coordinator.h"
#include "mongo/db/global_catalog/ddl/untrack_unsplittable_collection_coordinator.h"
#include "mongo/db/local_catalog/shard_role_catalog/database_sharding_state.h"
#include "mongo/db/local_catalog/shard_role_catalog/operation_sharding_state.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/s/forwardable_operation_metadata.h"
#include "mongo/db/sharding_environment/sharding_feature_flags_gen.h"
#include "mongo/db/topology/add_shard_coordinator.h"
#include "mongo/db/topology/remove_shard_commit_coordinator.h"
#include "mongo/db/version_context.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/str.h"

#include <mutex>
#include <utility>

#include <absl/container/node_hash_map.h>
#include <absl/meta/type_traits.h>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(pauseShardingDDLCoordinatorServiceOnRecovery);

std::shared_ptr<ShardingDDLCoordinator> constructShardingDDLCoordinatorInstance(
    ShardingDDLCoordinatorService* service, BSONObj initialState) {
    const auto op = extractShardingDDLCoordinatorMetadata(initialState);
    LOGV2(
        5390510, "Constructing new sharding DDL coordinator", "coordinatorDoc"_attr = op.toBSON());
    switch (op.getId().getOperationType()) {
        case DDLCoordinatorTypeEnum::kMovePrimary:
            return std::make_shared<MovePrimaryCoordinator>(service, std::move(initialState));
        case DDLCoordinatorTypeEnum::kDropDatabase:
            return std::make_shared<DropDatabaseCoordinator>(service, std::move(initialState));
        case DDLCoordinatorTypeEnum::kDropCollection:
            return std::make_shared<DropCollectionCoordinator>(service, std::move(initialState));
        case DDLCoordinatorTypeEnum::kDropIndexes:
            return std::make_shared<DropIndexesCoordinator>(service, std::move(initialState));
        case DDLCoordinatorTypeEnum::kRenameCollection:
            return std::make_shared<RenameCollectionCoordinator>(service, std::move(initialState));
        case DDLCoordinatorTypeEnum::kCreateCollection:
            return std::make_shared<CreateCollectionCoordinator>(service, std::move(initialState));
        case DDLCoordinatorTypeEnum::kRefineCollectionShardKey:
            return std::make_shared<RefineCollectionShardKeyCoordinator>(service,
                                                                         std::move(initialState));
        case DDLCoordinatorTypeEnum::kSetAllowMigrations:
            return std::make_shared<SetAllowMigrationsCoordinator>(service,
                                                                   std::move(initialState));
        case DDLCoordinatorTypeEnum::kCollMod:
            return std::make_shared<CollModCoordinator>(service, std::move(initialState));
        case DDLCoordinatorTypeEnum::kReshardCollection:
            return std::make_shared<ReshardCollectionCoordinator>(service, std::move(initialState));
        case DDLCoordinatorTypeEnum::kCompactStructuredEncryptionData:
            return std::make_shared<CompactStructuredEncryptionDataCoordinator>(
                service, std::move(initialState));
        case DDLCoordinatorTypeEnum::kCleanupStructuredEncryptionData:
            return std::make_shared<CleanupStructuredEncryptionDataCoordinator>(
                service, std::move(initialState));
        case DDLCoordinatorTypeEnum::kMigrationBlockingOperation:
            return std::make_shared<MigrationBlockingOperationCoordinator>(service,
                                                                           std::move(initialState));
        case DDLCoordinatorTypeEnum::kConvertToCapped:
            return std::make_shared<ConvertToCappedCoordinator>(service, std::move(initialState));
        case DDLCoordinatorTypeEnum::kUntrackUnsplittableCollection:
            return std::make_shared<UntrackUnsplittableCollectionCoordinator>(
                service, std::move(initialState));
        case DDLCoordinatorTypeEnum::kCreateDatabase:
            return std::make_shared<CreateDatabaseCoordinator>(service, std::move(initialState));
        case DDLCoordinatorTypeEnum::kRemoveShardCommit:
            return std::make_shared<RemoveShardCommitCoordinator>(service, std::move(initialState));
        case DDLCoordinatorTypeEnum::kAddShard:
            return std::make_shared<AddShardCoordinator>(service, std::move(initialState));
        case DDLCoordinatorTypeEnum::kCloneAuthoritativeMetadata:
            return std::make_shared<CloneAuthoritativeMetadataCoordinator>(service,
                                                                           std::move(initialState));
        case DDLCoordinatorTypeEnum::kInitializePlacementHistory:
            return std::make_shared<InitializePlacementHistoryCoordinator>(service,
                                                                           std::move(initialState));
        default:
            uasserted(ErrorCodes::BadValue,
                      str::stream()
                          << "Encountered unknown Sharding DDL operation type: "
                          << DDLCoordinatorType_serializer(op.getId().getOperationType()));
    }
}


}  // namespace

ShardingDDLCoordinatorService* ShardingDDLCoordinatorService::getService(OperationContext* opCtx) {
    auto registry = repl::PrimaryOnlyServiceRegistry::get(opCtx->getServiceContext());
    auto service = registry->lookupServiceByName(kServiceName);
    return checked_cast<ShardingDDLCoordinatorService*>(std::move(service));
}

std::shared_ptr<ShardingDDLCoordinatorService::Instance>
ShardingDDLCoordinatorService::constructInstance(BSONObj initialState) {
    auto coord = constructShardingDDLCoordinatorInstance(this, std::move(initialState));

    {
        stdx::lock_guard lg(_mutex);
        const auto coordTypeAndOfcv =
            std::make_pair(coord->operationType(), coord->getOperationFCV());
        const auto coordPerTypeAndOfcvIt =
            _numActiveCoordinatorsPerTypeAndOfcv.find(coordTypeAndOfcv);
        if (coordPerTypeAndOfcvIt != _numActiveCoordinatorsPerTypeAndOfcv.end()) {
            coordPerTypeAndOfcvIt->second++;
        } else {
            _numActiveCoordinatorsPerTypeAndOfcv.emplace(coordTypeAndOfcv, 1);
        }
    }

    pauseShardingDDLCoordinatorServiceOnRecovery.pauseWhileSet();

    coord->getConstructionCompletionFuture()
        .thenRunOn(**getInstanceExecutor())
        .getAsync([this](auto status) {
            AllowOpCtxWhenServiceRebuildingBlock allowOpCtxBlock(Client::getCurrent());
            auto opCtx = cc().makeOperationContext();
            stdx::lock_guard lg(_mutex);
            if (_state != State::kRecovering) {
                return;
            }
            invariant(_numCoordinatorsToWait > 0);
            if (--_numCoordinatorsToWait == 0) {
                _transitionToRecovered(lg, opCtx.get());
            }
        });

    coord->getCompletionFuture()
        .thenRunOn(**getInstanceExecutor())
        .getAsync([this,
                   coordTypeAndOfcv = std::make_pair(coord->operationType(),
                                                     coord->getOperationFCV())](auto status) {
            stdx::lock_guard lg(_mutex);
            if (_state == State::kPaused) {
                return;
            }
            const auto coordPerTypeAndOfcvIt =
                _numActiveCoordinatorsPerTypeAndOfcv.find(coordTypeAndOfcv);
            invariant(coordPerTypeAndOfcvIt != _numActiveCoordinatorsPerTypeAndOfcv.end());
            coordPerTypeAndOfcvIt->second--;
            if (coordPerTypeAndOfcvIt->second == 0) {
                _numActiveCoordinatorsPerTypeAndOfcv.erase(coordPerTypeAndOfcvIt);
            }
            _recoveredOrCoordinatorCompletedCV.notify_all();
        });

    return coord;
}

std::shared_ptr<ShardingDDLCoordinatorExternalState>
ShardingDDLCoordinatorService::createExternalState() const {
    return _externalStateFactory->create();
}

void ShardingDDLCoordinatorService::waitForCoordinatorsOfGivenOfcvToComplete(
    OperationContext* opCtx, std::function<bool(boost::optional<FCV>)> pred) const {
    stdx::unique_lock lk(_mutex);
    opCtx->waitForConditionOrInterrupt(_recoveredOrCoordinatorCompletedCV, lk, [this, pred]() {
        const auto numActiveCoords = _countActiveCoordinators(
            [pred](DDLCoordinatorTypeEnum, boost::optional<FCV> ofcv) { return pred(ofcv); });
        return _state == State::kRecovered && numActiveCoords == 0;
    });
}

void ShardingDDLCoordinatorService::waitForCoordinatorsOfGivenTypeToComplete(
    OperationContext* opCtx, DDLCoordinatorTypeEnum coordType) const {
    stdx::unique_lock lk(_mutex);
    opCtx->waitForConditionOrInterrupt(_recoveredOrCoordinatorCompletedCV, lk, [this, coordType]() {
        const auto numActiveCoords = _countActiveCoordinators(
            [coordType](DDLCoordinatorTypeEnum activeCoordType, boost::optional<FCV>) {
                return coordType == activeCoordType;
            });
        return _state == State::kRecovered && numActiveCoords == 0;
    });
}

void ShardingDDLCoordinatorService::waitForOngoingCoordinatorsToFinish(
    OperationContext* opCtx, std::function<bool(const ShardingDDLCoordinator&)> pred) {
    std::vector<SharedSemiFuture<void>> futuresToWait;

    const auto instances = getAllInstances(opCtx);
    for (const auto& instance : instances) {
        auto typedInstance = checked_pointer_cast<ShardingDDLCoordinator>(instance);
        if (pred(*typedInstance)) {
            futuresToWait.emplace_back(typedInstance->getCompletionFuture());
        }
    }

    for (auto&& future : futuresToWait) {
        future.wait(opCtx);
    }
}

void ShardingDDLCoordinatorService::_onServiceInitialization() {
    stdx::lock_guard lg(_mutex);
    invariant(_state == State::kPaused);
    invariant(_numCoordinatorsToWait == 0);
    invariant(_numActiveCoordinatorsPerTypeAndOfcv.empty());
    _state = State::kRecovering;
}

void ShardingDDLCoordinatorService::_onServiceTermination() {
    stdx::lock_guard lg(_mutex);
    _state = State::kPaused;
    _numCoordinatorsToWait = 0;
    _numActiveCoordinatorsPerTypeAndOfcv.clear();
    _recoveredOrCoordinatorCompletedCV.notify_all();
}

size_t ShardingDDLCoordinatorService::_countActiveCoordinators(
    std::function<bool(DDLCoordinatorTypeEnum, boost::optional<FCV>)> pred) const {
    size_t cnt = 0;
    for (const auto& [typeAndOfcvPair, numCoords] : _numActiveCoordinatorsPerTypeAndOfcv) {
        if (pred(typeAndOfcvPair.first, typeAndOfcvPair.second)) {
            cnt += numCoords;
        }
    }
    return cnt;
}

size_t ShardingDDLCoordinatorService::_countCoordinatorDocs(OperationContext* opCtx) const {
    constexpr auto kNumCoordLabel = "numCoordinators"_sd;
    static const auto countStage = BSON("$count" << kNumCoordLabel);

    AggregateCommandRequest aggRequest{getStateDocumentsNS(), {countStage}};

    DBDirectClient client(opCtx);
    auto cursor = uassertStatusOKWithContext(
        DBClientCursor::fromAggregationRequest(
            &client, std::move(aggRequest), false /* secondaryOk */, true /* useExhaust */),
        "Failed to establish a cursor for aggregation");

    if (!cursor->more()) {
        return 0;
    }

    auto res = cursor->nextSafe();
    auto numCoordField = res.getField(kNumCoordLabel);
    invariant(numCoordField);
    return numCoordField.numberLong();
}

void ShardingDDLCoordinatorService::_waitForRecovery(OperationContext* opCtx,
                                                     std::unique_lock<stdx::mutex>& lock) const {
    opCtx->waitForConditionOrInterrupt(_recoveredOrCoordinatorCompletedCV, lock, [this]() {
        return _state != State::kRecovering;
    });

    uassert(ErrorCodes::NotWritablePrimary,
            "Not primary when trying to create a DDL coordinator",
            _state != State::kPaused);
}

void ShardingDDLCoordinatorService::waitForRecovery(OperationContext* opCtx) const {
    stdx::unique_lock lk(_mutex);
    _waitForRecovery(opCtx, lk);
}

bool ShardingDDLCoordinatorService::areAllCoordinatorsOfTypeFinished(
    OperationContext* opCtx, DDLCoordinatorTypeEnum coordinatorType) {
    stdx::unique_lock lk(_mutex);

    _waitForRecovery(opCtx, lk);

    const auto numActiveCoords = _countActiveCoordinators(
        [coordinatorType](DDLCoordinatorTypeEnum activeCoordType, boost::optional<FCV>) {
            return coordinatorType == activeCoordType;
        });

    return numActiveCoords == 0;
}

ExecutorFuture<void> ShardingDDLCoordinatorService::_rebuildService(
    std::shared_ptr<executor::ScopedTaskExecutor> executor, const CancellationToken& token) {
    return ExecutorFuture<void>(**executor)
        .then([this] {
            AllowOpCtxWhenServiceRebuildingBlock allowOpCtxBlock(Client::getCurrent());
            auto opCtx = cc().makeOperationContext();
            const auto numCoordinators = _countCoordinatorDocs(opCtx.get());
            if (numCoordinators > 0) {
                LOGV2(5622500,
                      "Found Sharding DDL Coordinators to rebuild",
                      "numCoordinators"_attr = numCoordinators);
            }

            pauseShardingDDLCoordinatorServiceOnRecovery.pauseWhileSet();

            {
                stdx::lock_guard lg(_mutex);
                // Since this is an asyncronous task,
                // It could happen that it gets executed after the node stepped down and changed
                // the state to kPaused.
                invariant(_state == State::kRecovering || _state == State::kPaused);
                invariant(_numCoordinatorsToWait == 0);
                if (_state == State::kRecovering) {
                    if (numCoordinators > 0) {
                        _numCoordinatorsToWait = numCoordinators;
                    } else {
                        _transitionToRecovered(lg, opCtx.get());
                    }
                }
            }
        })
        .onError([this](const Status& status) {
            LOGV2_ERROR(5469630,
                        "Failed to rebuild Sharding DDL coordinator service",
                        "error"_attr = status);
            return status;
        });
}

std::shared_ptr<ShardingDDLCoordinatorService::Instance>
ShardingDDLCoordinatorService::getOrCreateInstance(OperationContext* opCtx,
                                                   BSONObj coorDoc,
                                                   const FixedFCVRegion& fcvRegion,
                                                   bool checkOptions) {
    // Wait for all coordinators to be recovered before to allow the creation of new ones.
    waitForRecovery(opCtx);

    auto coorMetadata = extractShardingDDLCoordinatorMetadata(coorDoc);
    const auto& nss = coorMetadata.getId().getNss();

    if (!nss.isConfigDB() && !nss.isAdminDB() &&
        coorMetadata.getId().getOperationType() != DDLCoordinatorTypeEnum::kCreateDatabase) {
        // Check that the operation context has a database version for this namespace
        const auto clientDbVersion = OperationShardingState::get(opCtx).getDbVersion(nss.dbName());
        uassert(ErrorCodes::IllegalOperation,
                "Request sent without attaching database version",
                clientDbVersion);
        {
            const auto scopedDss = DatabaseShardingState::acquire(opCtx, nss.dbName());
            scopedDss->assertIsPrimaryShardForDb(opCtx);
        }
        coorMetadata.setDatabaseVersion(clientDbVersion);
    }

    const auto fcv = serverGlobalParams.featureCompatibility.acquireFCVSnapshot();
    ForwardableOperationMetadata forwardableOpMetadata(opCtx);
    // We currently only propagate the Operation FCV for DDL operations.
    // Moreover, DDL operations cannot be nested. Therefore, the VersionContext
    // shouldn't have been initialized yet.
    invariant(!VersionContext::getDecoration(opCtx).isInitialized());
    if (feature_flags::gSnapshotFCVInDDLCoordinators.isEnabled(kVersionContextIgnored_UNSAFE,
                                                               fcv)) {
        forwardableOpMetadata.setVersionContext(VersionContext{fcv});
    }
    coorMetadata.setForwardableOpMetadata(forwardableOpMetadata);
    const auto patchedCoorDoc = coorDoc.addFields(coorMetadata.toBSON());

    auto [coordinator, created] = [&] {
        while (true) {
            try {
                auto [coordinator, created] =
                    PrimaryOnlyService::getOrCreateInstance(opCtx, patchedCoorDoc, checkOptions);
                return std::make_pair(
                    checked_pointer_cast<ShardingDDLCoordinator>(std::move(coordinator)),
                    std::move(created));
            } catch (const ExceptionFor<ErrorCodes::AddOrRemoveShardInProgress>&) {
                LOGV2_WARNING(5687900,
                              "Cannot start sharding DDL coordinator because a topology change is "
                              "in progress. Will retry after backoff.");
                // Backoff
                opCtx->sleepFor(Seconds(1));
                continue;
            } catch (const DBException& ex) {
                LOGV2_ERROR(5390512,
                            "Failed to create instance of sharding DDL coordinator",
                            "coordinatorId"_attr = coorMetadata.getId(),
                            "reason"_attr = redact(ex));
                throw;
            }
        }
    }();

    return coordinator;
}


std::shared_ptr<executor::TaskExecutor> ShardingDDLCoordinatorService::getInstanceCleanupExecutor()
    const {
    return PrimaryOnlyService::getInstanceCleanupExecutor();
}

void ShardingDDLCoordinatorService::_transitionToRecovered(WithLock lk, OperationContext* opCtx) {
    _state = State::kRecovered;
    _recoveredOrCoordinatorCompletedCV.notify_all();
}

void ShardingDDLCoordinatorService::checkIfConflictsWithOtherInstances(
    OperationContext* opCtx,
    BSONObj initialState,
    const std::vector<const PrimaryOnlyService::Instance*>& existingInstances) {
    auto coorMetadata = extractShardingDDLCoordinatorMetadata(initialState);
    const auto& opType = coorMetadata.getId().getOperationType();
    if (opType != DDLCoordinatorTypeEnum::kRemoveShardCommit &&
        opType != DDLCoordinatorTypeEnum::kAddShard) {

        auto* addOrRemoveShardInProgressParam =
            ServerParameterSet::getClusterParameterSet()
                ->get<ClusterParameterWithStorage<AddOrRemoveShardInProgressParam>>(
                    "addOrRemoveShardInProgress");
        const auto addOrRemoveShardInProgressVal =
            addOrRemoveShardInProgressParam->getValue(boost::none).getInProgress();

        uassert(ErrorCodes::AddOrRemoveShardInProgress,
                "Cannot start ShardingDDLCoordinator because a topology change is in progress",
                !addOrRemoveShardInProgressVal);
    }
}

}  // namespace mongo
