// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/s/resharding/resharding_coordinator_service_conflicting_op_in_progress_info.h"

#include "mongo/base/init.h"  // IWYU pragma: keep

#include <utility>

namespace mongo {

MONGO_INIT_REGISTER_ERROR_EXTRA_INFO(
    ReshardingCoordinatorServiceConflictingOperationInProgressInfo);

ReshardingCoordinatorServiceConflictingOperationInProgressInfo::
    ReshardingCoordinatorServiceConflictingOperationInProgressInfo(
        std::shared_ptr<const repl::PrimaryOnlyService::Instance> instance)
    : reshardingCoordinatorInstance(std::move(instance)) {}

void ReshardingCoordinatorServiceConflictingOperationInProgressInfo::serialize(
    BSONObjBuilder* builder) const {}

std::shared_ptr<const ErrorExtraInfo>
ReshardingCoordinatorServiceConflictingOperationInProgressInfo::parse(const BSONObj& obj) {
    return std::make_shared<ReshardingCoordinatorServiceConflictingOperationInProgressInfo>(
        ReshardingCoordinatorServiceConflictingOperationInProgressInfo());
}

std::shared_ptr<const repl::PrimaryOnlyService::Instance>
ReshardingCoordinatorServiceConflictingOperationInProgressInfo::getInstance() const {
    return reshardingCoordinatorInstance;
}

}  // namespace mongo
