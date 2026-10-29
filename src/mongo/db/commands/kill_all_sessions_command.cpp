// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/kill_sessions.h"
#include "mongo/db/session/kill_sessions_common.h"
#include "mongo/db/session/kill_sessions_gen.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/assert_util.h"

#include <string>
#include <vector>

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>

namespace mongo {

class KillAllSessionsCommand final : public BasicCommand {
    KillAllSessionsCommand(const KillAllSessionsCommand&) = delete;
    KillAllSessionsCommand& operator=(const KillAllSessionsCommand&) = delete;

public:
    KillAllSessionsCommand() : BasicCommand("killAllSessions") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }
    bool adminOnly() const override {
        return false;
    }
    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }
    std::string help() const override {
        return "kill all logical sessions, for a user, and their operations";
    }
    Status checkAuthForOperation(OperationContext* opCtx,
                                 const DatabaseName& dbName,
                                 const BSONObj&) const override {
        AuthorizationSession* authSession = AuthorizationSession::get(opCtx->getClient());
        if (!authSession->isAuthorizedForPrivilege(
                Privilege{ResourcePattern::forClusterResource(dbName.tenantId()),
                          ActionType::killAnySession})) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }

        return Status::OK();
    }

    /**
     * Should ignore the lsid attached to this command in order to prevent it from killing itself.
     */
    bool attachLogicalSessionsToOpCtx() const override {
        return false;
    }

    bool run(OperationContext* opCtx,
             const DatabaseName&,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        IDLParserContext ctx("KillAllSessionsCmd");
        auto ksc = KillAllSessionsCmd::parse(cmdObj, ctx);

        KillAllSessionsByPatternSet patterns;

        // The empty command kills all
        if (ksc.getKillAllSessions().empty()) {
            patterns.emplace(makeKillAllSessionsByPattern(opCtx));
        } else {
            patterns.reserve(ksc.getKillAllSessions().size());

            for (const auto& user : ksc.getKillAllSessions()) {
                patterns.emplace(makeKillAllSessionsByPattern(opCtx, user));
            }
        }

        uassertStatusOK(killSessionsCmdHelper(opCtx, patterns));
        killSessionsReport(opCtx, cmdObj);
        return true;
    }
};
MONGO_REGISTER_COMMAND(KillAllSessionsCommand).forRouter().forShard();

}  // namespace mongo
