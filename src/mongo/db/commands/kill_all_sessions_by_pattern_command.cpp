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

#include "mongo/base/error_codes.h"
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/api_parameters.h"
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
#include "mongo/util/decorable.h"

#include <string>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

class KillAllSessionsByPatternCommand final : public BasicCommand {
    KillAllSessionsByPatternCommand(const KillAllSessionsByPatternCommand&) = delete;
    KillAllSessionsByPatternCommand& operator=(const KillAllSessionsByPatternCommand&) = delete;

public:
    KillAllSessionsByPatternCommand() : BasicCommand("killAllSessionsByPattern") {}

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
        return "kill logical sessions by pattern";
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
             const DatabaseName& dbName,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        IDLParserContext ctx("KillAllSessionsByPatternCmd");
        auto ksc = KillAllSessionsByPatternCmd::parse(cmdObj, ctx);

        // The empty command kills all
        if (ksc.getKillAllSessionsByPattern().empty()) {
            auto item = makeKillAllSessionsByPattern(opCtx);
            std::vector<mongo::KillAllSessionsByPattern> patterns;
            patterns.push_back({std::move(item.pattern)});
            ksc.setKillAllSessionsByPattern(std::move(patterns));
        } else {
            // If a pattern is passed, you may only pass impersonate data if you have the
            // impersonate privilege.
            auto authSession = AuthorizationSession::get(opCtx->getClient());

            if (!authSession->isAuthorizedForPrivilege(
                    Privilege(ResourcePattern::forClusterResource(dbName.tenantId()),
                              ActionType::impersonate))) {

                for (const auto& pattern : ksc.getKillAllSessionsByPattern()) {
                    if (pattern.getUsers() || pattern.getRoles()) {
                        uasserted(ErrorCodes::Unauthorized,
                                  "Not authorized to impersonate in killAllSessionsByPattern");
                    }
                }
            }
        }

        KillAllSessionsByPatternSet patterns;
        for (auto& pattern : ksc.getKillAllSessionsByPattern()) {
            patterns.insert({std::move(pattern), APIParameters::get(opCtx)});
        }

        uassertStatusOK(killSessionsCmdHelper(opCtx, patterns));
        killSessionsReport(opCtx, cmdObj);
        return true;
    }
};
MONGO_REGISTER_COMMAND(KillAllSessionsByPatternCommand).forRouter().forShard();

}  // namespace mongo
