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
#include "mongo/db/auth/privilege.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/rename_collection_common.h"
#include "mongo/db/commands/rename_collection_gen.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/grid.h"

namespace mongo {
namespace {

bool nonShardedCollectionCommandPassthrough(OperationContext* opCtx,
                                            StringData dbName,
                                            const NamespaceString& nss,
                                            const ChunkManager& cm,
                                            const BSONObj& cmdObj,
                                            Shard::RetryPolicy retryPolicy,
                                            BSONObjBuilder* out) {
    const StringData cmdName(cmdObj.firstElementFieldName());
    uassert(ErrorCodes::IllegalOperation,
            str::stream() << "Can't do command: " << cmdName << " on a sharded collection",
            !cm.isSharded());

    auto responses = scatterGatherVersionedTargetByRoutingTable(
        opCtx, dbName, nss, cm, cmdObj, ReadPreferenceSetting::get(opCtx), retryPolicy, {}, {});
    invariant(responses.size() == 1);

    const auto cmdResponse = uassertStatusOK(std::move(responses.front().swResponse));
    const auto status = getStatusFromCommandResult(cmdResponse.data);

    uassert(ErrorCodes::IllegalOperation,
            str::stream() << "Can't do command: " << cmdName << " on a sharded collection",
            !ErrorCodes::isStaleShardVersionError(status));

    out->appendElementsUnique(CommandHelpers::filterCommandReplyForPassthrough(cmdResponse.data));
    return status.isOK();
}

class RenameCollectionCmd : public BasicCommand {
public:
    RenameCollectionCmd() : BasicCommand("renameCollection") {}

    std::string parseNs(const std::string& dbname, const BSONObj& cmdObj) const override {
        return CommandHelpers::parseNsFullyQualified(cmdObj);
    }

    bool adminOnly() const override {
        return true;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    Status checkAuthForCommand(Client* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) const override {
        return rename_collection::checkAuthForRenameCollectionCommand(client, dbname, cmdObj);
    }

    bool run(OperationContext* opCtx,
             const std::string& dbName,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        auto renameRequest =
            RenameCollectionCommand::parse(IDLParserErrorContext("renameCollection"), cmdObj);
        auto fromNss = renameRequest.getCommandParameter();
        auto toNss = renameRequest.getTo();

        uassert(ErrorCodes::InvalidNamespace,
                str::stream() << "Invalid target namespace: " << toNss.ns(),
                toNss.isValid());

        const auto fromCM = uassertStatusOK(
            Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(opCtx, fromNss));
        uassert(13138, "You can't rename a sharded collection", !fromCM.isSharded());

        const auto toCM = uassertStatusOK(
            Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(opCtx, toNss));
        uassert(13139, "You can't rename to a sharded collection", !toCM.isSharded());

        uassert(13137,
                "Source and destination collections must be on same shard",
                fromCM.dbPrimary() == toCM.dbPrimary());

        return nonShardedCollectionCommandPassthrough(
            opCtx,
            NamespaceString::kAdminDb,
            fromNss,
            fromCM,
            applyReadWriteConcern(
                opCtx, this, CommandHelpers::filterCommandRequestForPassthrough(cmdObj)),
            Shard::RetryPolicy::kNoRetry,
            &result);
    }

} renameCollectionCmd;

class ConvertToCappedCmd : public BasicCommand {
public:
    ConvertToCappedCmd() : BasicCommand("convertToCapped") {}

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    std::string parseNs(const std::string& dbname, const BSONObj& cmdObj) const override {
        return CommandHelpers::parseNsCollectionRequired(dbname, cmdObj).ns();
    }

    void addRequiredPrivileges(const std::string& dbname,
                               const BSONObj& cmdObj,
                               std::vector<Privilege>* out) const override {
        ActionSet actions;
        actions.addAction(ActionType::convertToCapped);
        out->push_back(Privilege(parseResourcePattern(dbname, cmdObj), actions));
    }

    bool run(OperationContext* opCtx,
             const std::string& dbName,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        const NamespaceString nss(parseNs(dbName, cmdObj));
        const auto cm =
            uassertStatusOK(Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(opCtx, nss));
        uassert(ErrorCodes::IllegalOperation,
                "You can't convertToCapped a sharded collection",
                !cm.isSharded());

        // convertToCapped creates a temp collection and renames it at the end. It will require
        // special handling for create collection.
        return nonShardedCollectionCommandPassthrough(
            opCtx,
            dbName,
            nss,
            cm,
            applyReadWriteConcern(
                opCtx, this, CommandHelpers::filterCommandRequestForPassthrough(cmdObj)),
            Shard::RetryPolicy::kIdempotent,
            &result);
    }

} convertToCappedCmd;

class SplitVectorCmd : public BasicCommand {
public:
    SplitVectorCmd() : BasicCommand("splitVector") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    std::string parseNs(const std::string& dbname, const BSONObj& cmdObj) const override {
        return CommandHelpers::parseNsFullyQualified(cmdObj);
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    Status checkAuthForCommand(Client* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) const override {
        if (!AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
                ResourcePattern::forExactNamespace(NamespaceString(parseNs(dbname, cmdObj))),
                ActionType::splitVector)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }
        return Status::OK();
    }

    bool run(OperationContext* opCtx,
             const std::string& dbName,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        const NamespaceString nss(parseNs(dbName, cmdObj));
        uassert(ErrorCodes::IllegalOperation,
                "Performing splitVector across dbs isn't supported via mongos",
                nss.db() == dbName);

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
            appendShardVersion(filteredCmdObj, ChunkVersion::UNSHARDED()));

        auto shard =
            uassertStatusOK(Grid::get(opCtx)->shardRegistry()->getShard(opCtx, cm.dbPrimary()));
        auto commandResponse = uassertStatusOK(shard->runCommandWithFixedRetryAttempts(
            opCtx,
            ReadPreferenceSetting::get(opCtx),
            dbName,
            cm.dbPrimary() == ShardRegistry::kConfigServerShardId ? filteredCmdObj
                                                                  : filteredCmdObjWithVersion,
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
