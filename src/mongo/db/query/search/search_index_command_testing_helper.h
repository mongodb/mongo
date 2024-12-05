/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/search/manage_search_index_request_gen.h"
#include "mongo/db/query/search/search_index_options.h"
#include "mongo/db/service_context.h"
#include "mongo/executor/async_multicaster.h"
#include "mongo/s/grid.h"

namespace mongo {

namespace search_index_testing_helper {

inline std::vector<HostAndPort> getAllClusterHosts(OperationContext* opCtx) {
    auto registry = Grid::get(opCtx)->shardRegistry();

    const auto shardIds = registry->getAllShardIds(opCtx);

    std::vector<HostAndPort> servers;

    for (const auto& shardId : shardIds) {
        auto shard = uassertStatusOK(registry->getShard(opCtx, shardId));
        auto cs = shard->getConnString();
        // get all mongods associated with this shard.
        for (auto&& host : cs.getServers()) {
            servers.emplace_back(host);
        }
    }
    return servers;
}

// In production sharded clusters, search index commands are received by mongos which forwards them
// to the SearchIndexManagement service (Envoy) which are then routed to MMS and stored in the
// control plane DB. The mongots makes regular calls to the control plane to get the updated set of
// search indexes.

// However, server testing's infrastructure uses mongot-localdev which is a special mongot binary
// used for local (eg not on Atlas) development.  As such, server's testing infrastructure does not
// have a control plane for managing search index commands. Instead mongot-localdev receives all
// search index commands itself.

// Moreover, the testing environment deploys each mongod with its own mongot (for purposes of server
// testing, "mongot" is a shorthand for "mongot-localdev"). Thus the search index command needs to
// be forwarded to all mongots in the cluster. Previously, this was done via a javascript library
// function that would repeat the search index command on every mongod in the cluster. However, in
// sharded clusters, the views catalog lives exclusively on the primary shard, therefore secondary
// shards cannot resolve the view without making a call to the primary. To support server testing of
// sharded mongot-indexed views, the following system was devised:

// 1. The javascript search index helper calls the search index command on the collection.
// 2. mongos receives the search index command, resolves the view name if necessary, and forwards
// the command to it's assigned mongot (eg searchIndexManagementHostAndPort) which it
// shares with the last spun up mongod.
// 3. mongot completes the request and mongos retrieves the response.
// 4. mongos replicates the search index command on every mongod. It does so by asynchronously
// multicasting _shardSvrRunSearchIndexCommand (with the original user command, the
// alreadyInformedMongot hostAndPort, and the optional resolved view name) on every mongod in the
// cluster.
// 5. Each mongod receives the _shardSvrRunSearchIndexCommand command. If this mongod shares its
// mongot with mongos, it does nothing as its mongot has already received the search index command.
// Otherwise, mongod calls runSearchIndexCommand with the necessary parameters forwarded from
// mongos.
// 6. Once every mongod has forwarded the search index command, mongos returns the response from
// step 3.

inline void _replicateSearchIndexCommandOnAllMongodsForTesting(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const BSONObj& userCmd,
    boost::optional<StringData> viewName) {

    // This helper can only be called by routers for server testing.
    if (!getTestCommandsEnabled() ||
        !opCtx->getService()->role().hasExclusively(ClusterRole::RouterServer)) {
        return;
    }

    BSONObjBuilder bob;
    bob.append("_shardsvrRunSearchIndexCommand", 1);
    bob.append("resolvedNss",
               NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault()));
    bob.append("userCmd", userCmd);
    if (viewName) {
        bob.append("viewName", *viewName);
    }
    /*
     * Fetch the search index management host and port to forward the mongotAlreadyInformed
     * parameters to every mongod. When the mongod receives the _shardSvrRunSearchIndexCommand, it
     * will check that its mongot connection params are not equal to mongotAlreadyInformed as it
     * would redundant to run the search index command on the mongot that mongos already
     * communicated with.
     */
    invariant(!globalSearchIndexParams.host.empty());
    bob.append("mongotAlreadyInformed", globalSearchIndexParams.host);

    // Grab an arbitrary executor.
    auto executor = Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor();

    // Grab all mongods in the cluster.
    auto servers = getAllClusterHosts(opCtx);

    executor::AsyncMulticaster::Options options;

    auto results = executor::AsyncMulticaster(executor, options)
                       .multicast(servers,
                                  nss.dbName(),
                                  bob.obj(),
                                  opCtx,
                                  executor::RemoteCommandRequest::kNoTimeout);

}  // namespace search_index_testing_helper

}  // namespace search_index_testing_helper
}  // namespace mongo
