// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/commands.h"
#include "mongo/db/mirror_maestro.h"
#include "mongo/executor/connection_pool_stats.h"

namespace mongo {

class CmdMirrorMaestroConnPoolStats final : public BasicCommand {
public:
    CmdMirrorMaestroConnPoolStats() : BasicCommand("_mirrorMaestroConnPoolStats") {}

    std::string help() const override {
        return "Internal testing command. Returns an object containing statistics about the "
               "connection pool used for mirroring reads.";
    }

    bool run(OperationContext* opCtx,
             const DatabaseName&,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        auto swExec = getMirroringTaskExecutor_forTest(opCtx->getServiceContext());
        if (!swExec.isOK()) {
            result.append("code", swExec.getStatus().code());
            result.append("codeName", ErrorCodes::errorString(swExec.getStatus().code()));
            result.append("errmsg", swExec.getStatus().reason());
            return false;
        }
        auto exec = swExec.getValue();

        // TODO SERVER-100677: Unify global connection stats collection.
        executor::ConnectionPoolStats stats{};
        exec->appendConnectionStats(&stats);
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
};

class CmdDropMirrorMaestroConnections final : public BasicCommand {
public:
    CmdDropMirrorMaestroConnections() : BasicCommand("_dropMirrorMaestroConnections") {}

    std::string help() const override {
        return "Internal testing command. Used to drop mirroring connections.";
    }

    bool run(OperationContext* opCtx,
             const DatabaseName&,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        auto hps = cmdObj["hostAndPort"].Array();
        auto swExec = getMirroringTaskExecutor_forTest(opCtx->getServiceContext());
        if (!swExec.isOK()) {
            result.append("code", swExec.getStatus().code());
            result.append("codeName", ErrorCodes::errorString(swExec.getStatus().code()));
            result.append("errmsg", swExec.getStatus().reason());
            return false;
        }
        auto exec = swExec.getValue();

        for (auto&& hp : hps) {
            exec->dropConnections(
                HostAndPort(hp.String()),
                Status(ErrorCodes::PooledConnectionsDropped,
                       "Internal testing command to drop the mirroring connection."));
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
};

MONGO_REGISTER_COMMAND(CmdMirrorMaestroConnPoolStats).testOnly().forShard();
MONGO_REGISTER_COMMAND(CmdDropMirrorMaestroConnections).testOnly().forShard();

}  // namespace mongo
