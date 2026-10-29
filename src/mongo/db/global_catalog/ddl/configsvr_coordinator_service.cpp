// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/global_catalog/ddl/configsvr_coordinator_service.h"

#include "mongo/base/checked_cast.h"
#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/dbclient_cursor.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/global_catalog/ddl/configsvr_coordinator.h"
#include "mongo/db/global_catalog/ddl/sharding_coordinator_service.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/topology/cluster_parameters/set_cluster_parameter_coordinator.h"
#include "mongo/db/topology/cluster_parameters/sharding_cluster_parameters_gen.h"
#include "mongo/db/topology/user_write_block/set_user_write_block_mode_coordinator.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/future.h"
#include "mongo/util/str.h"

#include <string>
#include <type_traits>
#include <utility>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {

ConfigsvrCoordinatorService* ConfigsvrCoordinatorService::getService(OperationContext* opCtx) {
    auto registry = repl::PrimaryOnlyServiceRegistry::get(opCtx->getServiceContext());
    auto service = registry->lookupServiceByName(kServiceName);
    return checked_cast<ConfigsvrCoordinatorService*>(std::move(service));
}

std::shared_ptr<ConfigsvrCoordinator> ConfigsvrCoordinatorService::getOrCreateService(
    OperationContext* opCtx, BSONObj coorDoc) {
    auto [coordinator, created] = [&] {
        try {
            auto [coordinator, created] = PrimaryOnlyService::getOrCreateInstance(opCtx, coorDoc);
            return std::make_pair(
                checked_pointer_cast<ConfigsvrCoordinator>(std::move(coordinator)),
                std::move(created));
        } catch (const DBException& ex) {
            LOGV2_ERROR(6226201,
                        "Failed to create instance of configsvr coordinator",
                        "reason"_attr = redact(ex));
            throw;
        }
    }();

    // Ensure the existing instance have the same options.
    uassert(ErrorCodes::ConflictingOperationInProgress,
            "Another ConfigsvrCoordinator with different arguments is already running",
            created || coordinator->hasSameOptions(coorDoc));

    return coordinator;
}

std::shared_ptr<ConfigsvrCoordinatorService::Instance>
ConfigsvrCoordinatorService::constructInstance(BSONObj initialState) {
    LOGV2_DEBUG(6347300,
                2,
                "Constructing new ConfigsvrCoordinator instance",
                "initialState"_attr = initialState);

    const auto op = extractConfigsvrCoordinatorMetadata(initialState);
    switch (op.getId().getCoordinatorType()) {
        case ConfigsvrCoordinatorTypeEnum::kSetUserWriteBlockMode:
            return std::make_shared<SetUserWriteBlockModeCoordinator>(std::move(initialState));
        case ConfigsvrCoordinatorTypeEnum::kSetClusterParameter:
            return std::make_shared<SetClusterParameterCoordinator>(std::move(initialState));
        default:
            uasserted(ErrorCodes::BadValue,
                      str::stream() << "Encountered unknown ConfigsvrCoordinator operation type: "
                                    << idl::serialize(op.getId().getCoordinatorType()));
    }
}

bool ConfigsvrCoordinatorService::areAllCoordinatorsOfTypeFinished(
    OperationContext* opCtx, ConfigsvrCoordinatorTypeEnum coordinatorType) {

    // First, check if all in-memory ConfigsvrCoordinators are finished.
    const auto& instances = getAllInstances(opCtx);
    for (const auto& instance : instances) {
        auto typedInstance = checked_pointer_cast<ConfigsvrCoordinator>(instance);
        if (typedInstance->coordinatorType() == coordinatorType) {
            if (!typedInstance->getCompletionFuture().isReady()) {
                return false;
            }
        }
    }

    // If the POS has just been rebuilt on a newly-elected primary, there is a chance that the
    // coordinator instance does not exist yet. Query the state document namespace for any
    // documents that will be built into instances.
    DBDirectClient client(opCtx);
    FindCommandRequest findStateDocs{NamespaceString::kConfigsvrCoordinatorsNamespace};
    findStateDocs.setFilter(BSON("_id" << BSON("coordinatorType" << coordinatorType)));

    return !client.find(std::move(findStateDocs))->more();
}

void ConfigsvrCoordinatorService::waitForAllOngoingCoordinatorsOfType(
    OperationContext* opCtx, ConfigsvrCoordinatorTypeEnum coordinatorType) {
    const auto& instances = getAllInstances(opCtx);
    std::vector<SharedSemiFuture<void>> futuresToWait;
    for (const auto& instance : instances) {
        auto typedInstance = checked_pointer_cast<ConfigsvrCoordinator>(instance);
        if (typedInstance->coordinatorType() == coordinatorType) {
            futuresToWait.push_back(typedInstance->getCompletionFuture());
        }
    }

    for (const auto& inProgressCoordinator : futuresToWait) {
        inProgressCoordinator.wait(opCtx);
    }
}

void ConfigsvrCoordinatorService::checkIfConflictsWithOtherInstances(
    OperationContext* opCtx,
    BSONObj initialState,
    const std::vector<const PrimaryOnlyService::Instance*>& existingInstances) {
    const auto op = extractConfigsvrCoordinatorMetadata(initialState);

    if (op.getId().getCoordinatorType() == ConfigsvrCoordinatorTypeEnum::kSetClusterParameter) {
        const auto stateDoc = SetClusterParameterCoordinatorDocument::parse(
            initialState, IDLParserContext("CoordinatorDocument"));
        if (stateDoc.getCompatibleWithTopologyChange().value_or(false)) {
            return;
        }
    }

    const auto service = ShardingCoordinatorService::getService(opCtx);
    if (!service) {
        return;
    }

    uassert(ErrorCodes::AddOrRemoveShardInProgress,
            fmt::format("Cannot start {} because a topology change is in progress",
                        idl::serialize(op.getId().getCoordinatorType())),
            service->areAllCoordinatorsOfTypeFinished(opCtx, CoordinatorTypeEnum::kAddShard));
}

}  // namespace mongo
