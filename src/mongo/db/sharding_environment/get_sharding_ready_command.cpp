// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/commands.h"
#include "mongo/db/sharding_environment/sharding_ready.h"

namespace mongo {

/* For testing purposes only. */
class GetShardingReadyCmd : public BasicCommand {
public:
    GetShardingReadyCmd() : BasicCommand("getShardingReady") {}

    std::string help() const override {
        return "Internal testing command. Returns if sharding is ready or not.";
    }

    bool run(OperationContext* opCtx,
             const DatabaseName&,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        result.append("isReady", ShardingReady::get(opCtx)->isReady());
        return true;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    bool adminOnly() const override {
        return false;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    Status checkAuthForOperation(OperationContext*,
                                 const DatabaseName&,
                                 const BSONObj&) const override {
        return Status::OK();
    }

    bool requiresAuthzChecks() const override {
        return false;
    }
};
MONGO_REGISTER_COMMAND(GetShardingReadyCmd).forShard().testOnly();
}  // namespace mongo
