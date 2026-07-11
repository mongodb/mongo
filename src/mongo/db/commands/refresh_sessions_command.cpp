// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/commands.h"
#include "mongo/db/commands/sessions_commands_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_cache.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/logical_session_id_helpers.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <set>
#include <string>

#include <absl/container/node_hash_set.h>

namespace mongo {
namespace {

class RefreshSessionsCommand final : public RefreshSessionsCmdVersion1Gen<RefreshSessionsCommand> {
public:
    RefreshSessionsCommand() = default;

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return AllowedOnSecondary::kAlways;
    }

    bool adminOnly() const final {
        return false;
    }

    std::string help() const final {
        return "renew a set of logical sessions";
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
            const auto lsCache = LogicalSessionCache::get(opCtx);

            for (const auto& lsid : makeLogicalSessionIds(request().getCommandParameter(), opCtx)) {
                uassertStatusOK(lsCache->vivify(opCtx, lsid));
            }

            return Reply();
        }
    };
};
MONGO_REGISTER_COMMAND(RefreshSessionsCommand).forRouter().forShard();

}  // namespace
}  // namespace mongo
