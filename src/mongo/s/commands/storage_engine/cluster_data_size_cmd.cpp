// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/dbcommands_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/legacy_runtime_constants_gen.h"
#include "mongo/db/router_role/cluster_commands_helpers.h"
#include "mongo/db/router_role/router_role.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace {

class DataSizeCmd : public TypedCommand<DataSizeCmd> {
public:
    using Request = DataSizeCommand;
    using Reply = typename Request::Reply;

    DataSizeCmd() : TypedCommand(Request::kCommandName, Request::kCommandAlias) {}

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        NamespaceString ns() const final {
            return request().getCommandParameter();
        }

        bool supportsWriteConcern() const final {
            return false;
        }

        void doCheckAuthorization(OperationContext* opCtx) const final {
            auto* as = AuthorizationSession::get(opCtx->getClient());
            uassert(ErrorCodes::Unauthorized,
                    "unauthorized",
                    as->isAuthorizedForActionsOnResource(ResourcePattern::forExactNamespace(ns()),
                                                         ActionType::find));
        }

        Reply typedRun(OperationContext* opCtx) {
            auto& cmd = request();
            const auto& nss = ns();

            setReadWriteConcern(opCtx, cmd, this);

            sharding::router::CollectionRouter router(opCtx, nss);
            return router.routeWithRoutingContext(
                Request::kCommandParameterFieldName,
                [&](OperationContext* opCtx, RoutingContext& routingCtx) {
                    auto shardResults = scatterGatherVersionedTargetByRoutingTable(
                        opCtx,
                        routingCtx,
                        nss,
                        CommandHelpers::filterCommandRequestForPassthrough(cmd.toBSON()),
                        ReadPreferenceSetting::get(opCtx),
                        Shard::RetryPolicy::kIdempotent,
                        {} /*query*/,
                        {} /*collation*/,
                        boost::none /*letParameters*/,
                        boost::none /*runtimeConstants*/);

                    std::int64_t size = 0;
                    std::int64_t numObjects = 0;
                    std::int64_t millis = 0;

                    for (const auto& shardResult : shardResults) {
                        const auto shardResponse =
                            uassertStatusOK(std::move(shardResult.swResponse));
                        uassertStatusOK(shardResponse.status);

                        const auto& res = shardResponse.data;
                        uassertStatusOK(getStatusFromCommandResult(res));

                        auto parsedResponse = Reply::parse(res, IDLParserContext{"dataSize"});
                        size += parsedResponse.getSize();
                        numObjects += parsedResponse.getNumObjects();
                        millis += parsedResponse.getMillis();
                    }

                    Reply reply;
                    reply.setSize(size);
                    reply.setNumObjects(numObjects);
                    reply.setMillis(millis);
                    return reply;
                });
        }
    };

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return AllowedOnSecondary::kAlways;
    }

    bool adminOnly() const final {
        return false;
    }
};
MONGO_REGISTER_COMMAND(DataSizeCmd).forRouter();

}  // namespace
}  // namespace mongo
