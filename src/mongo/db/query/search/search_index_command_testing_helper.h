/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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
#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/query_cmd/search_index_commands_gen.h"
#include "mongo/db/commands/shardsvr_run_search_index_command_gen.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/client_cursor/cursor_response.h"
#include "mongo/db/query/search/manage_search_index_request_gen.h"
#include "mongo/db/query/search/mongot_options.h"
#include "mongo/db/query/search/search_index_common.h"
#include "mongo/db/query/search/search_index_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/executor/async_multicaster.h"
#include "mongo/logv2/log.h"
#include "mongo/util/stacktrace.h"

#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {

namespace search_index_testing_helper {

constexpr mongo::StringData kListCommand = "$listSearchIndexes"_sd;
constexpr mongo::StringData kCreateCommand = "createSearchIndexes"_sd;
constexpr mongo::StringData kUpdateCommand = "updateSearchIndex"_sd;
constexpr mongo::StringData kDropCommand = "dropSearchIndex"_sd;
// In production sharded clusters, search index commands are received by the router which forwards
// them to the SearchIndexManagement service (Envoy) which are then routed to MMS and stored in the
// control plane DB. The mongots makes regular calls to the control plane to get the updated set of
// search indexes.

// However, server testing's infrastructure uses mongot-localdev which is a special mongot binary
// used for local (eg not on Atlas) development.  As such, server's testing infrastructure does not
// have a control plane for managing search index commands. Instead mongot-localdev receives all
// search index commands itself.

// Moreover, the testing environment deploys each mongod with its own mongot (for purposes of server
// testing, "mongot" is a shorthand for "mongot-localdev"). Thus the search index command needs to
// be forwarded to all mongots in the cluster. To support server testing of sharded mongot-indexed
// views, the following system was devised:

// 1. The javascript search index command helper calls the search index command on the request nss.
// 2. The router receives the search index command, resolves the view name if necessary, and
// forwards the command to it's assigned mongot-localdev (eg searchIndexManagementHostAndPort) which
// it shares with the last spun up mongod.
// 3. mongot completes the request and the router retrieves and returns the response.
// 4. The javascript search index helper calls _runAndReplicateSearchIndexCommand(), which sends a
// replicateSearchIndexCommand to the router with the original user command.
// 5. replicateSearchIndexCommand::typedRun() calls
// search_index_testing_helper::_replicateSearchIndexCommandOnAllMongodsForTesting(). This helper
// resolves the view name (if necessary) and then asynchronously multicasts
// _shardsvrRunSearchIndexCommand (which includes the original user command, the
// alreadyInformedMongot hostAndPort, and the optional resolved view name) on every mongod in the
// cluster.
// 6. Each mongod receives the _shardsvrRunSearchIndexCommand command. If this mongod shares its
// mongot with the router, it does nothing as its mongot has already received the search index
// command. Otherwise, mongod calls runSearchIndexCommand with the necessary parameters forwarded
// from the router.
// 7. After every mongod has been issued the _shardsvrRunSearchIndexCommand,
// search_index_testing_helper::_replicateSearchIndexCommandOnAllMongodsForTesting() then issues a
// $listSearchIndex command on every mongod until every mongod reports that the specified index is
// queryable. It will return once the index is queryable across the entire cluster and throw an
// error otherwise.
// 8. The javascript search index command helper returns the response from step 3.


inline constexpr Milliseconds kRetryPeriodMs = Milliseconds{500};
inline constexpr Seconds kRemoteCommandTimeout{60};

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

inline bool indexIsReady(const BSONObj& cmdResponseBson,
                         boost::optional<BSONObj> indexDefinitionFromUserCmd) {
    auto reply = ShardsvrRunSearchIndexCommandReply::parse(
        cmdResponseBson, IDLParserContext("ShardsvrRunSearchIndexCommandReply"));
    auto searchIndexManagerResponse = reply.getSearchIndexManagerResponse();
    tassert(9638405,
            "We should have a cursor field in "
            "ShardsvrRunSearchIndexCommandReply.searchIndexManagerResponse for $listSearchIndexes "
            "requests",
            searchIndexManagerResponse->getCursor());

    auto batch = searchIndexManagerResponse->getCursor()->getFirstBatch();

    if (batch.empty()) {
        return false;
    }
    auto idxEntryFromMongot = batch[0];

    if (idxEntryFromMongot.getStringField("status") == "READY") {
        if (!indexDefinitionFromUserCmd) {
            return true;
        } else if (indexDefinitionFromUserCmd->woCompare(
                       idxEntryFromMongot["latestDefinition"].Obj()) == 0) {
            // This check represents a case where a test creates a search index and then
            // subsequently updates the definition of that idx. This test ensures that the idx
            // that mongot is reporting as READY, matches the idx definition from the update
            // command. Otherwise, the READY status refers to a stale idx entry from the initial
            // create command.
            return true;
        }
        // This is a stale index entry.
        return false;
    }
    return false;
}

using Reply = std::tuple<HostAndPort, executor::RemoteCommandResponse>;
// TODO SERVER-101352 instead of multicast, issue the search command on each host sequentially and
// block until command is completed.
inline std::vector<Reply> multiCastShardsvrRunSearchIndexCommandOnAllMongods(
    OperationContext* opCtx,
    std::vector<HostAndPort> allClusterHosts,
    const DatabaseName& dbName,
    const BSONObj& ShardsvrRunSearchIndexCmdObj) {

    auto executor = Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor();

    executor::AsyncMulticaster::Options options;

    auto results = executor::AsyncMulticaster(executor, options)
                       .multicast(allClusterHosts,
                                  dbName,
                                  ShardsvrRunSearchIndexCmdObj,
                                  opCtx,
                                  executor::RemoteCommandRequest::kNoTimeout);

    return results;
}

// should probably make this private somehow
inline void blockUntilIndexQueryable(OperationContext* opCtx,
                                     const DatabaseName& dbName,
                                     const BSONObj& listSearchIndexesCmdObj,
                                     std::vector<HostAndPort> allClusterHosts,
                                     boost::optional<BSONObj> indexDefinition) {
    auto& clock = opCtx->fastClockSource();
    // This is 10 minutes to match the max timeout of assert.soon() when running on evergreen.
    // TODO SERVER-101359 dynamically set maxTimeout depending on if we're running on evergreen or
    // locally.
    auto maxTimeout = Milliseconds(10 * 60 * 1000);
    auto runElapsed = Milliseconds(0);
    auto executor = Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor();

    for (auto& host : allClusterHosts) {
        auto runStart = clock.now();
        do {
            executor::RemoteCommandRequest request(host,
                                                   dbName,
                                                   listSearchIndexesCmdObj,
                                                   rpc::makeEmptyMetadata(),
                                                   opCtx,
                                                   kRemoteCommandTimeout);

            executor::RemoteCommandResponse response(
                host, Status(ErrorCodes::InternalError, "Internal error running command"));

            auto callbackHandle = uassertStatusOK(executor->scheduleRemoteCommand(
                request,
                [&response](const executor::TaskExecutor::RemoteCommandCallbackArgs& args) {
                    response = args.response;
                }));

            try {
                // Block until the command is carried out
                executor->wait(callbackHandle);
            } catch (const DBException& ex) {
                LOGV2(
                    9638401, "DB Exception running command ", "error"_attr = redact(ex.toStatus()));
                // If waiting for the response is interrupted, then we still have a callback out and
                // registered with the TaskExecutor to run when the response finally does come back.
                // Since the callback references local state, cbkResponse, it would be invalid for
                // the callback to run after leaving the this function. Therefore, we cancel the
                // callback and wait uninterruptably for the callback to be run.
                executor->cancel(callbackHandle);
                executor->wait(callbackHandle);
                throw;
            }


            if (response.status == ErrorCodes::ExceededTimeLimit) {
                LOGV2(9638402, "Operation timed out", "error"_attr = redact(response.status));
            }

            if (!response.isOK()) {
                if (!Shard::shouldErrorBePropagated(response.status.code())) {
                    uasserted(ErrorCodes::OperationFailed,
                              str::stream() << "failed to run command " << listSearchIndexesCmdObj
                                            << causedBy(response.status));
                }
                uassertStatusOK(response.status);
            }

            BSONObj result = response.data.getOwned();
            uassertStatusOKWithContext(getStatusFromCommandResult(result),
                                       "blockUntilIndexQueryable failed");

            LOGV2_DEBUG(
                9638403, 0, "One response", "result"_attr = result, "hostAndPort"_attr = host);

            if (indexIsReady(result, indexDefinition)) {
                break;
            }

            LOGV2_DEBUG(9638404, 1, "Index not yet queryable, retrying", "response"_attr = result);

            runElapsed = clock.now() - runStart;
        } while (runElapsed < maxTimeout);
    }

    if (runElapsed > maxTimeout) {
        uasserted(9638406, "Index is not replicated and queryable within the max timeout");
    }
}


inline BSONObj wrapCmdInShardSvrRunSearchIndexCmd(const NamespaceString& nss,
                                                  const BSONObj& userCmd,
                                                  boost::optional<SearchQueryViewSpec> view) {
    BSONObjBuilder bob;
    bob.append("_shardsvrRunSearchIndexCommand", 1);
    bob.append("resolvedNss",
               NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault()));
    bob.append("userCmd", userCmd);

    if (view) {
        bob.append("view", view->toBSON());
    }
    /*
     * Fetch the search index management host and port to forward the mongotAlreadyInformed
     * parameters to every mongod. When the mongod receives the _shardsvrRunSearchIndexCommand, it
     * will check that its mongot connection params are not equal to mongotAlreadyInformed as it
     * would redundant to run the search index command on the mongot that mongos already
     * communicated with.
     */
    invariant(!globalSearchIndexParams.host.empty());
    bob.append("mongotAlreadyInformed", globalSearchIndexParams.host);
    return bob.obj();
}

inline BSONObj createWrappedListSearchIndexesCmd(const NamespaceString& nss,
                                                 const BSONObj& cmd,
                                                 boost::optional<SearchQueryViewSpec> view) {
    auto idxCmdType = std::string(cmd.firstElement().fieldName());
    // In order to use the IDL commands for retrieving the index name, we have to add $db field.
    auto newCmdObj = cmd.addField(BSON("$db" << DatabaseNameUtil::serialize(
                                           nss.dbName(), SerializationContext::stateDefault()))
                                      .firstElement());
    BSONObj listSearchIndexes;
    if (idxCmdType.compare(std::string{kCreateCommand}) == 0) {

        IndexDefinition indexDefinition =
            CreateSearchIndexesCommand::parse(newCmdObj,
                                              IDLParserContext("createWrappedListSearchIndexesCmd"))
                .getIndexes()
                .front();

        if (indexDefinition.getName()) {
            listSearchIndexes =
                BSON("$listSearchIndexes" << BSON("name" << *indexDefinition.getName()));
        } else {
            listSearchIndexes = BSON("$listSearchIndexes" << BSON("name" << "default"));
        }

    } else if (idxCmdType.compare(std::string{kUpdateCommand}) == 0) {
        auto updateCmd = UpdateSearchIndexCommand::parse(
            newCmdObj, IDLParserContext("createWrappedListSearchIndexesCmd"));
        if (updateCmd.getName()) {
            listSearchIndexes = BSON("$listSearchIndexes" << BSON("name" << *updateCmd.getName()));
        } else {
            listSearchIndexes = BSON("$listSearchIndexes" << BSON("name" << "default"));
        }
    }

    return wrapCmdInShardSvrRunSearchIndexCmd(nss, listSearchIndexes, view);
}

inline void _replicateSearchIndexCommandOnAllMongodsForTesting(OperationContext* opCtx,
                                                               const NamespaceString& nss,
                                                               const BSONObj& userCmd) {
    const auto [collUUID, resolvedNss, view] = uassertStatusOKWithContext(
        retrieveCollectionUUIDAndResolveView(opCtx, nss), "Error retrieving collection UUID");

    // This helper can only be called by routers for server testing with a real mongot (eg not tests
    // that use mongotmock).
    if (!getTestCommandsEnabled() ||
        !opCtx->getService()->role().hasExclusively(ClusterRole::RouterServer)) {
        return;
    }
    auto idxCmdType = std::string(userCmd.firstElement().fieldName());
    auto allClusterHosts = getAllClusterHosts(opCtx);
    const auto dbName = nss.dbName();
    BSONObj listSearchIndexesCmd;
    boost::optional<BSONObj> searchIdxLatestDefinition;
    if (idxCmdType.compare(std::string{kListCommand}) == 0) {
        listSearchIndexesCmd = wrapCmdInShardSvrRunSearchIndexCmd(resolvedNss, userCmd, view);
    } else {
        auto cmdObj = wrapCmdInShardSvrRunSearchIndexCmd(resolvedNss, userCmd, view);
        multiCastShardsvrRunSearchIndexCommandOnAllMongods(opCtx, allClusterHosts, dbName, cmdObj);
        if (idxCmdType.compare(std::string{kCreateCommand}) == 0 ||
            idxCmdType.compare(std::string{kUpdateCommand}) == 0) {
            listSearchIndexesCmd = createWrappedListSearchIndexesCmd(resolvedNss, userCmd, view);
            if (idxCmdType.compare(std::string{kUpdateCommand}) == 0) {
                if (userCmd.hasField("definition") &&
                    userCmd["definition"].type() == BSONType::object) {
                    searchIdxLatestDefinition = boost::make_optional(userCmd["definition"].Obj());
                }
            }
        }
    }

    if (idxCmdType.compare(std::string{kDropCommand}) == 0) {
        // dropSearchIndex command doesn't return until the specified index is fully wiped.
        return;
    }
    blockUntilIndexQueryable(
        opCtx, dbName, listSearchIndexesCmd, allClusterHosts, searchIdxLatestDefinition);
    return;
}

}  // namespace search_index_testing_helper
}  // namespace mongo

#undef MONGO_LOGV2_DEFAULT_COMPONENT
