// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/legacy_runtime_constants_gen.h"
#include "mongo/db/query/analyze_command_gen.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/db/router_role/cluster_commands_helpers.h"
#include "mongo/db/router_role/router_role.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/str.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>

namespace mongo {
namespace {

class ClusterAnalyzeCmd final : public TypedCommand<ClusterAnalyzeCmd> {
public:
    using Request = AnalyzeCommandRequest;

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    std::string help() const override {
        return "Command to generate statistics for a collection for use in the optimizer.";
    }

    ReadWriteType getReadWriteType() const override {
        return ReadWriteType::kWrite;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        bool supportsWriteConcern() const final {
            return true;
        }

        NamespaceString ns() const final {
            return request().getNamespace();
        }

        void typedRun(OperationContext* opCtx) {
            const NamespaceString& nss = ns();

            auto& cmd = request();
            setReadWriteConcern(opCtx, cmd, this);

            sharding::router::CollectionRouter router(opCtx, nss);
            router.routeWithRoutingContext(
                Request::kCommandName, [&](OperationContext* opCtx, RoutingContext& routingCtx) {
                    auto shardResponses = scatterGatherVersionedTargetByRoutingTable(
                        opCtx,
                        routingCtx,
                        nss,
                        CommandHelpers::filterCommandRequestForPassthrough(cmd.toBSON()),
                        ReadPreferenceSetting::get(opCtx),
                        Shard::RetryPolicy::kIdempotent,
                        BSONObj() /*query*/,
                        BSONObj() /*collation*/,
                        boost::none /*letParameters*/,
                        boost::none /*runtimeConstants*/);

                    for (const auto& shardResult : shardResponses) {
                        const auto& shardResponse =
                            uassertStatusOK(std::move(shardResult.swResponse));

                        uassertStatusOK(shardResponse.status);
                        uassertStatusOK(getStatusFromCommandResult(shardResponse.data));
                    }
                });
        }

    private:
        void doCheckAuthorization(OperationContext* opCtx) const override {
            auto* authzSession = AuthorizationSession::get(opCtx->getClient());
            const NamespaceString& ns = request().getNamespace();

            uassert(ErrorCodes::Unauthorized,
                    str::stream() << "Not authorized to call analyze on collection "
                                  << ns.toStringForErrorMsg(),
                    authzSession->isAuthorizedForActionsOnNamespace(ns, ActionType::analyze));

            // Require find privilege to prevent analyze from being used as a proxy to read
            // documents from collections the caller cannot directly access.
            uassert(ErrorCodes::Unauthorized,
                    str::stream() << "Not authorized to read collection "
                                  << ns.toStringForErrorMsg(),
                    authzSession->isAuthorizedForActionsOnNamespace(ns, ActionType::find));
        }
    };
};
MONGO_REGISTER_COMMAND(ClusterAnalyzeCmd).forRouter().testOnly();

}  // namespace
}  // namespace mongo
