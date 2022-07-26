/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include <vector>

#include "mongo/base/init.h"
#include "mongo/db/commands.h"
#include "mongo/executor/async_multicaster.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/commands/cluster_commands_gen.h"
#include "mongo/s/grid.h"
#include "mongo/util/duration.h"

namespace mongo {
namespace {

std::vector<HostAndPort> getAllClusterHosts(OperationContext* opCtx) {
    auto registry = Grid::get(opCtx)->shardRegistry();

    const auto shardIds = registry->getAllShardIds(opCtx);

    std::vector<HostAndPort> servers;
    for (const auto& shardId : shardIds) {
        auto shard = uassertStatusOK(registry->getShard(opCtx, shardId));

        auto cs = shard->getConnString();
        for (auto&& host : cs.getServers()) {
            servers.emplace_back(host);
        }
    }

    return servers;
}

class ClusterMulticastCmd : public BasicCommand {
public:
    ClusterMulticastCmd() : BasicCommand("multicast") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool adminOnly() const override {
        return true;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    std::string help() const override {
        return "multicasts a command to hosts in a system";
    }

    // no privs because it's a test command
    void addRequiredPrivileges(const std::string& dbname,
                               const BSONObj& cmdObj,
                               std::vector<Privilege>* out) const override {}

    bool run(OperationContext* opCtx,
             const std::string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        IDLParserContext ctx("ClusterMulticast");
        auto args = ClusterMulticast::parse(ctx, cmdObj);

        // Grab an arbitrary executor.
        auto executor = Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor();

        // Grab all hosts in the cluster.
        auto servers = getAllClusterHosts(opCtx);

        executor::AsyncMulticaster::Options options;
        if (args.getConcurrency()) {
            options.maxConcurrency = *args.getConcurrency();
        }

        auto results =
            executor::AsyncMulticaster(executor, options)
                .multicast(servers,
                           args.getDb().toString(),
                           args.getMulticast(),
                           opCtx,
                           (args.getTimeout() ? Milliseconds(*args.getTimeout())
                                              : executor::RemoteCommandRequest::kNoTimeout));

        bool success = true;

        BSONObjBuilder bob(result.subobjStart("hosts"));

        for (const auto& r : results) {
            HostAndPort host;
            executor::RemoteCommandResponse response;
            std::tie(host, response) = r;

            if (!response.isOK() || !response.data["ok"].trueValue()) {
                success = false;
            }

            {
                BSONObjBuilder subbob(bob.subobjStart(host.toString()));

                if (CommandHelpers::appendCommandStatusNoThrow(subbob, response.status)) {
                    subbob.append("data", response.data);
                    if (response.elapsed) {
                        subbob.append("elapsedMillis",
                                      durationCount<Milliseconds>(*response.elapsed));
                    }
                }
            }
        }

        return success;
    }
};

MONGO_REGISTER_TEST_COMMAND(ClusterMulticastCmd);

}  // namespace
}  // namespace mongo
