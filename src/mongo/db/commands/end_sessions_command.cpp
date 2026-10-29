// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/db/commands.h"
#include "mongo/db/commands/sessions_commands_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_cache.h"
#include "mongo/db/session/logical_session_id_helpers.h"
#include "mongo/rpc/op_msg.h"

#include <memory>
#include <set>
#include <string>

namespace mongo {
namespace {

class EndSessionsCommand final : public EndSessionsCmdVersion1Gen<EndSessionsCommand> {
    EndSessionsCommand(const EndSessionsCommand&) = delete;
    EndSessionsCommand& operator=(const EndSessionsCommand&) = delete;

public:
    EndSessionsCommand() = default;

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return AllowedOnSecondary::kAlways;
    }

    bool adminOnly() const final {
        return false;
    }

    std::string help() const final {
        return "end a set of logical sessions";
    }

    // We should allow users to end sessions even if the user does not have the direct shard roles
    // action type.
    bool shouldSkipDirectConnectionChecks() const final {
        return true;
    }

    /**
     * Drivers may implicitly call {endSessions:...} for unauthenticated clients.
     * Don't bother auditing when this happens.
     */
    bool auditAuthorizationFailure() const final {
        return false;
    }

    bool requiresAuthzChecks() const override {
        return false;
    }
    class Invocation final : public InvocationBaseGen {
    public:
        using InvocationBaseGen::InvocationBaseGen;

        bool supportsWriteConcern() const final {
            return false;
        }

        NamespaceString ns() const final {
            return NamespaceString(request().getDbName());
        }

        void doCheckAuthorization(OperationContext* opCtx) const final {
            // It is always ok to run this command, as long as you are authenticated
            // as some user, if auth is enabled.
            // requiresAuth() => true covers this for us.
        }

        Reply typedRun(OperationContext* opCtx) final {
            LogicalSessionCache::get(opCtx)->endSessions(
                makeLogicalSessionIds(request().getCommandParameter(), opCtx));

            return Reply();
        }
    };
};
MONGO_REGISTER_COMMAND(EndSessionsCommand).forRouter().forShard();

}  // namespace
}  // namespace mongo
