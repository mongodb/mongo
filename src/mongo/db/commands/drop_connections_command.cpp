// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/drop_connections_gen.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/executor/egress_connection_closer_manager.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <string>
#include <vector>

namespace mongo {
namespace {

class DropConnectionsCmd final : public TypedCommand<DropConnectionsCmd> {
public:
    using Request = DropConnections;

    std::string help() const override {
        return "Drop egress connections to specified host and port";
    }

    bool adminOnly() const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {

            const auto& hostAndPorts = request().getHostAndPort();

            auto& egressConnectionCloserManager =
                executor::EgressConnectionCloserManager::get(opCtx->getServiceContext());

            for (const auto& hostAndPort : hostAndPorts) {
                egressConnectionCloserManager.dropConnections(
                    hostAndPort,
                    Status(ErrorCodes::PooledConnectionsDropped,
                           "Dropping egress connections to specific target due to the "
                           "dropConnections command"));
            }
        }

        NamespaceString ns() const override {
            return NamespaceString::kEmpty;
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
                            ActionType::dropConnections));
        }
    };
};
MONGO_REGISTER_COMMAND(DropConnectionsCmd).forRouter().forShard();

}  // namespace
}  // namespace mongo
