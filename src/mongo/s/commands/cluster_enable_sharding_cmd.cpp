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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/field_parser.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/util/scopeguard.h"

namespace mongo {
namespace {

class EnableShardingCmd : public ErrmsgCommandDeprecated {
public:
    EnableShardingCmd() : ErrmsgCommandDeprecated("enableSharding", "enablesharding") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool adminOnly() const override {
        return true;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }
    static constexpr StringData kShardNameField = "primaryShard"_sd;

    std::string help() const override {
        return "Enable sharding for a database. Optionally allows the caller to specify the shard "
               "to be used as primary."
               "(Use 'shardcollection' command afterwards.)\n"
               "  { enableSharding : \"<dbname>\", primaryShard:  \"<shard>\"}\n";
    }

    Status checkAuthForCommand(Client* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) const override {
        if (!AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
                ResourcePattern::forDatabaseName(parseNs(dbname, cmdObj)),
                ActionType::enableSharding)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }

        return Status::OK();
    }

    std::string parseNs(const std::string& dbname_unused, const BSONObj& cmdObj) const override {
        return cmdObj.firstElement().str();
    }

    bool errmsgRun(OperationContext* opCtx,
                   const std::string& dbname_unused,
                   const BSONObj& cmdObj,
                   std::string& errmsg,
                   BSONObjBuilder& result) override {

        const std::string db = parseNs("", cmdObj);

        auto shardElem = cmdObj[kShardNameField];
        std::string shardId = shardElem.ok() ? shardElem.String() : "";

        // Invalidate the routing table cache entry for this database so that we reload the
        // collection the next time it's accessed, even if we receive a failure, e.g. NetworkError.
        auto guard =
            makeGuard([opCtx, db] { Grid::get(opCtx)->catalogCache()->purgeDatabase(db); });


        BSONObjBuilder remoteCmdObj;
        remoteCmdObj.append("_configsvrEnableSharding", db);
        if (shardElem.ok()) {
            remoteCmdObj.append(kShardNameField, shardId);
        }

        auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();
        auto cmdResponse = uassertStatusOK(configShard->runCommandWithFixedRetryAttempts(
            opCtx,
            ReadPreferenceSetting(ReadPreference::PrimaryOnly),
            "admin",
            CommandHelpers::appendMajorityWriteConcern(
                CommandHelpers::appendPassthroughFields(cmdObj, remoteCmdObj.obj()),
                opCtx->getWriteConcern()),
            Shard::RetryPolicy::kIdempotent));

        CommandHelpers::filterCommandReplyForPassthrough(cmdResponse.response, &result);
        return true;
    }

} enableShardingCmd;

}  // namespace
}  // namespace mongo
