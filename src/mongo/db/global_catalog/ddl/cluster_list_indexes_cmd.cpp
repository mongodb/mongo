/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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


#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/global_catalog/router_role_api/cluster_commands_helpers.h"
#include "mongo/db/global_catalog/router_role_api/collection_routing_info_targeter.h"
#include "mongo/db/global_catalog/router_role_api/router_role.h"
#include "mongo/db/local_catalog/ddl/list_indexes_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/timeseries/timeseries_commands_conversion_helper.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/s/query/exec/store_possible_cursor.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/str.h"

#include <memory>
#include <set>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace {

ListIndexesReply cursorCommandPassthroughShardWithMinKeyChunk(OperationContext* opCtx,
                                                              RoutingContext& routingCtx,
                                                              const NamespaceString& nss,
                                                              const BSONObj& cmdObj,
                                                              const PrivilegeVector& privileges) {
    auto response = executeCommandAgainstShardWithMinKeyChunk(
        opCtx,
        routingCtx,
        nss,
        CommandHelpers::filterCommandRequestForPassthrough(cmdObj),
        ReadPreferenceSetting::get(opCtx),
        Shard::RetryPolicy::kIdempotent);
    const auto cmdResponse = uassertStatusOK(std::move(response.swResponse));

    auto transformedResponse = uassertStatusOK(
        storePossibleCursor(opCtx,
                            response.shardId,
                            *response.shardHostAndPort,
                            cmdResponse.data,
                            nss,
                            Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor(),
                            Grid::get(opCtx)->getCursorManager(),
                            privileges));

    BSONObjBuilder out;
    CommandHelpers::filterCommandReplyForPassthrough(transformedResponse, &out);
    const auto& resultObj = out.obj();
    uassertStatusOK(getStatusFromCommandResult(resultObj));
    // The reply syntax must conform to its IDL definition.
    return ListIndexesReply::parse(resultObj, IDLParserContext{"listIndexes"});
}

class CmdListIndexes final : public ListIndexesCmdVersion1Gen<CmdListIndexes> {
public:
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return AllowedOnSecondary::kAlways;
    }
    bool maintenanceOk() const final {
        return false;
    }
    bool adminOnly() const final {
        return false;
    }

    class Invocation final : public InvocationBaseGen {
    public:
        using InvocationBaseGen::InvocationBaseGen;

        bool supportsWriteConcern() const final {
            return false;
        }

        bool supportsRawData() const final {
            return true;
        }

        NamespaceString ns() const final {
            uassert(ErrorCodes::BadValue,
                    "Mongos requires a namespace for listIndexes command",
                    request().getNamespaceOrUUID().isNamespaceString());
            return request().getNamespaceOrUUID().nss();
        }

        void doCheckAuthorization(OperationContext* opCtx) const final {
            AuthorizationSession* authzSession = AuthorizationSession::get(opCtx->getClient());
            uassert(ErrorCodes::Unauthorized,
                    str::stream() << "Not authorized to list indexes on collection:"
                                  << ns().toStringForErrorMsg(),
                    authzSession->isAuthorizedForActionsOnResource(
                        ResourcePattern::forExactNamespace(ns()), ActionType::listIndexes));
        }

        ListIndexesReply typedRun(OperationContext* opCtx) final {
            CommandHelpers::handleMarkKillOnClientDisconnect(opCtx);

            sharding::router::CollectionRouter router{opCtx->getServiceContext(), ns()};
            return router.routeWithRoutingContext(
                opCtx,
                Request::kCommandName,
                [&](OperationContext* opCtx, RoutingContext& unusedRoutingCtx) {
                    // The CollectionRouter is not capable of implicitly translate the namespace to
                    // a timeseries buckets collection, which is required in this command. Hence,
                    // we'll use the CollectionRouter to handle StaleConfig errors but will ignore
                    // its RoutingContext. Instead, we'll use a CollectionRoutingInfoTargeter object
                    // to properly get the RoutingContext when the collection is timeseries.
                    // TODO (SPM-3830) Use the RoutingContext provided by the CollectionRouter once
                    // all timeseries collections become viewless.
                    unusedRoutingCtx.skipValidation();

                    // The command's IDL definition permits namespace or UUID, but mongos requires a
                    // namespace.
                    auto targeter = CollectionRoutingInfoTargeter(opCtx, ns());
                    return routing_context_utils::runAndValidate(
                        targeter.getRoutingCtx(), [&](RoutingContext& routingCtx) {
                            auto& cmd = request();
                            setReadWriteConcern(opCtx, cmd, this);
                            auto cmdToBeSent = cmd.toBSON();
                            if (targeter.timeseriesNamespaceNeedsRewrite(ns())) {
                                cmdToBeSent = timeseries::makeTimeseriesCommand(
                                    cmdToBeSent,
                                    ns(),
                                    ListIndexes::kCommandName,
                                    ListIndexes::kIsTimeseriesNamespaceFieldName);
                            }

                            return cursorCommandPassthroughShardWithMinKeyChunk(
                                opCtx,
                                routingCtx,
                                targeter.getNS(),
                                cmdToBeSent,
                                {Privilege(ResourcePattern::forExactNamespace(ns()),
                                           ActionType::listIndexes)});
                        });
                });
        }
    };
};
MONGO_REGISTER_COMMAND(CmdListIndexes).forRouter();

}  // namespace
}  // namespace mongo
