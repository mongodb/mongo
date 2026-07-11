// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/router_role/cluster_commands_helpers.h"
#include "mongo/db/router_role/router_role.h"
#include "mongo/db/service_context.h"
#include "mongo/s/request_types/auto_split_vector_gen.h"

#include <memory>
#include <string>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {

namespace {

class ClusterAutoSplitVectorCommand final : public TypedCommand<ClusterAutoSplitVectorCommand> {
public:
    using Request = AutoSplitVectorRequest;

    std::string help() const override {
        return "Returns the split points for a chunk, given a range and the maximum chunk size.";
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        AutoSplitVectorResponse typedRun(OperationContext* opCtx) {
            const auto nss = ns();
            const auto& req = request();

            sharding::router::CollectionRouter router(opCtx, nss);
            return router.routeWithRoutingContext(
                Request::kCommandName, [&](OperationContext* opCtx, RoutingContext& routingCtx) {
                    BSONObj filteredCmdObj =
                        CommandHelpers::filterCommandRequestForPassthrough(req.toBSON());

                    // autoSplitVector is allowed to run on a sharded cluster only if the range
                    // requested belongs to one shard. We target the shard owning the input min
                    // chunk and we let the targetted shard figure whether the range is fully owned
                    // by itself. In case the constraint is not respected we will get a
                    // InvalidOptions as part of the response.
                    auto response = scatterGatherVersionedTargetByRoutingTable(
                                        opCtx,
                                        routingCtx,
                                        nss,
                                        filteredCmdObj,
                                        ReadPreferenceSetting::get(opCtx),
                                        Shard::RetryPolicy::kIdempotent,
                                        req.getMin(),
                                        {} /*collation*/,
                                        boost::none /*letParameters*/,
                                        boost::none /*runtimeConstants*/)
                                        .front();

                    auto status = AsyncRequestsSender::Response::getEffectiveStatus(response);
                    uassertStatusOK(status);
                    return AutoSplitVectorResponse::parse(response.swResponse.getValue().data,
                                                          IDLParserContext(""));
                });
        }

    private:
        NamespaceString ns() const override {
            return request().getNamespace();
        }

        bool supportsWriteConcern() const override {
            return false;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(
                            ResourcePattern::forClusterResource(request().getDbName().tenantId()),
                            ActionType::splitVector));
        }
    };
};

MONGO_REGISTER_COMMAND(ClusterAutoSplitVectorCommand).forRouter();

}  // namespace
}  // namespace mongo
