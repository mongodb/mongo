// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/commands.h"
#include "mongo/db/query/search/mongot_options.h"
#include "mongo/db/query/search/search_task_executors.h"
#include "mongo/executor/connection_pool_stats.h"

namespace mongo {

class CmdMongotConnPoolStats final : public BasicCommand {
public:
    CmdMongotConnPoolStats() : BasicCommand("_mongotConnPoolStats") {}

    std::string help() const override {
        return "internal testing command. Returns an object containing statistics about the "
               "connection pool between the server and mongot";
    }

    bool run(OperationContext* opCtx,
             const DatabaseName&,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        auto mongotExec =
            uassertStatusOK(executor::getMongotTaskExecutor(opCtx->getServiceContext()));
        // The metrics reported through appendConnectionPoolStats do not map cleanly to gRPC's
        // networking concepts, so when gRPC is enabled, we report the metrics separately. This stat
        // reporting will be revisited in SERVER-100677
        if (globalMongotParams.useGRPC) {
            mongotExec->appendNetworkInterfaceStats(result);
            return true;
        }

        // TODO SERVER-100677: Unify global connection stats collection.
        executor::ConnectionPoolStats stats{};
        mongotExec->appendConnectionStats(&stats);
        stats.appendToBSON(result);
        return true;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    bool adminOnly() const override {
        return true;
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

class CmdDropConnectionsToMongot final : public BasicCommand {
public:
    CmdDropConnectionsToMongot() : BasicCommand("_dropConnectionsToMongot") {}

    std::string help() const override {
        return "internal testing command. Used to drop connections between the server and mongot";
    }

    bool run(OperationContext* opCtx,
             const DatabaseName&,
             const BSONObj& cmdObj,
             BSONObjBuilder&) override {
        auto hps = cmdObj["hostAndPort"].Array();
        auto mongotExec =
            uassertStatusOK(executor::getMongotTaskExecutor(opCtx->getServiceContext()));
        for (auto&& hp : hps) {
            mongotExec->dropConnections(HostAndPort(hp.String()),
                                        Status(ErrorCodes::PooledConnectionsDropped,
                                               "Internal testing command to drop the connections "
                                               "between the server and mongot"));
        }
        return true;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    bool adminOnly() const override {
        return true;
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

MONGO_REGISTER_COMMAND(CmdMongotConnPoolStats).testOnly().forShard().forRouter();
MONGO_REGISTER_COMMAND(CmdDropConnectionsToMongot).testOnly().forShard().forRouter();

}  // namespace mongo
