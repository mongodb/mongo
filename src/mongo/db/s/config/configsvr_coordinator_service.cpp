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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/config/configsvr_coordinator_service.h"

#include "mongo/base/checked_cast.h"
#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/s/config/configsvr_coordinator.h"
#include "mongo/db/s/config/set_cluster_parameter_coordinator.h"
#include "mongo/db/s/config/set_user_write_block_mode_coordinator.h"
#include "mongo/logv2/log.h"

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

}  // namespace mongo
