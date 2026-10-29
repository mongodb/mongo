// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/update_metrics_filtering_allowlist_command_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"

#include <string>

namespace mongo {

class CmdUpdateMetricsFilteringAllowlist final
    : public TypedCommand<CmdUpdateMetricsFilteringAllowlist> {
public:
    using Request = UpdateMetricsFilteringAllowlist;
    using Response = UpdateMetricsFilteringAllowlistReply;

    std::string help() const override {
        return "Command to update the metrics filtering allowlist for a given category";
    }

    bool adminOnly() const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool requiresAuthzChecks() const override {
        return true;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        NamespaceString ns() const override {
            return NamespaceString::kEmpty;
        }

        bool supportsWriteConcern() const override {
            return false;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            auto* authSession = AuthorizationSession::get(opCtx->getClient());
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    authSession->isAuthorizedForActionsOnResource(
                        ResourcePattern::forClusterResource(authSession->getUserTenantId()),
                        ActionType::manageMetricsFiltering));
        }

        Response typedRun(OperationContext* opCtx) {
            uassert(ErrorCodes::BadValue,
                    "Expected to add at least one metric path to the allowlist",
                    !request().getAdd().empty());
            return {};
        }
    };
};

MONGO_REGISTER_COMMAND(CmdUpdateMetricsFilteringAllowlist).forShard().forRouter();

}  // namespace mongo
