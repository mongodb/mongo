/**
 *    Copyright (C) 2018 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/bson/bsonobj_comparator.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/client/connpool.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/copydb.h"
#include "mongo/db/commands/rename_collection.h"
#include "mongo/db/operation_context.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/s/client/shard_connection.h"
#include "mongo/s/commands/cluster_commands_helpers.h"
#include "mongo/s/commands/cluster_explain.h"
#include "mongo/s/grid.h"
#include "mongo/s/query/store_possible_cursor.h"
#include "mongo/util/log.h"
#include "mongo/util/timer.h"

namespace mongo {
namespace {

bool cursorCommandPassthrough(OperationContext* opCtx,
                              StringData dbName,
                              const ShardId& shardId,
                              const BSONObj& cmdObj,
                              const NamespaceString& nss,
                              BSONObjBuilder* out) {
    const auto shardStatus = Grid::get(opCtx)->shardRegistry()->getShard(opCtx, shardId);
    if (!shardStatus.isOK()) {
        return CommandHelpers::appendCommandStatus(*out, shardStatus.getStatus());
    }
    const auto shard = shardStatus.getValue();
    ScopedDbConnection conn(shard->getConnString());
    auto cursor = conn->query(str::stream() << dbName << ".$cmd",
                              CommandHelpers::filterCommandRequestForPassthrough(cmdObj),
                              /* nToReturn=*/-1);
    if (!cursor || !cursor->more()) {
        return CommandHelpers::appendCommandStatus(
            *out, {ErrorCodes::OperationFailed, "failed to read command response from shard"});
    }
    BSONObj response = cursor->nextSafe().getOwned();
    conn.done();
    Status status = getStatusFromCommandResult(response);
    if (ErrorCodes::StaleConfig == status) {
        uassertStatusOK(status.withContext("command failed because of stale config"));
    }
    if (!status.isOK()) {
        return CommandHelpers::appendCommandStatus(*out, status);
    }

    StatusWith<BSONObj> transformedResponse =
        storePossibleCursor(opCtx,
                            shardId,
                            HostAndPort(cursor->originalHost()),
                            response,
                            nss,
                            Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor(),
                            Grid::get(opCtx)->getCursorManager());
    if (!transformedResponse.isOK()) {
        return CommandHelpers::appendCommandStatus(*out, transformedResponse.getStatus());
    }
    CommandHelpers::filterCommandReplyForPassthrough(transformedResponse.getValue(), out);

    return true;
}

class PublicGridCommand : public BasicCommand {
protected:
    PublicGridCommand(const char* n, const char* oldname = NULL) : BasicCommand(n, oldname) {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool adminOnly() const override {
        return false;
    }

    bool passthrough(OperationContext* opCtx,
                     StringData dbName,
                     const ShardId& shardId,
                     const BSONObj& cmdObj,
                     BSONObjBuilder& result) {
        const auto shard =
            uassertStatusOK(Grid::get(opCtx)->shardRegistry()->getShard(opCtx, shardId));

        ShardConnection conn(shard->getConnString(), "");

        BSONObj res;
        bool ok = conn->runCommand(
            dbName.toString(), CommandHelpers::filterCommandRequestForPassthrough(cmdObj), res);
        conn.done();

        // First append the properly constructed writeConcernError. It will then be skipped
        // in appendElementsUnique.
        if (auto wcErrorElem = res["writeConcernError"]) {
            appendWriteConcernErrorToCmdResponse(shard->getId(), wcErrorElem, result);
        }
        result.appendElementsUnique(CommandHelpers::filterCommandReplyForPassthrough(res));
        return ok;
    }
};

class NotAllowedOnShardedCollectionCmd : public BasicCommand {
protected:
    NotAllowedOnShardedCollectionCmd(const char* n) : BasicCommand(n) {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool run(OperationContext* opCtx,
             const std::string& dbName,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        const NamespaceString nss(parseNs(dbName, cmdObj));

        const auto routingInfo =
            uassertStatusOK(Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(opCtx, nss));
        uassert(ErrorCodes::IllegalOperation,
                str::stream() << "can't do command: " << getName() << " on sharded collection",
                !routingInfo.cm());

        const auto primaryShardId = routingInfo.primaryId();
        const auto primaryShard =
            uassertStatusOK(Grid::get(opCtx)->shardRegistry()->getShard(opCtx, primaryShardId));

        // Here, we first filter the command before appending an UNSHARDED shardVersion, because
        // "shardVersion" is one of the fields that gets filtered out.
        BSONObj filteredCmdObj(CommandHelpers::filterCommandRequestForPassthrough(cmdObj));
        BSONObj filteredCmdObjWithVersion(
            appendShardVersion(filteredCmdObj, ChunkVersion::UNSHARDED()));

        auto commandResponse = uassertStatusOK(primaryShard->runCommandWithFixedRetryAttempts(
            opCtx,
            ReadPreferenceSetting::get(opCtx),
            dbName,
            primaryShard->isConfig() ? filteredCmdObj : filteredCmdObjWithVersion,
            Shard::RetryPolicy::kIdempotent));

        uassert(ErrorCodes::IllegalOperation,
                str::stream() << "can't do command: " << getName() << " on a sharded collection",
                !ErrorCodes::isStaleShardingError(commandResponse.commandStatus.code()));

        uassertStatusOK(commandResponse.commandStatus);

        if (!commandResponse.writeConcernStatus.isOK()) {
            appendWriteConcernErrorToCmdResponse(
                primaryShardId, commandResponse.response["writeConcernError"], result);
        }
        result.appendElementsUnique(
            CommandHelpers::filterCommandReplyForPassthrough(std::move(commandResponse.response)));

        return true;
    }
};

class RenameCollectionCmd : public PublicGridCommand {
public:
    RenameCollectionCmd() : PublicGridCommand("renameCollection") {}

    Status checkAuthForCommand(Client* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) const override {
        return rename_collection::checkAuthForRenameCollectionCommand(client, dbname, cmdObj);
    }

    bool adminOnly() const override {
        return true;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    bool run(OperationContext* opCtx,
             const std::string& dbName,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        const auto fullNsFromElt = cmdObj.firstElement();
        uassert(ErrorCodes::InvalidNamespace,
                "'renameCollection' must be of type String",
                fullNsFromElt.type() == BSONType::String);
        const NamespaceString fullnsFrom(fullNsFromElt.valueStringData());
        uassert(ErrorCodes::InvalidNamespace,
                str::stream() << "Invalid source namespace: " << fullnsFrom.ns(),
                fullnsFrom.isValid());

        const auto fullnsToElt = cmdObj["to"];
        uassert(ErrorCodes::InvalidNamespace,
                "'to' must be of type String",
                fullnsToElt.type() == BSONType::String);
        const NamespaceString fullnsTo(fullnsToElt.valueStringData());
        uassert(ErrorCodes::InvalidNamespace,
                str::stream() << "Invalid target namespace: " << fullnsTo.ns(),
                fullnsTo.isValid());

        const auto fromRoutingInfo = uassertStatusOK(
            Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(opCtx, fullnsFrom));
        uassert(13138, "You can't rename a sharded collection", !fromRoutingInfo.cm());

        const auto toRoutingInfo = uassertStatusOK(
            Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(opCtx, fullnsTo));
        uassert(13139, "You can't rename to a sharded collection", !toRoutingInfo.cm());

        uassert(13137,
                "Source and destination collections must be on same shard",
                fromRoutingInfo.primaryId() == toRoutingInfo.primaryId());

        return passthrough(
            opCtx, NamespaceString::kAdminDb, fromRoutingInfo.primaryId(), cmdObj, result);
    }

} renameCollectionCmd;

class CopyDBCmd : public PublicGridCommand {
public:
    CopyDBCmd() : PublicGridCommand("copydb") {}

    bool adminOnly() const override {
        return true;
    }

    Status checkAuthForCommand(Client* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) const override {
        return copydb::checkAuthForCopydbCommand(client, dbname, cmdObj);
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    bool run(OperationContext* opCtx,
             const std::string& dbName,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        const auto todbElt = cmdObj["todb"];
        uassert(ErrorCodes::InvalidNamespace,
                "'todb' must be of type String",
                todbElt.type() == BSONType::String);
        const std::string todb = todbElt.str();
        uassert(ErrorCodes::InvalidNamespace,
                "Invalid todb argument",
                NamespaceString::validDBName(todb, NamespaceString::DollarInDbNameBehavior::Allow));

        auto toDbInfo = uassertStatusOK(createShardDatabase(opCtx, todb));
        uassert(ErrorCodes::IllegalOperation,
                "Cannot copy to a sharded database",
                !toDbInfo.shardingEnabled());

        const std::string fromhost = cmdObj.getStringField("fromhost");
        if (!fromhost.empty()) {
            return passthrough(
                opCtx, NamespaceString::kAdminDb, toDbInfo.primaryId(), cmdObj, result);
        }

        const auto fromDbElt = cmdObj["fromdb"];
        uassert(ErrorCodes::InvalidNamespace,
                "'fromdb' must be of type String",
                fromDbElt.type() == BSONType::String);
        const std::string fromdb = fromDbElt.str();
        uassert(
            ErrorCodes::InvalidNamespace,
            "invalid fromdb argument",
            NamespaceString::validDBName(fromdb, NamespaceString::DollarInDbNameBehavior::Allow));

        auto fromDbInfo = uassertStatusOK(createShardDatabase(opCtx, fromdb));
        uassert(ErrorCodes::IllegalOperation,
                "Cannot copy from a sharded database",
                !fromDbInfo.shardingEnabled());

        BSONObjBuilder b;
        BSONForEach(e, CommandHelpers::filterCommandRequestForPassthrough(cmdObj)) {
            if (strcmp(e.fieldName(), "fromhost") != 0) {
                b.append(e);
            }
        }

        {
            const auto shard = uassertStatusOK(
                Grid::get(opCtx)->shardRegistry()->getShard(opCtx, fromDbInfo.primaryId()));
            b.append("fromhost", shard->getConnString().toString());
        }

        // copyDb creates multiple collections and should handle collection creation differently.
        return passthrough(opCtx,
                           NamespaceString::kAdminDb,
                           toDbInfo.primaryId(),
                           appendAllowImplicitCreate(b.obj(), true),
                           result);
    }

} clusterCopyDBCmd;

class ConvertToCappedCmd : public NotAllowedOnShardedCollectionCmd {
public:
    ConvertToCappedCmd() : NotAllowedOnShardedCollectionCmd("convertToCapped") {}

    std::string parseNs(const std::string& dbname, const BSONObj& cmdObj) const override {
        return CommandHelpers::parseNsCollectionRequired(dbname, cmdObj).ns();
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
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
        // convertToCapped creates a temp collection and renames it at the end. It will require
        // special handling for create collection.
        return NotAllowedOnShardedCollectionCmd::run(
            opCtx, dbName, appendAllowImplicitCreate(cmdObj, true), result);
    }
} convertToCappedCmd;

class GroupCmd : public NotAllowedOnShardedCollectionCmd {
public:
    GroupCmd() : NotAllowedOnShardedCollectionCmd("group") {}

    void addRequiredPrivileges(const std::string& dbname,
                               const BSONObj& cmdObj,
                               std::vector<Privilege>* out) const override {
        ActionSet actions;
        actions.addAction(ActionType::find);
        out->push_back(Privilege(parseResourcePattern(dbname, cmdObj), actions));
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    std::string parseNs(const std::string& dbname, const BSONObj& cmdObj) const override {
        const auto nsElt = cmdObj.firstElement().embeddedObjectUserCheck()["ns"];
        uassert(ErrorCodes::InvalidNamespace,
                "'ns' must be of type String",
                nsElt.type() == BSONType::String);
        const NamespaceString nss(dbname, nsElt.valueStringData());
        uassert(ErrorCodes::InvalidNamespace,
                str::stream() << "Invalid namespace: " << nss.ns(),
                nss.isValid());
        return nss.ns();
    }

    Status explain(OperationContext* opCtx,
                   const OpMsgRequest& request,
                   ExplainOptions::Verbosity verbosity,
                   BSONObjBuilder* out) const override {
        std::string dbname = request.getDatabase().toString();
        const BSONObj& cmdObj = request.body;
        // We will time how long it takes to run the commands on the shards.
        Timer timer;
        BSONObj command = ClusterExplain::wrapAsExplain(cmdObj, verbosity);
        const NamespaceString nss(parseNs(dbname, cmdObj));

        auto routingInfo =
            uassertStatusOK(Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(opCtx, nss));
        uassert(ErrorCodes::IllegalOperation,
                str::stream() << "Passthrough command failed: " << command.toString() << " on ns "
                              << nss.ns()
                              << ". Cannot run on sharded namespace.",
                !routingInfo.cm());

        BSONObj shardResult;
        try {
            ShardConnection conn(routingInfo.primary()->getConnString(), "");

            // TODO: this can throw a stale config when mongos is not up-to-date -- fix.
            if (!conn->runCommand(nss.db().toString(), command, shardResult)) {
                conn.done();
                return Status(ErrorCodes::OperationFailed,
                              str::stream() << "Passthrough command failed: " << command
                                            << " on ns "
                                            << nss.ns()
                                            << "; result: "
                                            << shardResult);
            }

            conn.done();
        } catch (const DBException& ex) {
            return ex.toStatus();
        }

        // Fill out the command result.
        Strategy::CommandResult cmdResult;
        cmdResult.shardTargetId = routingInfo.primaryId();
        cmdResult.result = shardResult;
        cmdResult.target = routingInfo.primary()->getConnString();

        return ClusterExplain::buildExplainResult(
            opCtx, {cmdResult}, ClusterExplain::kSingleShard, timer.millis(), out);
    }

} groupCmd;

class SplitVectorCmd : public NotAllowedOnShardedCollectionCmd {
public:
    SplitVectorCmd() : NotAllowedOnShardedCollectionCmd("splitVector") {}

    std::string parseNs(const std::string& dbname, const BSONObj& cmdObj) const override {
        return CommandHelpers::parseNsFullyQualified(dbname, cmdObj);
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

        return NotAllowedOnShardedCollectionCmd::run(opCtx, dbName, cmdObj, result);
    }

} splitVectorCmd;

class CmdListCollections : public BasicCommand {
public:
    CmdListCollections() : BasicCommand("listCollections") {}

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool adminOnly() const override {
        return false;
    }

    Status checkAuthForCommand(Client* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) const final {
        AuthorizationSession* authzSession = AuthorizationSession::get(client);

        // Check for the listCollections ActionType on the database
        // or find on system.namespaces for pre 3.0 systems.
        if (authzSession->isAuthorizedForActionsOnResource(ResourcePattern::forDatabaseName(dbname),
                                                           ActionType::listCollections) ||
            authzSession->isAuthorizedForActionsOnResource(
                ResourcePattern::forExactNamespace(NamespaceString(dbname, "system.namespaces")),
                ActionType::find)) {
            return Status::OK();
        }

        return Status(ErrorCodes::Unauthorized,
                      str::stream() << "Not authorized to list collections on db: " << dbname);
    }

    bool run(OperationContext* opCtx,
             const std::string& dbName,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        auto nss = NamespaceString::makeListCollectionsNSS(dbName);

        auto dbInfoStatus = Grid::get(opCtx)->catalogCache()->getDatabase(opCtx, dbName);
        if (!dbInfoStatus.isOK()) {
            return appendEmptyResultSet(result, dbInfoStatus.getStatus(), nss.ns());
        }

        const auto& dbInfo = dbInfoStatus.getValue();

        return cursorCommandPassthrough(opCtx, dbName, dbInfo.primaryId(), cmdObj, nss, &result);
    }

} cmdListCollections;

class CmdListIndexes : public BasicCommand {
public:
    CmdListIndexes() : BasicCommand("listIndexes") {}

    std::string parseNs(const std::string& dbname, const BSONObj& cmdObj) const override {
        return CommandHelpers::parseNsCollectionRequired(dbname, cmdObj).ns();
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool adminOnly() const override {
        return false;
    }

    Status checkAuthForCommand(Client* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) const override {
        AuthorizationSession* authzSession = AuthorizationSession::get(client);

        // Check for the listIndexes ActionType on the database, or find on system.indexes for pre
        // 3.0 systems.
        const NamespaceString ns(parseNs(dbname, cmdObj));

        if (authzSession->isAuthorizedForActionsOnResource(ResourcePattern::forExactNamespace(ns),
                                                           ActionType::listIndexes) ||
            authzSession->isAuthorizedForActionsOnResource(
                ResourcePattern::forExactNamespace(NamespaceString(dbname, "system.indexes")),
                ActionType::find)) {
            return Status::OK();
        }

        return Status(ErrorCodes::Unauthorized,
                      str::stream() << "Not authorized to list indexes on collection: "
                                    << ns.coll());
    }

    bool run(OperationContext* opCtx,
             const std::string& dbName,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        const NamespaceString nss(parseNs(dbName, cmdObj));

        const auto routingInfo =
            uassertStatusOK(Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(opCtx, nss));

        const auto commandNss = NamespaceString::makeListIndexesNSS(nss.db(), nss.coll());

        return cursorCommandPassthrough(
            opCtx, nss.db(), routingInfo.primaryId(), cmdObj, commandNss, &result);
    }

} cmdListIndexes;

}  // namespace
}  // namespace mongo
