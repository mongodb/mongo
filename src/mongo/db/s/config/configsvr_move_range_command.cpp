// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/base/error_codes.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/s/balancer/balancer.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/s/request_types/move_range_request_gen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <memory>
#include <string>

#include <boost/move/utility_core.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace {

class ConfigSvrMoveRangeCommand final : public TypedCommand<ConfigSvrMoveRangeCommand> {
public:
    using Request = ConfigsvrMoveRange;

    std::string help() const override {
        return "Internal command only invokable on the config server. Do not call directly. "
               "Requests the balancer to move a range.";
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return true;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            uassert(ErrorCodes::IllegalOperation,
                    str::stream() << Request::kCommandName
                                  << " can only be run on the config server",
                    serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer));

            opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

            const auto nss = ns();
            auto& req = request();

            // Set read concern level to local for reads into the config database
            repl::ReadConcernArgs::get(opCtx) =
                repl::ReadConcernArgs(repl::ReadConcernLevel::kLocalReadConcern);

            const auto resolvedToShard =
                uassertStatusOKWithContext(Grid::get(opCtx)->shardRegistry()->resolveShardId(
                                               opCtx,
                                               req.getMoveRangeRequestBase().getToShard(),
                                               true /* allowNonShardIdIdentifiers */),
                                           "Could not find destination shard");

            // Ensure we forward to the balancer the resolved shard id, not the original identifier.
            req.setToShard(resolvedToShard);

            try {
                Balancer::get(opCtx)->moveRange(opCtx, nss, req, true /* issuedByRemoteUser */);
            } catch (ExceptionFor<ErrorCodes::InterruptedDueToReplStateChange>& ex) {
                // Rewrite `InterruptedDueToReplStateChange` with `RetriableRemoteCommandFailure` to
                // ensure it is not forwarded to remote callers. Until we can expose the error
                // origins, we should rewrite this error to ensure this failure doesn't involve the
                // RSM. TODO SERVER-91633: remove this rewriting once error origins are known.
                Status error(ErrorCodes::RetriableRemoteCommandFailure,
                             "Encountered a replication event");
                error.addContext(ex.toString());
                iasserted(error);
            }
        }

    private:
        NamespaceString ns() const override {
            return request().getCommandParameter();
        }

        bool supportsWriteConcern() const override {
            return true;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(
                            ResourcePattern::forClusterResource(request().getDbName().tenantId()),
                            ActionType::internal));
        }
    };
};
MONGO_REGISTER_COMMAND(ConfigSvrMoveRangeCommand).forShard();

}  // namespace
}  // namespace mongo
