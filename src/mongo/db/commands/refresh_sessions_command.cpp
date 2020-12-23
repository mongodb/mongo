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

#include "mongo/platform/basic.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/sessions_commands_gen.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/logical_session_cache.h"
#include "mongo/db/logical_session_id_helpers.h"
#include "mongo/db/operation_context.h"

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
            uassert(ErrorCodes::Unauthorized,
                    "Not authorized to run refreshSessions command",
                    AuthorizationSession::get(opCtx->getClient())->getSingleUser());
        }

        Reply typedRun(OperationContext* opCtx) final {
            const auto lsCache = LogicalSessionCache::get(opCtx);

            for (const auto& lsid : makeLogicalSessionIds(request().getCommandParameter(), opCtx)) {
                uassertStatusOK(lsCache->vivify(opCtx, lsid));
            }

            return Reply();
        }
    };

} refreshSessionsCommand;

}  // namespace
}  // namespace mongo
