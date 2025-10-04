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
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/crypto/sha256_block.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/auth/user.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/kill_sessions.h"
#include "mongo/db/session/kill_sessions_common.h"
#include "mongo/db/session/kill_sessions_gen.h"
#include "mongo/db/session/logical_session_id_helpers.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/read_through_cache.h"

#include <string>
#include <utility>
#include <vector>

#include <absl/container/node_hash_set.h>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

namespace {

KillAllSessionsByPatternSet patternsForLoggedInUser(OperationContext* opCtx) {
    auto client = opCtx->getClient();

    KillAllSessionsByPatternSet patterns;

    if (AuthorizationManager::get(opCtx->getService())->isAuthEnabled()) {
        auto* as = AuthorizationSession::get(client);
        if (auto user = as->getAuthenticatedUser()) {
            auto item = makeKillAllSessionsByPattern(opCtx);
            item.pattern.setUid(user.value()->getDigest());
            patterns.emplace(std::move(item));
        }
    } else {
        patterns.emplace(makeKillAllSessionsByPattern(opCtx));
    }

    return patterns;
}

}  // namespace

class KillSessionsCommand final : public BasicCommand {
    KillSessionsCommand(const KillSessionsCommand&) = delete;
    KillSessionsCommand& operator=(const KillSessionsCommand&) = delete;

public:
    KillSessionsCommand() : BasicCommand("killSessions") {}

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
        return "kill a logical session and its operations";
    }

    // Any user can kill their own sessions
    Status checkAuthForOperation(OperationContext*,
                                 const DatabaseName&,
                                 const BSONObj&) const override {
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
        IDLParserContext ctx("KillSessionsCmd");
        auto ksc = KillSessionsCmdFromClient::parse(cmdObj, ctx);

        KillAllSessionsByPatternSet patterns;

        if (ksc.getKillSessions().empty()) {
            patterns = patternsForLoggedInUser(opCtx);
        } else {
            auto lsids = makeLogicalSessionIds(
                ksc.getKillSessions(),
                opCtx,
                {Privilege{ResourcePattern::forClusterResource(dbName.tenantId()),
                           ActionType::killAnySession}});

            patterns.reserve(lsids.size());
            for (const auto& lsid : lsids) {
                patterns.emplace(makeKillAllSessionsByPattern(opCtx, lsid));
            }
        }

        uassertStatusOK(killSessionsCmdHelper(opCtx, patterns));
        killSessionsReport(opCtx, cmdObj);
        return true;
    }
};
MONGO_REGISTER_COMMAND(KillSessionsCommand).forRouter().forShard();

}  // namespace mongo
