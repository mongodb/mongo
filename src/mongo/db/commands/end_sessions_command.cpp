/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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
