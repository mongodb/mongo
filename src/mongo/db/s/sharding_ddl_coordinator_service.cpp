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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/sharding_ddl_coordinator_service.h"

#include "mongo/base/checked_cast.h"
#include "mongo/db/s/database_sharding_state.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/sharding_ddl_coordinator.h"
#include "mongo/logv2/log.h"

namespace mongo {

ShardingDDLCoordinatorService* ShardingDDLCoordinatorService::getService(OperationContext* opCtx) {
    auto registry = repl::PrimaryOnlyServiceRegistry::get(opCtx->getServiceContext());
    auto service = registry->lookupServiceByName(kServiceName);
    return checked_cast<ShardingDDLCoordinatorService*>(std::move(service));
}

std::shared_ptr<ShardingDDLCoordinatorService::Instance>
ShardingDDLCoordinatorService::constructInstance(BSONObj initialState) const {
    const auto op = extractShardingDDLCoordinatorMetadata(initialState);
    LOGV2_DEBUG(5390510,
                2,
                "Constructing new sharding DDL coordinator",
                "coordinatorDoc"_attr = op.toBSON());
    switch (op.getId().getOperationType()) {
        case DDLCoordinatorTypeEnum::kDropCollection:
            // Switch with only default case are not supported on windows compiler
            return {};
            break;
        default:
            uasserted(ErrorCodes::BadValue,
                      str::stream()
                          << "Encountered unknown Sharding DDL operation type: "
                          << DDLCoordinatorType_serializer(op.getId().getOperationType()));
    }
}

std::shared_ptr<ShardingDDLCoordinatorService::Instance>
ShardingDDLCoordinatorService::getOrCreateInstance(OperationContext* opCtx, BSONObj coorDoc) {
    auto coorMetadata = extractShardingDDLCoordinatorMetadata(coorDoc);
    const auto& nss = coorMetadata.getId().getNss();

    if (!nss.isConfigDB()) {
        // Check that the operation context has a database version for this namespace
        const auto clientDbVersion = OperationShardingState::get(opCtx).getDbVersion(nss.db());
        uassert(ErrorCodes::IllegalOperation,
                "Request sent without attaching database version",
                clientDbVersion);
        DatabaseShardingState::checkIsPrimaryShardForDb(opCtx, nss.db());
        coorMetadata.setDatabaseVersion(clientDbVersion);
    }

    coorMetadata.setForwardableOpMetadata(boost::optional<ForwardableOperationMetadata>(opCtx));
    const auto patchedCoorDoc = coorDoc.addFields(coorMetadata.toBSON());

    auto [coordinator, created] = [&] {
        try {
            auto [coordinator, created] =
                PrimaryOnlyService::getOrCreateInstance(opCtx, patchedCoorDoc);
            return std::make_pair(
                checked_pointer_cast<ShardingDDLCoordinator>(std::move(coordinator)),
                std::move(created));
        } catch (const DBException& ex) {
            LOGV2_ERROR(5390512,
                        "Failed to create instance of sharding DDL coordinator",
                        "coordinatorId"_attr = coorMetadata.getId(),
                        "reason"_attr = redact(ex));
            throw;
        }
    }();

    // If the existing instance doesn't have conflicting options just return that one
    if (!created) {
        coordinator->checkIfOptionsConflict(coorDoc);
    }

    coordinator->getConstructionCompletionFuture().get(opCtx);
    return coordinator;
}

}  // namespace mongo
