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

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/connection_string.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/cluster_commands_gen.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/executor/async_multicaster.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/net/hostandport.h"

#include <cstdint>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

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
    Status checkAuthForOperation(OperationContext*,
                                 const DatabaseName&,
                                 const BSONObj&) const override {
        return Status::OK();
    }

    bool run(OperationContext* opCtx,
             const DatabaseName&,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        IDLParserContext ctx("ClusterMulticast");
        auto args = ClusterMulticast::parse(cmdObj, ctx);

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
                           args.getDb(),
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

MONGO_REGISTER_COMMAND(ClusterMulticastCmd).testOnly().forRouter();

}  // namespace
}  // namespace mongo
