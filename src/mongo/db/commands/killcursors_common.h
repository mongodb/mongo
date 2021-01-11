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

#include "mongo/db/audit.h"
#include "mongo/db/client.h"
#include "mongo/db/cursor_id.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/kill_cursors_gen.h"
#include "mongo/db/read_concern_support_result.h"

namespace mongo {

/**
 * Base class for the killCursors command, which attempts to kill all given cursors.  Contains code
 * common to mongos and mongod implementations.
 */
template <typename Impl>
class KillCursorsCmdBase : public KillCursorsCmdVersion1Gen<KillCursorsCmdBase<Impl>> {
public:
    using KCV1Gen = KillCursorsCmdVersion1Gen<KillCursorsCmdBase<Impl>>;

    BasicCommand::AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return BasicCommand::AllowedOnSecondary::kAlways;
    }

    bool maintenanceOk() const final {
        return false;
    }

    bool adminOnly() const final {
        return false;
    }

    std::string help() const final {
        return "Kill a list of cursor ids";
    }

    bool shouldAffectCommandCounter() const final {
        return true;
    }

    class Invocation : public KCV1Gen::InvocationBaseGen {
    public:
        using KCV1Gen::InvocationBaseGen::InvocationBaseGen;

        bool supportsWriteConcern() const final {
            return false;
        }

        ReadConcernSupportResult supportsReadConcern(repl::ReadConcernLevel level) const final {
            if constexpr (Impl::supportsReadConcern) {
                return ReadConcernSupportResult::allSupportedAndDefaultPermitted();
            } else {
                return KCV1Gen::InvocationBaseGen::supportsReadConcern(level);
            }
        }

        NamespaceString ns() const final {
            return this->request().getNamespace();
        }

        void doCheckAuthorization(OperationContext* opCtx) const final {
            auto killCursorsRequest = this->request();

            const auto& nss = killCursorsRequest.getNamespace();
            for (CursorId id : killCursorsRequest.getCursorIds()) {
                auto status = Impl::doCheckAuth(opCtx, nss, id);
                if (!status.isOK()) {
                    if (status.code() == ErrorCodes::CursorNotFound) {
                        // Not found isn't an authorization issue.
                        // run() will raise it as a return value.
                        continue;
                    }
                    audit::logKillCursorsAuthzCheck(opCtx->getClient(), nss, id, status.code());
                    uassertStatusOK(status);  // throws
                }
            }
        }

        KillCursorsReply typedRun(OperationContext* opCtx) final {
            auto killCursorsRequest = this->request();

            std::vector<CursorId> cursorsKilled;
            std::vector<CursorId> cursorsNotFound;
            std::vector<CursorId> cursorsAlive;

            for (CursorId id : killCursorsRequest.getCursorIds()) {
                auto status = Impl::doKillCursor(opCtx, killCursorsRequest.getNamespace(), id);
                if (status.isOK()) {
                    cursorsKilled.push_back(id);
                } else if (status.code() == ErrorCodes::CursorNotFound) {
                    cursorsNotFound.push_back(id);
                } else {
                    cursorsAlive.push_back(id);
                }

                audit::logKillCursorsAuthzCheck(
                    opCtx->getClient(), killCursorsRequest.getNamespace(), id, status.code());
            }

            KillCursorsReply reply;
            reply.setCursorsKilled(std::move(cursorsKilled));
            reply.setCursorsNotFound(std::move(cursorsNotFound));
            reply.setCursorsAlive(std::move(cursorsAlive));
            reply.setCursorsUnknown({});
            return reply;
        }
    };
};

}  // namespace mongo
