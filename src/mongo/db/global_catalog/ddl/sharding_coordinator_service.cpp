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


#include "mongo/db/global_catalog/ddl/sharding_coordinator_service.h"

#include "mongo/base/checked_cast.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/client/dbclient_cursor.h"
#include "mongo/db/cleanup_structured_encryption_data_coordinator.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/compact_structured_encryption_data_coordinator.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/global_catalog/ddl/clone_authoritative_metadata_coordinator.h"
#include "mongo/db/global_catalog/ddl/collmod_coordinator.h"
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
#include "mongo/db/global_catalog/ddl/set_allow_migrations_coordinator.h"
#include "mongo/db/global_catalog/ddl/sharding_coordinator.h"
#include "mongo/db/global_catalog/ddl/timeseries_upgrade_downgrade_coordinator.h"
#include "mongo/db/global_catalog/ddl/untrack_unsplittable_collection_coordinator.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/s/forwardable_operation_metadata.h"
#include "mongo/db/s/resharding/reshard_collection_coordinator.h"
#include "mongo/db/shard_role/shard_catalog/operation_sharding_state.h"
#include "mongo/db/sharding_environment/sharding_feature_flags_gen.h"
#include "mongo/db/topology/add_shard_coordinator.h"
#include "mongo/db/topology/cluster_parameters/sharding_cluster_parameters_gen.h"
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

MONGO_FAIL_POINT_DEFINE(pauseShardingCoordinatorServiceOnRecovery);

template <typename T>
std::shared_ptr<ShardingCoordinator> typedInstance(ShardingCoordinatorService* service,
                                                   BSONObj initialState) {
    return std::make_shared<T>(service, std::move(initialState));
}

std::shared_ptr<ShardingCoordinator> noInstance(ShardingCoordinatorService*, BSONObj) {
    return nullptr;
}

constexpr std::pair<CoordinatorTypeEnum,
                    std::shared_ptr<ShardingCoordinator> (*)(ShardingCoordinatorService*, BSONObj)>
    kInstanceBuilders[]{
        {CoordinatorTypeEnum::kMovePrimary, typedInstance<MovePrimaryCoordinator>},
        {CoordinatorTypeEnum::kDropDatabase, typedInstance<DropDatabaseCoordinator>},
        {CoordinatorTypeEnum::kDropCollection, typedInstance<DropCollectionCoordinator>},
        {CoordinatorTypeEnum::kDropIndexes, typedInstance<DropIndexesCoordinator>},
        {CoordinatorTypeEnum::kRenameCollection, typedInstance<RenameCollectionCoordinator>},
        {CoordinatorTypeEnum::kCreateCollection, typedInstance<CreateCollectionCoordinator>},
        {CoordinatorTypeEnum::kRefineCollectionShardKey,
         typedInstance<RefineCollectionShardKeyCoordinator>},
        {CoordinatorTypeEnum::kSetAllowMigrations, typedInstance<SetAllowMigrationsCoordinator>},
        {CoordinatorTypeEnum::kCollMod, typedInstance<CollModCoordinator>},
        {CoordinatorTypeEnum::kReshardCollection, typedInstance<ReshardCollectionCoordinator>},
        {CoordinatorTypeEnum::kCompactStructuredEncryptionData,
         typedInstance<CompactStructuredEncryptionDataCoordinator>},
        {CoordinatorTypeEnum::kCleanupStructuredEncryptionData,
         typedInstance<CleanupStructuredEncryptionDataCoordinator>},
        {CoordinatorTypeEnum::kMigrationBlockingOperation,
         typedInstance<MigrationBlockingOperationCoordinator>},
        {CoordinatorTypeEnum::kConvertToCapped, typedInstance<ConvertToCappedCoordinator>},
        {CoordinatorTypeEnum::kUntrackUnsplittableCollection,
         typedInstance<UntrackUnsplittableCollectionCoordinator>},
        {CoordinatorTypeEnum::kCreateDatabase, typedInstance<CreateDatabaseCoordinator>},
        {CoordinatorTypeEnum::kRemoveShardCommit, typedInstance<RemoveShardCommitCoordinator>},
        {CoordinatorTypeEnum::kAddShard, typedInstance<AddShardCoordinator>},
        {CoordinatorTypeEnum::kCloneAuthoritativeMetadata,
         typedInstance<CloneAuthoritativeMetadataCoordinator>},
        {CoordinatorTypeEnum::kInitializePlacementHistory,
         typedInstance<InitializePlacementHistoryCoordinator>},
        // TODO (SERVER-116499): Remove this once 9.0 becomes last LTS.
        {CoordinatorTypeEnum::kTimeseriesUpgradeDowngrade,
         typedInstance<TimeseriesUpgradeDowngradeCoordinator>},
        {CoordinatorTypeEnum::kTestCoordinator, noInstance},
    };

static_assert(std::size(kInstanceBuilders) >= idlEnumCount<CoordinatorTypeEnum>,
              "Missing entries in kInstanceBuilders");

static_assert(std::size(kInstanceBuilders) <= idlEnumCount<CoordinatorTypeEnum>,
              "Too many entries in kInstanceBuilders");

static_assert(
    [] {
        for (auto i = 0u; i < std::size(kInstanceBuilders); i++) {
            if (static_cast<size_t>(kInstanceBuilders[i].first) != i) {
                return false;
            }
        }
        return true;
    }(),
    "Entries in kInstanceBuilders must be ordered");

std::shared_ptr<ShardingCoordinator> constructShardingCoordinatorInstance(
    ShardingCoordinatorService* service, BSONObj initialState) {
    const auto op = extractShardingCoordinatorMetadata(initialState);
    const auto operationType = op.getId().getOperationType();

    LOGV2(5390510, "Constructing new sharding coordinator", "coordinatorDoc"_attr = op.toBSON());

    auto instance = [&]() -> std::shared_ptr<ShardingCoordinator> {
        const auto index = static_cast<size_t>(operationType);
        if (index >= std::size(kInstanceBuilders)) {
            return nullptr;
        }
        return kInstanceBuilders[index].second(service, std::move(initialState));
    }();

    uassert(ErrorCodes::BadValue,
            str::stream() << "Encountered unknown Sharding Coordinator type: "
                          << idl::serialize(operationType),
            instance);

    return instance;
}


size_t countCoordinatorDocs(OperationContext* opCtx, const NamespaceString& nss) {
    constexpr auto kNumCoordLabel = "numCoordinators"_sd;
    static const auto countStage = BSON("$count" << kNumCoordLabel);

    AggregateCommandRequest aggRequest{nss, {countStage}};

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

}  // namespace

ShardingCoordinatorService* ShardingCoordinatorService::getService(OperationContext* opCtx) {
    auto registry = repl::PrimaryOnlyServiceRegistry::get(opCtx->getServiceContext());
    auto service = registry->lookupServiceByName(kServiceName);
    return checked_cast<ShardingCoordinatorService*>(std::move(service));
}

std::shared_ptr<ShardingCoordinatorService::Instance> ShardingCoordinatorService::constructInstance(
    BSONObj initialState) {
    auto coord = constructShardingCoordinatorInstance(this, std::move(initialState));

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

    pauseShardingCoordinatorServiceOnRecovery.pauseWhileSet();

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

std::shared_ptr<ShardingCoordinatorExternalState> ShardingCoordinatorService::createExternalState()
    const {
    return _externalStateFactory->create();
}

void ShardingCoordinatorService::waitForCoordinatorsOfGivenOfcvToComplete(
    OperationContext* opCtx, std::function<bool(boost::optional<FCV>)> pred) const {
    stdx::unique_lock lk(_mutex);
    opCtx->waitForConditionOrInterrupt(_recoveredOrCoordinatorCompletedCV, lk, [this, pred]() {
        uassert(ErrorCodes::NotWritablePrimary,
                "Should not wait on DDL Coordinators in kPaused state",
                _state != State::kPaused);
        const auto numActiveCoords = _countActiveCoordinators(
            [pred](CoordinatorTypeEnum, boost::optional<FCV> ofcv) { return pred(ofcv); });
        return _state == State::kRecovered && numActiveCoords == 0;
    });
}

void ShardingCoordinatorService::waitForCoordinatorsOfGivenTypeToComplete(
    OperationContext* opCtx, CoordinatorTypeEnum coordType) const {
    stdx::unique_lock lk(_mutex);
    opCtx->waitForConditionOrInterrupt(_recoveredOrCoordinatorCompletedCV, lk, [this, coordType]() {
        const auto numActiveCoords = _countActiveCoordinators(
            [coordType](CoordinatorTypeEnum activeCoordType, boost::optional<FCV>) {
                return coordType == activeCoordType;
            });
        return _state == State::kRecovered && numActiveCoords == 0;
    });
}

void ShardingCoordinatorService::waitForOngoingCoordinatorsToFinish(
    OperationContext* opCtx, std::function<bool(const ShardingCoordinator&)> pred) {
    std::vector<SharedSemiFuture<void>> futuresToWait;

    const auto instances = getAllInstances(opCtx);
    for (const auto& instance : instances) {
        auto typedInstance = checked_pointer_cast<ShardingCoordinator>(instance);
        if (pred(*typedInstance)) {
            futuresToWait.emplace_back(typedInstance->getCompletionFuture());
        }
    }

    for (auto&& future : futuresToWait) {
        future.wait(opCtx);
    }
}

void ShardingCoordinatorService::_onServiceInitialization() {
    stdx::lock_guard lg(_mutex);
    invariant(_state == State::kPaused);
    invariant(_numCoordinatorsToWait == 0);
    invariant(_numActiveCoordinatorsPerTypeAndOfcv.empty());
    _state = State::kRecovering;
}

void ShardingCoordinatorService::_onServiceTermination() {
    stdx::lock_guard lg(_mutex);
    _state = State::kPaused;
    _numCoordinatorsToWait = 0;
    _numActiveCoordinatorsPerTypeAndOfcv.clear();
    _recoveredOrCoordinatorCompletedCV.notify_all();
}

size_t ShardingCoordinatorService::_countActiveCoordinators(
    std::function<bool(CoordinatorTypeEnum, boost::optional<FCV>)> pred) const {
    size_t cnt = 0;
    for (const auto& [typeAndOfcvPair, numCoords] : _numActiveCoordinatorsPerTypeAndOfcv) {
        if (pred(typeAndOfcvPair.first, typeAndOfcvPair.second)) {
            cnt += numCoords;
        }
    }
    return cnt;
}

void ShardingCoordinatorService::_waitForRecovery(OperationContext* opCtx,
                                                  std::unique_lock<stdx::mutex>& lock) const {
    opCtx->waitForConditionOrInterrupt(_recoveredOrCoordinatorCompletedCV, lock, [this]() {
        return _state != State::kRecovering;
    });

    uassert(ErrorCodes::NotWritablePrimary,
            "Not primary when trying to create a sharding coordinator",
            _state != State::kPaused);
}

void ShardingCoordinatorService::waitForRecovery(OperationContext* opCtx) const {
    stdx::unique_lock lk(_mutex);
    _waitForRecovery(opCtx, lk);
}

bool ShardingCoordinatorService::areAllCoordinatorsOfTypeFinished(
    OperationContext* opCtx, CoordinatorTypeEnum coordinatorType) {
    // getAllInstances on the PrimaryOnlyServices will wait for recovery, so all coordinators should
    // have been loaded into memory by this point.
    const auto& instances = getAllInstances(opCtx);
    for (const auto& instance : instances) {
        auto typedInstance = checked_pointer_cast<ShardingCoordinator>(instance);
        if (typedInstance->operationType() == coordinatorType) {
            if (!typedInstance->getCompletionFuture().isReady()) {
                return false;
            }
        }
    }

    return true;
}

ExecutorFuture<void> ShardingCoordinatorService::_rebuildService(
    std::shared_ptr<executor::ScopedTaskExecutor> executor, const CancellationToken& token) {
    return ExecutorFuture<void>(**executor)
        .then([this, stateDocNss = getStateDocumentsNS()] {
            AllowOpCtxWhenServiceRebuildingBlock allowOpCtxBlock(Client::getCurrent());
            auto opCtx = cc().makeOperationContext();
            const auto numCoordinators = countCoordinatorDocs(opCtx.get(), stateDocNss);
            if (numCoordinators > 0) {
                LOGV2(5622500,
                      "Found Sharding Coordinators to rebuild",
                      "numCoordinators"_attr = numCoordinators);
            }

            pauseShardingCoordinatorServiceOnRecovery.pauseWhileSet();

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
            LOGV2_ERROR(
                5469630, "Failed to rebuild Sharding coordinator service", "error"_attr = status);
            return status;
        });
}

std::shared_ptr<ShardingCoordinatorService::Instance>
ShardingCoordinatorService::getOrCreateInstance(OperationContext* opCtx,
                                                BSONObj coorDoc,
                                                const FixedFCVRegion& fcvRegion,
                                                bool checkOptions) {
    // Wait for all coordinators to be recovered before to allow the creation of new ones.
    waitForRecovery(opCtx);

    const auto fcv = serverGlobalParams.featureCompatibility.acquireFCVSnapshot();
    ForwardableOperationMetadata forwardableOpMetadata(opCtx);
    // We currently only propagate the Operation FCV for DDL operations.
    // Moreover, DDL operations cannot be nested. Therefore, the VersionContext
    // shouldn't have an OFCV yet.
    invariant(!VersionContext::getDecoration(opCtx).hasOperationFCV());
    if (feature_flags::gSnapshotFCVInDDLCoordinators.isEnabled(kVersionContextIgnored_UNSAFE,
                                                               fcv)) {
        forwardableOpMetadata.setVersionContext(VersionContext{fcv});
    }
    auto coorMetadata = extractShardingCoordinatorMetadata(coorDoc);
    coorMetadata.setDatabaseVersion(
        OperationShardingState::get(opCtx).getDbVersion(coorMetadata.getId().getNss().dbName()));
    coorMetadata.setForwardableOpMetadata(forwardableOpMetadata);
    const auto patchedCoorDoc = coorDoc.addFields(coorMetadata.toBSON());

    auto [coordinator, created] = [&] {
        while (true) {
            try {
                auto [coordinator, created] =
                    PrimaryOnlyService::getOrCreateInstance(opCtx, patchedCoorDoc, checkOptions);
                return std::make_pair(
                    checked_pointer_cast<ShardingCoordinator>(std::move(coordinator)),
                    std::move(created));
            } catch (const ExceptionFor<ErrorCodes::AddOrRemoveShardInProgress>&) {
                LOGV2_WARNING(5687900,
                              "Cannot start sharding coordinator because a topology change is "
                              "in progress. Will retry after backoff.");
                // Backoff
                opCtx->sleepFor(Seconds(1));
                continue;
            } catch (const ExceptionFor<ErrorCodes::PlacementHistoryInitializationInProgress>&) {
                LOGV2_WARNING(10899800,
                              "Cannot start sharding coordinator because an initialization of "
                              "config.placementHistory is in progress. Will retry after backoff.");
                opCtx->sleepFor(Seconds(1));
                continue;
            } catch (const DBException& ex) {
                LOGV2_ERROR(5390512,
                            "Failed to create instance of sharding coordinator",
                            "coordinatorId"_attr = coorMetadata.getId(),
                            "reason"_attr = redact(ex));
                throw;
            }
        }
    }();

    return coordinator;
}


std::shared_ptr<executor::TaskExecutor> ShardingCoordinatorService::getInstanceCleanupExecutor()
    const {
    return PrimaryOnlyService::getInstanceCleanupExecutor();
}

void ShardingCoordinatorService::_transitionToRecovered(WithLock lk, OperationContext* opCtx) {
    _state = State::kRecovered;
    _recoveredOrCoordinatorCompletedCV.notify_all();
}

void ShardingCoordinatorService::checkIfConflictsWithOtherInstances(
    OperationContext* opCtx,
    BSONObj initialState,
    const std::vector<const PrimaryOnlyService::Instance*>& existingInstances) {
    auto coorMetadata = extractShardingCoordinatorMetadata(initialState);
    const auto& opType = coorMetadata.getId().getOperationType();
    if (opType != CoordinatorTypeEnum::kRemoveShardCommit &&
        opType != CoordinatorTypeEnum::kAddShard &&
        opType != CoordinatorTypeEnum::kInitializePlacementHistory) {

        const auto addOrRemoveShardInProgress = [] {
            auto* clusterParameter =
                ServerParameterSet::getClusterParameterSet()
                    ->get<ClusterParameterWithStorage<AddOrRemoveShardInProgressParam>>(
                        "addOrRemoveShardInProgress");
            return clusterParameter->getValue(boost::none).getInProgress();
        }();

        uassert(ErrorCodes::AddOrRemoveShardInProgress,
                "Cannot start ShardingCoordinator because a topology change is in progress",
                !addOrRemoveShardInProgress);

        const auto placementHistoryInitializationInProgress = [] {
            auto* clusterParameter =
                ServerParameterSet::getClusterParameterSet()
                    ->get<
                        ClusterParameterWithStorage<PlacementHistoryInitializationInProgressParam>>(
                        "placementHistoryInitializationInProgress");
            return clusterParameter->getValue(boost::none).getInProgress();
        }();

        uassert(ErrorCodes::PlacementHistoryInitializationInProgress,
                "Cannot start ShardingCoordinator because an initialization of "
                "config.placementHistory is in progress",
                !placementHistoryInitializationInProgress);
    }
}

}  // namespace mongo
