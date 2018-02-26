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

class ValidateCmd : public PublicGridCommand {
public:
    ValidateCmd() : PublicGridCommand("validate") {}

    void addRequiredPrivileges(const std::string& dbname,
                               const BSONObj& cmdObj,
                               std::vector<Privilege>* out) const override {
        ActionSet actions;
        actions.addAction(ActionType::validate);
        out->push_back(Privilege(parseResourcePattern(dbname, cmdObj), actions));
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    bool run(OperationContext* opCtx,
             const std::string& dbName,
             const BSONObj& cmdObj,
             BSONObjBuilder& output) override {
        const NamespaceString nss(CommandHelpers::parseNsCollectionRequired(dbName, cmdObj));

        auto routingInfo =
            uassertStatusOK(Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(opCtx, nss));
        if (!routingInfo.cm()) {
            return passthrough(opCtx, dbName, routingInfo.primaryId(), cmdObj, output);
        }

        const auto cm = routingInfo.cm();

        std::vector<Strategy::CommandResult> results;
        const BSONObj query;
        Strategy::commandOp(opCtx,
                            dbName,
                            CommandHelpers::filterCommandRequestForPassthrough(cmdObj),
                            cm->getns().ns(),
                            query,
                            CollationSpec::kSimpleSpec,
                            &results);

        BSONObjBuilder rawResBuilder(output.subobjStart("raw"));
        bool isValid = true;
        bool errored = false;
        for (const auto& cmdResult : results) {
            const ShardId& shardName = cmdResult.shardTargetId;
            BSONObj result = cmdResult.result;
            const BSONElement valid = result["valid"];
            if (!valid.trueValue()) {
                isValid = false;
            }
            if (!result["errmsg"].eoo()) {
                // errmsg indicates a user error, so returning the message from one shard is
                // sufficient.
                output.append(result["errmsg"]);
                errored = true;
            }
            rawResBuilder.append(shardName.toString(), result);
        }
        rawResBuilder.done();

        output.appendBool("valid", isValid);

        int code = getUniqueCodeFromCommandResults(results);
        if (code != 0) {
            output.append("code", code);
            output.append("codeName", ErrorCodes::errorString(ErrorCodes::Error(code)));
        }

        if (errored) {
            return false;
        }
        return true;
    }

} validateCmd;

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

        return passthrough(opCtx, NamespaceString::kAdminDb, toDbInfo.primaryId(), b.obj(), result);
    }

} clusterCopyDBCmd;

class CollectionStats : public PublicGridCommand {
public:
    CollectionStats() : PublicGridCommand("collStats", "collstats") {}

    std::string parseNs(const std::string& dbname, const BSONObj& cmdObj) const override {
        return CommandHelpers::parseNsCollectionRequired(dbname, cmdObj).ns();
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    void addRequiredPrivileges(const std::string& dbname,
                               const BSONObj& cmdObj,
                               std::vector<Privilege>* out) const override {
        ActionSet actions;
        actions.addAction(ActionType::collStats);
        out->push_back(Privilege(parseResourcePattern(dbname, cmdObj), actions));
    }

    bool run(OperationContext* opCtx,
             const std::string& dbName,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        const NamespaceString nss(parseNs(dbName, cmdObj));

        auto routingInfo =
            uassertStatusOK(Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(opCtx, nss));
        if (!routingInfo.cm()) {
            result.appendBool("sharded", false);
            result.append("primary", routingInfo.primaryId().toString());
            return passthrough(opCtx, dbName, routingInfo.primaryId(), cmdObj, result);
        }

        const auto cm = routingInfo.cm();

        result.appendBool("sharded", true);

        BSONObjBuilder shardStats;
        std::map<std::string, long long> counts;
        std::map<std::string, long long> indexSizes;

        long long unscaledCollSize = 0;

        int nindexes = 0;
        bool warnedAboutIndexes = false;

        std::set<ShardId> shardIds;
        cm->getAllShardIds(&shardIds);

        for (const auto& shardId : shardIds) {
            const auto res = [&] {
                const auto shard =
                    uassertStatusOK(Grid::get(opCtx)->shardRegistry()->getShard(opCtx, shardId));
                auto commandResponse = uassertStatusOK(shard->runCommandWithFixedRetryAttempts(
                    opCtx,
                    ReadPreferenceSetting::get(opCtx),
                    dbName,
                    appendShardVersion(CommandHelpers::filterCommandRequestForPassthrough(cmdObj),
                                       cm->getVersion(shardId)),
                    Shard::RetryPolicy::kIdempotent));

                uassertStatusOK(commandResponse.commandStatus);
                return commandResponse.response;
            }();

            // We don't know the order that we will encounter the count and size, so we save them
            // until we've iterated through all the fields before updating unscaledCollSize
            const auto shardObjCount = static_cast<long long>(res["count"].Number());
            long long shardAvgObjSize = 0;

            for (const auto& e : res) {
                if (str::equals(e.fieldName(), "ns") ||              //
                    str::equals(e.fieldName(), "ok") ||              //
                    str::equals(e.fieldName(), "lastExtentSize") ||  //
                    str::equals(e.fieldName(), "paddingFactor")) {
                    continue;
                } else if (str::equals(e.fieldName(), "count") ||        //
                           str::equals(e.fieldName(), "size") ||         //
                           str::equals(e.fieldName(), "storageSize") ||  //
                           str::equals(e.fieldName(), "numExtents") ||   //
                           str::equals(e.fieldName(), "totalIndexSize")) {
                    counts[e.fieldName()] += e.numberLong();
                } else if (str::equals(e.fieldName(), "avgObjSize")) {
                    shardAvgObjSize = e.numberLong();
                } else if (str::equals(e.fieldName(), "indexSizes")) {
                    BSONObjIterator k(e.Obj());
                    while (k.more()) {
                        BSONElement temp = k.next();
                        indexSizes[temp.fieldName()] += temp.numberLong();
                    }
                } else if (str::equals(e.fieldName(), "userFlags")) {
                    if (!result.hasField(e.fieldName()))
                        result.append(e);
                } else if (str::equals(e.fieldName(), "capped")) {
                    if (!result.hasField(e.fieldName()))
                        result.append(e);
                } else if (str::equals(e.fieldName(), "paddingFactorNote")) {
                    if (!result.hasField(e.fieldName()))
                        result.append(e);
                } else if (str::equals(e.fieldName(), "indexDetails")) {
                    // skip this field in the rollup
                } else if (str::equals(e.fieldName(), "nindexes")) {
                    int myIndexes = e.numberInt();

                    if (nindexes == 0) {
                        nindexes = myIndexes;
                    } else if (nindexes == myIndexes) {
                        // no-op
                    } else {
                        // hopefully this means we're building an index

                        if (myIndexes > nindexes)
                            nindexes = myIndexes;

                        if (!warnedAboutIndexes) {
                            result.append("warning",
                                          "indexes don't all match - ok if ensureIndex is running");
                            warnedAboutIndexes = true;
                        }
                    }
                } else {
                    warning() << "mongos collstats doesn't know about: " << e.fieldName();
                }
            }
            shardStats.append(shardId.toString(), res);
            unscaledCollSize += shardAvgObjSize * shardObjCount;
        }

        result.append("ns", nss.ns());

        for (const auto& countEntry : counts) {
            result.appendNumber(countEntry.first, countEntry.second);
        }

        {
            BSONObjBuilder ib(result.subobjStart("indexSizes"));
            for (const auto& entry : indexSizes) {
                ib.appendNumber(entry.first, entry.second);
            }
            ib.done();
        }

        // The unscaled avgObjSize for each shard is used to get the unscaledCollSize
        // because the raw size returned by the shard is affected by the command's
        // scale parameter.
        if (counts["count"] > 0)
            result.append("avgObjSize", (double)unscaledCollSize / (double)counts["count"]);
        else
            result.append("avgObjSize", 0.0);

        result.append("nindexes", nindexes);

        result.append("nchunks", cm->numChunks());
        result.append("shards", shardStats.obj());

        return true;
    }

} collectionStatsCmd;

class DataSizeCmd : public PublicGridCommand {
public:
    DataSizeCmd() : PublicGridCommand("dataSize", "datasize") {}

    std::string parseNs(const std::string& dbname, const BSONObj& cmdObj) const override {
        return CommandHelpers::parseNsFullyQualified(dbname, cmdObj);
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    void addRequiredPrivileges(const std::string& dbname,
                               const BSONObj& cmdObj,
                               std::vector<Privilege>* out) const override {
        ActionSet actions;
        actions.addAction(ActionType::find);
        out->push_back(Privilege(parseResourcePattern(dbname, cmdObj), actions));
    }

    bool run(OperationContext* opCtx,
             const std::string& dbName,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        const NamespaceString nss(parseNs(dbName, cmdObj));

        auto routingInfo =
            uassertStatusOK(Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(opCtx, nss));
        if (!routingInfo.cm()) {
            return passthrough(opCtx, dbName, routingInfo.primaryId(), cmdObj, result);
        }

        const auto cm = routingInfo.cm();

        BSONObj min = cmdObj.getObjectField("min");
        BSONObj max = cmdObj.getObjectField("max");
        BSONObj keyPattern = cmdObj.getObjectField("keyPattern");

        uassert(13408,
                "keyPattern must equal shard key",
                SimpleBSONObjComparator::kInstance.evaluate(cm->getShardKeyPattern().toBSON() ==
                                                            keyPattern));
        uassert(13405,
                str::stream() << "min value " << min << " does not have shard key",
                cm->getShardKeyPattern().isShardKey(min));
        uassert(13406,
                str::stream() << "max value " << max << " does not have shard key",
                cm->getShardKeyPattern().isShardKey(max));

        min = cm->getShardKeyPattern().normalizeShardKey(min);
        max = cm->getShardKeyPattern().normalizeShardKey(max);

        // yes these are doubles...
        double size = 0;
        double numObjects = 0;
        int millis = 0;

        std::set<ShardId> shardIds;
        cm->getShardIdsForRange(min, max, &shardIds);

        for (const ShardId& shardId : shardIds) {
            const auto shardStatus = Grid::get(opCtx)->shardRegistry()->getShard(opCtx, shardId);
            if (!shardStatus.isOK()) {
                continue;
            }

            ScopedDbConnection conn(shardStatus.getValue()->getConnString());
            BSONObj res;
            bool ok = conn->runCommand(
                dbName, CommandHelpers::filterCommandRequestForPassthrough(cmdObj), res);
            conn.done();

            if (!ok) {
                CommandHelpers::filterCommandReplyForPassthrough(res, &result);
                return false;
            }

            size += res["size"].number();
            numObjects += res["numObjects"].number();
            millis += res["millis"].numberInt();
        }

        result.append("size", size);
        result.append("numObjects", numObjects);
        result.append("millis", millis);
        return true;
    }

} dataSizeCmd;

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
                   const std::string& dbname,
                   const BSONObj& cmdObj,
                   ExplainOptions::Verbosity verbosity,
                   BSONObjBuilder* out) const override {
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

class FileMD5Cmd : public PublicGridCommand {
public:
    FileMD5Cmd() : PublicGridCommand("filemd5") {}

    std::string help() const override {
        return " example: { filemd5 : ObjectId(aaaaaaa) , root : \"fs\" }";
    }

    std::string parseNs(const std::string& dbname, const BSONObj& cmdObj) const override {
        std::string collectionName;
        if (const auto rootElt = cmdObj["root"]) {
            uassert(ErrorCodes::InvalidNamespace,
                    "'root' must be of type String",
                    rootElt.type() == BSONType::String);
            collectionName = rootElt.str();
        }
        if (collectionName.empty())
            collectionName = "fs";
        collectionName += ".chunks";
        return NamespaceString(dbname, collectionName).ns();
    }

    void addRequiredPrivileges(const std::string& dbname,
                               const BSONObj& cmdObj,
                               std::vector<Privilege>* out) const override {
        out->push_back(Privilege(parseResourcePattern(dbname, cmdObj), ActionType::find));
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    bool run(OperationContext* opCtx,
             const std::string& dbName,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        const NamespaceString nss(parseNs(dbName, cmdObj));

        auto routingInfo =
            uassertStatusOK(Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(opCtx, nss));
        if (!routingInfo.cm()) {
            return passthrough(opCtx, dbName, routingInfo.primaryId(), cmdObj, result);
        }

        const auto cm = routingInfo.cm();

        if (SimpleBSONObjComparator::kInstance.evaluate(cm->getShardKeyPattern().toBSON() ==
                                                        BSON("files_id" << 1))) {
            BSONObj finder = BSON("files_id" << cmdObj.firstElement());

            std::vector<Strategy::CommandResult> results;
            Strategy::commandOp(opCtx,
                                dbName,
                                CommandHelpers::filterCommandRequestForPassthrough(cmdObj),
                                nss.ns(),
                                finder,
                                CollationSpec::kSimpleSpec,
                                &results);
            verify(results.size() == 1);  // querying on shard key so should only talk to one shard
            BSONObj res = results.begin()->result;

            CommandHelpers::filterCommandReplyForPassthrough(res, &result);
            return res["ok"].trueValue();
        } else if (SimpleBSONObjComparator::kInstance.evaluate(cm->getShardKeyPattern().toBSON() ==
                                                               BSON("files_id" << 1 << "n" << 1))) {
            int n = 0;
            BSONObj lastResult;

            while (true) {
                // Theory of operation: Starting with n=0, send filemd5 command to shard
                // with that chunk (gridfs chunk not sharding chunk). That shard will then
                // compute a partial md5 state (passed in the "md5state" field) for all
                // contiguous chunks that it has. When it runs out or hits a discontinuity
                // (eg [1,2,7]) it returns what it has done so far. This is repeated as
                // long as we keep getting more chunks. The end condition is when we go to
                // look for chunk n and it doesn't exist. This means that the file's last
                // chunk is n-1, so we return the computed md5 results.
                BSONObjBuilder bb(CommandHelpers::filterCommandRequestForPassthrough(cmdObj));
                bb.appendBool("partialOk", true);
                bb.append("startAt", n);
                if (!lastResult.isEmpty()) {
                    bb.append(lastResult["md5state"]);
                }
                BSONObj shardCmd = bb.obj();

                BSONObj finder = BSON("files_id" << cmdObj.firstElement() << "n" << n);

                std::vector<Strategy::CommandResult> results;
                try {
                    Strategy::commandOp(opCtx,
                                        dbName,
                                        shardCmd,
                                        nss.ns(),
                                        finder,
                                        CollationSpec::kSimpleSpec,
                                        &results);
                } catch (DBException& e) {
                    // This is handled below and logged
                    Strategy::CommandResult errResult;
                    errResult.shardTargetId = ShardId();
                    errResult.result = BSON("errmsg" << e.what() << "ok" << 0);
                    results.push_back(errResult);
                }

                verify(results.size() ==
                       1);  // querying on shard key so should only talk to one shard
                BSONObj res = results.begin()->result;
                bool ok = res["ok"].trueValue();

                if (!ok) {
                    // Add extra info to make debugging easier
                    result.append("failedAt", n);
                    result.append("sentCommand", shardCmd);
                    BSONForEach(e, res) {
                        if (!str::equals(e.fieldName(), "errmsg"))
                            result.append(e);
                    }

                    log() << "Sharded filemd5 failed: " << redact(result.asTempObj());

                    result.append("errmsg",
                                  str::stream() << "sharded filemd5 failed because: "
                                                << res["errmsg"].valuestrsafe());
                    return false;
                }

                uassert(
                    16246,
                    str::stream() << "Shard for database " << nss.db()
                                  << " is too old to support GridFS sharded by {files_id:1, n:1}",
                    res.hasField("md5state"));

                lastResult = res;
                int nNext = res["numChunks"].numberInt();

                if (n == nNext) {
                    // no new data means we've reached the end of the file
                    CommandHelpers::filterCommandReplyForPassthrough(res, &result);
                    return true;
                }

                verify(nNext > n);
                n = nNext;
            }

            verify(0);
        }

        // We could support arbitrary shard keys by sending commands to all shards but I don't
        // think we should
        result.append("errmsg",
                      "GridFS fs.chunks collection must be sharded on either {files_id:1} or "
                      "{files_id:1, n:1}");
        return false;
    }

} fileMD5Cmd;

/**
 * The geoNear command is deprecated. Users should prefer the $near query operator, the $nearSphere
 * query operator, or the $geoNear aggregation stage. See
 * http://dochub.mongodb.org/core/geoNear-deprecation for more detail.
 */
class Geo2dFindNearCmd : public PublicGridCommand {
public:
    Geo2dFindNearCmd() : PublicGridCommand("geoNear") {}

    std::string help() const override {
        return "http://dochub.mongodb.org/core/geo#GeospatialIndexing-geoNearCommand";
    }

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

    bool run(OperationContext* opCtx,
             const std::string& dbName,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        RARELY {
            warning() << "Support for the geoNear command has been deprecated. Please plan to "
                         "rewrite geoNear commands using the $near query operator, the $nearSphere "
                         "query operator, or the $geoNear aggregation stage. See "
                         "http://dochub.mongodb.org/core/geoNear-deprecation.";
        }

        const NamespaceString nss(CommandHelpers::parseNsCollectionRequired(dbName, cmdObj));

        auto routingInfo =
            uassertStatusOK(Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(opCtx, nss));
        if (!routingInfo.cm()) {
            return passthrough(opCtx, dbName, routingInfo.primaryId(), cmdObj, result);
        }

        const auto cm = routingInfo.cm();

        auto query = extractQuery(cmdObj);
        auto collation = extractCollation(cmdObj);

        std::set<ShardId> shardIds;
        cm->getShardIdsForQuery(opCtx, query, collation, &shardIds);

        // We support both "num" and "limit" options to control limit
        long long limit = 100;
        const char* limitName = cmdObj["num"].isNumber() ? "num" : "limit";
        if (cmdObj[limitName].isNumber())
            limit = cmdObj[limitName].safeNumberLong();

        // Construct the requests.
        std::vector<AsyncRequestsSender::Request> requests;
        BSONArrayBuilder shardArray;
        for (const ShardId& shardId : shardIds) {
            requests.emplace_back(shardId,
                                  CommandHelpers::filterCommandRequestForPassthrough(cmdObj));
            shardArray.append(shardId.toString());
        }

        // Send the requests.
        AsyncRequestsSender ars(opCtx,
                                Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor(),
                                dbName,
                                requests,
                                ReadPreferenceSetting::get(opCtx),
                                Shard::RetryPolicy::kIdempotent);

        // Receive the responses.
        std::multimap<double, BSONObj> results;  // TODO: maybe use merge-sort instead
        std::string nearStr;
        double time = 0;
        double btreelocs = 0;
        double nscanned = 0;
        double objectsLoaded = 0;
        while (!ars.done()) {
            // Block until a response is available.
            auto shardResult = uassertStatusOK(ars.next().swResponse).data;
            uassertStatusOK(getStatusFromCommandResult(shardResult));
            if (shardResult.hasField("near")) {
                nearStr = shardResult["near"].String();
            }
            time += shardResult["stats"]["time"].Number();
            if (!shardResult["stats"]["btreelocs"].eoo()) {
                btreelocs += shardResult["stats"]["btreelocs"].Number();
            }
            nscanned += shardResult["stats"]["nscanned"].Number();
            if (!shardResult["stats"]["objectsLoaded"].eoo()) {
                objectsLoaded += shardResult["stats"]["objectsLoaded"].Number();
            }

            BSONForEach(obj, shardResult["results"].embeddedObject()) {
                results.insert(
                    std::make_pair(obj["dis"].Number(), obj.embeddedObject().getOwned()));
            }

            // TODO: maybe shrink results if size() > limit
        }

        result.append("ns", nss.ns());
        result.append("near", nearStr);

        long long outCount = 0;
        double totalDistance = 0;
        double maxDistance = 0;
        {
            BSONArrayBuilder sub(result.subarrayStart("results"));
            for (std::multimap<double, BSONObj>::const_iterator it(results.begin()),
                 end(results.end());
                 it != end && outCount < limit;
                 ++it, ++outCount) {
                totalDistance += it->first;
                maxDistance = it->first;  // guaranteed to be highest so far

                sub.append(it->second);
            }
            sub.done();
        }

        {
            BSONObjBuilder sub(result.subobjStart("stats"));
            sub.append("time", time);
            sub.append("btreelocs", btreelocs);
            sub.append("nscanned", nscanned);
            sub.append("objectsLoaded", objectsLoaded);
            sub.append("avgDistance", (outCount == 0) ? 0 : (totalDistance / outCount));
            sub.append("maxDistance", maxDistance);
            sub.append("shards", shardArray.arr());
            sub.done();
        }

        return true;
    }

} geo2dFindNearCmd;

class EvalCmd : public PublicGridCommand {
public:
    EvalCmd() : PublicGridCommand("eval", "$eval") {}

    void addRequiredPrivileges(const std::string& dbname,
                               const BSONObj& cmdObj,
                               std::vector<Privilege>* out) const override {
        // $eval can do pretty much anything, so require all privileges.
        RoleGraph::generateUniversalPrivileges(out);
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    bool run(OperationContext* opCtx,
             const std::string& dbName,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        RARELY {
            warning() << "the eval command is deprecated" << startupWarningsLog;
        }

        // $eval isn't allowed to access sharded collections, but we need to leave the shard to
        // detect that
        const auto dbInfo =
            uassertStatusOK(Grid::get(opCtx)->catalogCache()->getDatabase(opCtx, dbName));
        return passthrough(opCtx, dbName, dbInfo.primaryId(), cmdObj, result);
    }

} evalCmd;

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
