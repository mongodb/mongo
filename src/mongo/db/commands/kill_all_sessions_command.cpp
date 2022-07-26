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

#include "mongo/base/init.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/kill_sessions.h"
#include "mongo/db/kill_sessions_common.h"
#include "mongo/db/kill_sessions_local.h"
#include "mongo/db/logical_session_cache.h"
#include "mongo/db/logical_session_id.h"
#include "mongo/db/logical_session_id_helpers.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/stats/top.h"

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
                                 const std::string& dbname,
                                 const BSONObj& cmdObj) const override {
        AuthorizationSession* authSession = AuthorizationSession::get(opCtx->getClient());
        if (!authSession->isAuthorizedForPrivilege(
                Privilege{ResourcePattern::forClusterResource(), ActionType::killAnySession})) {
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

    virtual bool run(OperationContext* opCtx,
                     const std::string& db,
                     const BSONObj& cmdObj,
                     BSONObjBuilder& result) override {
        IDLParserContext ctx("KillAllSessionsCmd");
        auto ksc = KillAllSessionsCmd::parse(ctx, cmdObj);

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

        uassertStatusOK(killSessionsCmdHelper(opCtx, result, patterns));
        killSessionsReport(opCtx, cmdObj);
        return true;
    }
} killAllSessionsCommand;

}  // namespace mongo
