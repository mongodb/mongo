// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/router_role/cluster_commands_helpers.h"
#include "mongo/db/router_role/collection_routing_info_targeter.h"
#include "mongo/db/router_role/router_role.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/ddl/list_indexes_gen.h"
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

// TODO SERVER-120795 Once all timeseries are viewless (9.0 is last LTS), translatedNss and origNss
// will always be the same and can be collapsed into one namespace (nss)
ListIndexesReply cursorCommandPassthroughShardWithMinKeyChunk(OperationContext* opCtx,
                                                              RoutingContext& routingCtx,
                                                              const NamespaceString& translatedNss,
                                                              const NamespaceString& origNss,
                                                              const BSONObj& cmdObj,
                                                              const PrivilegeVector& privileges) {
    auto response = executeCommandAgainstShardWithMinKeyChunk(
        opCtx,
        routingCtx,
        translatedNss,
        CommandHelpers::filterCommandRequestForPassthrough(cmdObj),
        ReadPreferenceSetting::get(opCtx),
        Shard::RetryPolicy::kIdempotent);
    const auto cmdResponse = uassertStatusOK(std::move(response.swResponse));

    auto transformedResponse = uassertStatusOK(
        storePossibleCursor(opCtx,
                            response.shardId,
                            *response.shardHostAndPort,
                            cmdResponse.data,
                            origNss,
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

            sharding::router::CollectionRouter router(opCtx, ns());
            return router.routeWithRoutingContext(
                Request::kCommandName,
                [&](OperationContext* opCtx, RoutingContext& unusedRoutingCtx) {
                    // The CollectionRouter is not capable of implicitly translate the namespace to
                    // a timeseries buckets collection, which is required in this command. Hence,
                    // we'll use the CollectionRouter to handle StaleConfig errors but will ignore
                    // its RoutingContext. Instead, we'll use a CollectionRoutingInfoTargeter object
                    // to properly get the RoutingContext when the collection is timeseries.
                    // TODO (SERVER-117193) Use the RoutingContext provided by the CollectionRouter
                    // once all timeseries collections become viewless.
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
                                ns(),
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
