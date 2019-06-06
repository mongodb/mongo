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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/create_collection_gen.h"

namespace mongo {
namespace {

class CreateCmd : public BasicCommand {
public:
    CreateCmd() : BasicCommand("create") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return false;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    std::string parseNs(const std::string& dbname, const BSONObj& cmdObj) const override {
        return CommandHelpers::parseNsCollectionRequired(dbname, cmdObj).ns();
    }

    Status checkAuthForCommand(Client* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) const override {
        const NamespaceString nss(parseNs(dbname, cmdObj));
        return AuthorizationSession::get(client)->checkAuthForCreate(nss, cmdObj, true);
    }

    bool run(OperationContext* opCtx,
             const std::string& dbName,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        const NamespaceString nss(parseNs(dbName, cmdObj));

        createShardDatabase(opCtx, dbName);

        uassert(ErrorCodes::InvalidOptions,
                "specify size:<n> when capped is true",
                !cmdObj["capped"].trueValue() || cmdObj["size"].isNumber() ||
                    cmdObj.hasField("$nExtents"));

        ConfigsvrCreateCollection configCreateCmd(nss);
        configCreateCmd.setDbName(NamespaceString::kAdminDb);

        {
            BSONObjIterator cmdIter(cmdObj);
            invariant(cmdIter.more());  // At least the command namespace should be present
            cmdIter.next();
            BSONObjBuilder optionsBuilder;
            CommandHelpers::filterCommandRequestForPassthrough(&cmdIter, &optionsBuilder);
            configCreateCmd.setOptions(optionsBuilder.obj());
        }

        const auto shardRegistry = Grid::get(opCtx)->shardRegistry();
        auto response = shardRegistry->getConfigShard()->runCommandWithFixedRetryAttempts(
            opCtx,
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            "admin",
            CommandHelpers::appendMajorityWriteConcern(
                CommandHelpers::appendPassthroughFields(cmdObj, configCreateCmd.toBSON({}))),
            Shard::RetryPolicy::kIdempotent);

        uassertStatusOK(Shard::CommandResponse::getEffectiveStatus(response));
        return true;
    }

} createCmd;

}  // namespace
}  // namespace mongo
