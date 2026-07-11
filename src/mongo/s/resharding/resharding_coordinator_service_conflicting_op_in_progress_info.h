// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/base/error_extra_info.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/repl/primary_only_service.h"
#include "mongo/util/modules.h"

#include <memory>

namespace mongo {

class [[MONGO_MOD_PUBLIC]] ReshardingCoordinatorServiceConflictingOperationInProgressInfo final
    : public ErrorExtraInfo {
public:
    static constexpr auto code =
        ErrorCodes::ReshardingCoordinatorServiceConflictingOperationInProgress;

    explicit ReshardingCoordinatorServiceConflictingOperationInProgressInfo(
        std::shared_ptr<const repl::PrimaryOnlyService::Instance> instance);

    ReshardingCoordinatorServiceConflictingOperationInProgressInfo() = default;

    void serialize(BSONObjBuilder* builder) const override;

    static std::shared_ptr<const ErrorExtraInfo> parse(const BSONObj&);

    std::shared_ptr<const repl::PrimaryOnlyService::Instance> getInstance() const;

private:
    // This variable is of type repl::PrimaryOnlyService::Instance rather than a
    // ReshardingCoordinator instance to avoid linking the
    // ReshardingCoordinatorService::ReshardingCoordinator type into mongos and the mongo shell
    std::shared_ptr<const repl::PrimaryOnlyService::Instance> reshardingCoordinatorInstance;
};

}  // namespace mongo
