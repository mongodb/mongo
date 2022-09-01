/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/grid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace {

class SplitVectorCmd : public BasicCommand {
public:
    SplitVectorCmd() : BasicCommand("splitVector") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    NamespaceString parseNs(const DatabaseName& dbName, const BSONObj& cmdObj) const override {
        return NamespaceString(dbName.tenantId(), CommandHelpers::parseNsFullyQualified(cmdObj));
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    Status checkAuthForCommand(Client* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) const override {
        if (!AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
                ResourcePattern::forExactNamespace(parseNs({boost::none, dbname}, cmdObj)),
                ActionType::splitVector)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }
        return Status::OK();
    }

    bool run(OperationContext* opCtx,
             const DatabaseName& dbName,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        const NamespaceString nss(parseNs(dbName, cmdObj));
        uassert(ErrorCodes::IllegalOperation,
                "Performing splitVector across dbs isn't supported via mongos",
                nss.dbName() == dbName);

        const auto cm =
            uassertStatusOK(Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(opCtx, nss));
        uassert(ErrorCodes::IllegalOperation,
                str::stream() << "can't do command: " << getName() << " on sharded collection",
                !cm.isSharded());

        // Here, we first filter the command before appending an UNSHARDED shardVersion, because
        // "shardVersion" is one of the fields that gets filtered out.
        BSONObj filteredCmdObj(applyReadWriteConcern(
            opCtx, this, CommandHelpers::filterCommandRequestForPassthrough(cmdObj)));
        BSONObj filteredCmdObjWithVersion(
            appendShardVersion(filteredCmdObj, ShardVersion::UNSHARDED()));

        auto shard =
            uassertStatusOK(Grid::get(opCtx)->shardRegistry()->getShard(opCtx, cm.dbPrimary()));
        auto commandResponse = uassertStatusOK(shard->runCommandWithFixedRetryAttempts(
            opCtx,
            ReadPreferenceSetting::get(opCtx),
            dbName.toStringWithTenantId(),
            cm.dbPrimary() == ShardId::kConfigServerId ? filteredCmdObj : filteredCmdObjWithVersion,
            Shard::RetryPolicy::kIdempotent));

        uassert(ErrorCodes::IllegalOperation,
                str::stream() << "can't do command: " << getName() << " on a sharded collection",
                !ErrorCodes::isStaleShardVersionError(commandResponse.commandStatus.code()));

        uassertStatusOK(commandResponse.commandStatus);

        if (!commandResponse.writeConcernStatus.isOK()) {
            appendWriteConcernErrorToCmdResponse(
                cm.dbPrimary(), commandResponse.response["writeConcernError"], result);
        }
        result.appendElementsUnique(
            CommandHelpers::filterCommandReplyForPassthrough(std::move(commandResponse.response)));

        return true;
    }

} splitVectorCmd;


}  // namespace
}  // namespace mongo
