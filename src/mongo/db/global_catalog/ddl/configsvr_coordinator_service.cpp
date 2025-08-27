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


#include "mongo/db/global_catalog/ddl/configsvr_coordinator_service.h"

#include "mongo/base/checked_cast.h"
#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/dbclient_cursor.h"
#include "mongo/db/cluster_parameters/set_cluster_parameter_coordinator.h"
#include "mongo/db/cluster_parameters/sharding_cluster_parameters_gen.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/global_catalog/ddl/configsvr_coordinator.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_coordinator_service.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/user_write_block/set_user_write_block_mode_coordinator.h"
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
                      str::stream()
                          << "Encountered unknown ConfigsvrCoordinator operation type: "
                          << ConfigsvrCoordinatorType_serializer(op.getId().getCoordinatorType()));
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
    // the coordinator instance does not exist yet. Query the state document namespace for any
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
    if (op.getId().getCoordinatorType() != ConfigsvrCoordinatorTypeEnum::kSetClusterParameter) {
        return;
    }

    const auto stateDoc = SetClusterParameterCoordinatorDocument::parse(
        initialState, IDLParserContext("CoordinatorDocument"));
    if (stateDoc.getCompatibleWithTopologyChange()) {
        return;
    }

    const auto service = ShardingDDLCoordinatorService::getService(opCtx);
    if (!service) {
        return;
    }

    uassert(ErrorCodes::AddOrRemoveShardInProgress,
            "Cannot start SetClusterParameterCoordinator because a topology change is in progress",
            service->areAllCoordinatorsOfTypeFinished(opCtx, DDLCoordinatorTypeEnum::kAddShard));
}

}  // namespace mongo
