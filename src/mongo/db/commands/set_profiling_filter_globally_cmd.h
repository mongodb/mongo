// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/shard_catalog/collection_catalog.h"
#include "mongo/util/modules.h"

#include <string>

namespace mongo {

class SetProfilingFilterGloballyCmdRequest;

/**
 * Command class implementing functionality for both the mongoD and mongoS
 * 'setProfilingFilterGlobally' command.
 */
class SetProfilingFilterGloballyCmd : public BasicCommand {
public:
    SetProfilingFilterGloballyCmd() : BasicCommand("setProfilingFilterGlobally") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return AllowedOnSecondary::kAlways;
    }

    std::string help() const final {
        return "updates a global filter that determines which operations are eligible for "
               "logging/profiling";
    }

    bool supportsWriteConcern(const BSONObj& cmd) const final {
        return false;
    }

    Status checkAuthForOperation(OperationContext*,
                                 const DatabaseName&,
                                 const BSONObj&) const final;

    bool run(OperationContext* opCtx,
             const DatabaseName& dbName,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) final;
};
}  // namespace mongo
