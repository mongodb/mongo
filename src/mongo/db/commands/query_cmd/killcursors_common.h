// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/audit.h"
#include "mongo/db/client.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/client_cursor/cursor_id.h"
#include "mongo/db/query/client_cursor/kill_cursors_gen.h"
#include "mongo/db/read_concern_support_result.h"
#include "mongo/util/modules.h"

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

    bool allowedInTransactions() const final {
        return true;
    }

    bool requiresAuthzChecks() const override {
        return false;
    }

    class Invocation : public KCV1Gen::InvocationBaseGen {
    public:
        using KCV1Gen::InvocationBaseGen::InvocationBaseGen;

        bool supportsWriteConcern() const final {
            return false;
        }

        ReadConcernSupportResult supportsReadConcern(repl::ReadConcernLevel level,
                                                     bool isImplicitDefault) const final {
            if constexpr (Impl::supportsReadConcern) {
                return ReadConcernSupportResult::allSupportedAndDefaultPermitted();
            } else {
                return KCV1Gen::InvocationBaseGen::supportsReadConcern(level, isImplicitDefault);
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
                audit::logKillCursorsAuthzCheck(opCtx->getClient(), nss, id, status.code());
                if (!status.isOK()) {
                    if (status.code() == ErrorCodes::CursorNotFound) {
                        // Not found isn't an authorization issue.
                        // run() will raise it as a return value.
                        continue;
                    }
                    uassertStatusOK(status);  // throws
                }
            }
        }

        KillCursorsCommandReply typedRun(OperationContext* opCtx) final {
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
            }

            KillCursorsCommandReply reply;
            reply.setCursorsKilled(std::move(cursorsKilled));
            reply.setCursorsNotFound(std::move(cursorsNotFound));
            reply.setCursorsAlive(std::move(cursorsAlive));
            reply.setCursorsUnknown({});
            return reply;
        }
    };
};

}  // namespace mongo
