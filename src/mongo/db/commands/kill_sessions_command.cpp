// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/status.h"
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

    bool requiresAuthzChecks() const override {
        return false;
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
