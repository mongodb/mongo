/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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
