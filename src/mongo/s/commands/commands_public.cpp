/**
 *    Copyright (C) 2008-2015 MongoDB Inc.
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

#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/connpool.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_manager_global.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/copydb.h"
#include "mongo/db/commands/rename_collection.h"
#include "mongo/db/lasterror.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/catalog/catalog_cache.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/client/shard_connection.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/cluster_last_error_info.h"
#include "mongo/s/commands/cluster_commands_common.h"
#include "mongo/s/commands/cluster_explain.h"
#include "mongo/s/commands/run_on_all_shards_cmd.h"
#include "mongo/s/commands/sharded_command_processing.h"
#include "mongo/s/config.h"
#include "mongo/s/grid.h"
#include "mongo/s/query/store_possible_cursor.h"
#include "mongo/s/sharding_raii.h"
#include "mongo/s/stale_exception.h"
#include "mongo/scripting/engine.h"
#include "mongo/util/log.h"
#include "mongo/util/timer.h"

namespace mongo {

using std::unique_ptr;
using std::shared_ptr;
using std::list;
using std::make_pair;
using std::map;
using std::multimap;
using std::set;
using std::string;
using std::stringstream;
using std::vector;

namespace {

bool cursorCommandPassthrough(OperationContext* txn,
                              shared_ptr<DBConfig> conf,
                              const BSONObj& cmdObj,
                              int options,
                              BSONObjBuilder* out) {
    const auto shard = grid.shardRegistry()->getShard(txn, conf->getPrimaryId());
    if (!shard) {
        return Command::appendCommandStatus(
            *out, {ErrorCodes::ShardNotFound, "failed to find a valid shard"});
    }
    ScopedDbConnection conn(shard->getConnString());
    auto cursor = conn->query(str::stream() << conf->name() << ".$cmd",
                              cmdObj,
                              -1,    // nToReturn
                              0,     // nToSkip
                              NULL,  // fieldsToReturn
                              options);
    if (!cursor || !cursor->more()) {
        return Command::appendCommandStatus(
            *out, {ErrorCodes::OperationFailed, "failed to read command response from shard"});
    }
    BSONObj response = cursor->nextSafe().getOwned();
    conn.done();
    Status status = getStatusFromCommandResult(response);
    if (ErrorCodes::SendStaleConfig == status || ErrorCodes::RecvStaleConfig == status) {
        throw RecvStaleConfigException("command failed because of stale config", response);
    }
    if (!status.isOK()) {
        return Command::appendCommandStatus(*out, status);
    }

    StatusWith<BSONObj> transformedResponse =
        storePossibleCursor(HostAndPort(cursor->originalHost()),
                            response,
                            grid.getExecutorPool()->getArbitraryExecutor(),
                            grid.getCursorManager());
    if (!transformedResponse.isOK()) {
        return Command::appendCommandStatus(*out, transformedResponse.getStatus());
    }
    out->appendElements(transformedResponse.getValue());

    return true;
}

BSONObj getQuery(const BSONObj& cmdObj) {
    if (cmdObj["query"].type() == Object)
        return cmdObj["query"].embeddedObject();
    if (cmdObj["q"].type() == Object)
        return cmdObj["q"].embeddedObject();
    return BSONObj();
}

class PublicGridCommand : public Command {
public:
    PublicGridCommand(const char* n, const char* oldname = NULL) : Command(n, false, oldname) {}
    virtual bool slaveOk() const {
        return true;
    }
    virtual bool adminOnly() const {
        return false;
    }

    // Override if passthrough should also send query options
    // Safer as off by default, can slowly enable as we add more tests
    virtual bool passOptions() const {
        return false;
    }

    // all grid commands are designed not to lock

protected:
    bool passthrough(OperationContext* txn,
                     DBConfig* conf,
                     const BSONObj& cmdObj,
                     BSONObjBuilder& result) {
        return _passthrough(txn, conf->name(), conf, cmdObj, 0, result);
    }

    bool adminPassthrough(OperationContext* txn,
                          DBConfig* conf,
                          const BSONObj& cmdObj,
                          BSONObjBuilder& result) {
        return _passthrough(txn, "admin", conf, cmdObj, 0, result);
    }

    bool passthrough(OperationContext* txn,
                     DBConfig* conf,
                     const BSONObj& cmdObj,
                     int options,
                     BSONObjBuilder& result) {
        return _passthrough(txn, conf->name(), conf, cmdObj, options, result);
    }

private:
    bool _passthrough(OperationContext* txn,
                      const string& db,
                      DBConfig* conf,
                      const BSONObj& cmdObj,
                      int options,
                      BSONObjBuilder& result) {
        const auto shard = grid.shardRegistry()->getShard(txn, conf->getPrimaryId());
        ShardConnection conn(shard->getConnString(), "");

        BSONObj res;
        bool ok = conn->runCommand(db, cmdObj, res, passOptions() ? options : 0);
        conn.done();

        // First append the properly constructed writeConcernError. It will then be skipped
        // in appendElementsUnique.
        if (auto wcErrorElem = res["writeConcernError"]) {
            appendWriteConcernErrorToCmdResponse(shard->getId(), wcErrorElem, result);
        }
        result.appendElementsUnique(res);
        return ok;
    }
};

class AllShardsCollectionCommand : public RunOnAllShardsCommand {
public:
    AllShardsCollectionCommand(const char* n,
                               const char* oldname = NULL,
                               bool useShardConn = false,
                               bool implicitCreateDb = false)
        : RunOnAllShardsCommand(n, oldname, useShardConn, implicitCreateDb) {}

    virtual void getShardIds(OperationContext* txn,
                             const string& dbName,
                             BSONObj& cmdObj,
                             vector<ShardId>& shardIds) {
        const string fullns = dbName + '.' + cmdObj.firstElement().valuestrsafe();

        auto status = grid.catalogCache()->getDatabase(txn, dbName);
        uassertStatusOK(status.getStatus());

        shared_ptr<DBConfig> conf = status.getValue();

        if (!conf->isShardingEnabled() || !conf->isSharded(fullns)) {
            shardIds.push_back(conf->getShardId(txn, fullns));
        } else {
            grid.shardRegistry()->getAllShardIds(&shardIds);
        }
    }
};

class NotAllowedOnShardedCollectionCmd : public PublicGridCommand {
public:
    NotAllowedOnShardedCollectionCmd(const char* n) : PublicGridCommand(n) {}

    virtual bool run(OperationContext* txn,
                     const string& dbName,
                     BSONObj& cmdObj,
                     int options,
                     string& errmsg,
                     BSONObjBuilder& result) {
        const string fullns = parseNs(dbName, cmdObj);

        auto conf = uassertStatusOK(grid.catalogCache()->getDatabase(txn, dbName));
        if (!conf->isSharded(fullns)) {
            return passthrough(txn, conf.get(), cmdObj, options, result);
        }

        return appendCommandStatus(
            result,
            Status(ErrorCodes::IllegalOperation,
                   str::stream() << "can't do command: " << getName() << " on sharded collection"));
    }
};

// MongoS commands implementation

class DropIndexesCmd : public AllShardsCollectionCommand {
public:
    DropIndexesCmd() : AllShardsCollectionCommand("dropIndexes", "deleteIndexes") {}
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {
        ActionSet actions;
        actions.addAction(ActionType::dropIndex);
        out->push_back(Privilege(parseResourcePattern(dbname, cmdObj), actions));
    }
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }
} dropIndexesCmd;

class CreateIndexesCmd : public AllShardsCollectionCommand {
public:
    CreateIndexesCmd()
        : AllShardsCollectionCommand("createIndexes",
                                     NULL, /* oldName */
                                     true /* use ShardConnection */,
                                     true /* implicit create db */) {
        // createIndexes command should use ShardConnection so the getLastError would
        // be able to properly enforce the write concern (via the saveGLEStats callback).
    }

    /**
     * the createIndexes command doesn't require the 'ns' field to be populated
     * so we make sure its here as its needed for the system.indexes insert
     */
    BSONObj fixSpec(const NamespaceString& ns, const BSONObj& original) const {
        if (original["ns"].type() == String)
            return original;
        BSONObjBuilder bb;
        bb.appendElements(original);
        bb.append("ns", ns.toString());
        return bb.obj();
    }

    /**
     * @return equivalent of gle
     */
    BSONObj createIndexLegacy(const string& server,
                              const NamespaceString& nss,
                              const BSONObj& spec) const {
        try {
            ScopedDbConnection conn(server);
            conn->insert(nss.getSystemIndexesCollection(), spec);
            BSONObj gle = conn->getLastErrorDetailed(nss.db().toString());
            conn.done();
            return gle;
        } catch (DBException& e) {
            BSONObjBuilder b;
            b.append("errmsg", e.toString());
            b.append("code", e.getCode());
            return b.obj();
        }
    }

    virtual BSONObj specialErrorHandler(const string& server,
                                        const string& dbName,
                                        const BSONObj& cmdObj,
                                        const BSONObj& originalResult) const {
        string errmsg = originalResult["errmsg"];
        if (errmsg.find("no such cmd") == string::npos) {
            // cannot use codes as 2.4 didn't have a code for this
            return originalResult;
        }

        // we need to down convert

        NamespaceString nss(dbName, cmdObj["createIndexes"].String());

        if (cmdObj["indexes"].type() != Array)
            return originalResult;

        BSONObjBuilder newResult;
        newResult.append("note", "downgraded");
        newResult.append("sentTo", server);

        BSONArrayBuilder individualResults;

        bool ok = true;

        BSONObjIterator indexIterator(cmdObj["indexes"].Obj());
        while (indexIterator.more()) {
            BSONObj spec = indexIterator.next().Obj();
            spec = fixSpec(nss, spec);

            BSONObj gle = createIndexLegacy(server, nss, spec);

            individualResults.append(BSON("spec" << spec << "gle" << gle));

            BSONElement e = gle["errmsg"];
            if (e.type() == String && e.String().size() > 0) {
                ok = false;
                newResult.appendAs(e, "errmsg");
                break;
            }

            e = gle["err"];
            if (e.type() == String && e.String().size() > 0) {
                ok = false;
                newResult.appendAs(e, "errmsg");
                break;
            }
        }

        newResult.append("eachIndex", individualResults.arr());

        newResult.append("ok", ok ? 1 : 0);
        return newResult.obj();
    }

    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {
        ActionSet actions;
        actions.addAction(ActionType::createIndex);
        out->push_back(Privilege(parseResourcePattern(dbname, cmdObj), actions));
    }

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

} createIndexesCmd;

class ReIndexCmd : public AllShardsCollectionCommand {
public:
    ReIndexCmd() : AllShardsCollectionCommand("reIndex") {}
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {
        ActionSet actions;
        actions.addAction(ActionType::reIndex);
        out->push_back(Privilege(parseResourcePattern(dbname, cmdObj), actions));
    }

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }
} reIndexCmd;

class CollectionModCmd : public AllShardsCollectionCommand {
public:
    CollectionModCmd() : AllShardsCollectionCommand("collMod") {}
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {
        ActionSet actions;
        actions.addAction(ActionType::collMod);
        out->push_back(Privilege(parseResourcePattern(dbname, cmdObj), actions));
    }

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }
} collectionModCmd;


class ValidateCmd : public PublicGridCommand {
public:
    ValidateCmd() : PublicGridCommand("validate") {}
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {
        ActionSet actions;
        actions.addAction(ActionType::validate);
        out->push_back(Privilege(parseResourcePattern(dbname, cmdObj), actions));
    }

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    bool run(OperationContext* txn,
             const string& dbName,
             BSONObj& cmdObj,
             int options,
             string& errmsg,
             BSONObjBuilder& output) {
        const NamespaceString nss = parseNsCollectionRequired(dbName, cmdObj);

        auto conf = uassertStatusOK(grid.catalogCache()->getDatabase(txn, dbName));
        if (!conf->isShardingEnabled() || !conf->isSharded(nss.ns())) {
            return passthrough(txn, conf.get(), cmdObj, output);
        }

        shared_ptr<ChunkManager> cm = conf->getChunkManager(txn, nss.ns());
        massert(40051, "chunk manager should not be null", cm);

        vector<Strategy::CommandResult> results;
        Strategy::commandOp(txn, dbName, cmdObj, options, cm->getns(), BSONObj(), &results);

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
                errmsg = result["errmsg"].toString();
                errored = true;
            }
            rawResBuilder.append(shardName.toString(), result);
        }
        rawResBuilder.done();

        output.appendBool("valid", isValid);

        int code = getUniqueCodeFromCommandResults(results);
        if (code != 0) {
            output.append("code", code);
        }

        if (errored) {
            return false;
        }
        return true;
    }
} validateCmd;

class CreateCmd : public PublicGridCommand {
public:
    CreateCmd() : PublicGridCommand("create") {}
    virtual Status checkAuthForCommand(ClientBasic* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) {
        AuthorizationSession* authzSession = AuthorizationSession::get(client);
        if (cmdObj["capped"].trueValue()) {
            if (!authzSession->isAuthorizedForActionsOnResource(
                    parseResourcePattern(dbname, cmdObj), ActionType::convertToCapped)) {
                return Status(ErrorCodes::Unauthorized, "unauthorized");
            }
        }

        // ActionType::createCollection or ActionType::insert are both acceptable
        if (authzSession->isAuthorizedForActionsOnResource(parseResourcePattern(dbname, cmdObj),
                                                           ActionType::createCollection) ||
            authzSession->isAuthorizedForActionsOnResource(parseResourcePattern(dbname, cmdObj),
                                                           ActionType::insert)) {
            return Status::OK();
        }

        return Status(ErrorCodes::Unauthorized, "unauthorized");
    }
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }
    bool run(OperationContext* txn,
             const string& dbName,
             BSONObj& cmdObj,
             int,
             string& errmsg,
             BSONObjBuilder& result) {
        auto dbStatus = ScopedShardDatabase::getOrCreate(txn, dbName);
        if (!dbStatus.isOK()) {
            return appendCommandStatus(result, dbStatus.getStatus());
        }

        auto scopedDb = std::move(dbStatus.getValue());
        return passthrough(txn, scopedDb.db(), cmdObj, result);
    }

} createCmd;

class DropCmd : public PublicGridCommand {
public:
    DropCmd() : PublicGridCommand("drop") {}
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {
        ActionSet actions;
        actions.addAction(ActionType::dropCollection);
        out->push_back(Privilege(parseResourcePattern(dbname, cmdObj), actions));
    }

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    bool run(OperationContext* txn,
             const string& dbName,
             BSONObj& cmdObj,
             int options,
             string& errmsg,
             BSONObjBuilder& result) {
        auto status = grid.catalogCache()->getDatabase(txn, dbName);
        if (!status.isOK()) {
            if (status == ErrorCodes::NamespaceNotFound) {
                return true;
            }

            return appendCommandStatus(result, status.getStatus());
        }

        const NamespaceString fullns = parseNsCollectionRequired(dbName, cmdObj);

        log() << "DROP: " << fullns;

        const auto& db = status.getValue();
        if (!db->isShardingEnabled() || !db->isSharded(fullns.ns())) {
            log() << "\tdrop going to do passthrough";
            return passthrough(txn, db.get(), cmdObj, result);
        }

        uassertStatusOK(grid.catalogClient(txn)->dropCollection(txn, fullns));

        // Force a full reload next time the just dropped namespace is accessed
        db->invalidateNs(fullns.ns());

        return true;
    }
} dropCmd;

class RenameCollectionCmd : public PublicGridCommand {
public:
    RenameCollectionCmd() : PublicGridCommand("renameCollection") {}
    virtual Status checkAuthForCommand(ClientBasic* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) {
        return rename_collection::checkAuthForRenameCollectionCommand(client, dbname, cmdObj);
    }
    virtual bool adminOnly() const {
        return true;
    }
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }
    bool run(OperationContext* txn,
             const string& dbName,
             BSONObj& cmdObj,
             int,
             string& errmsg,
             BSONObjBuilder& result) {
        const string fullnsFrom = cmdObj.firstElement().valuestrsafe();
        const string dbNameFrom = nsToDatabase(fullnsFrom);
        auto confFrom = uassertStatusOK(grid.catalogCache()->getDatabase(txn, dbNameFrom));

        const string fullnsTo = cmdObj["to"].valuestrsafe();
        const string dbNameTo = nsToDatabase(fullnsTo);
        auto confTo = uassertStatusOK(grid.catalogCache()->getDatabase(txn, dbNameTo));

        uassert(13138, "You can't rename a sharded collection", !confFrom->isSharded(fullnsFrom));
        uassert(13139, "You can't rename to a sharded collection", !confTo->isSharded(fullnsTo));

        const ShardId& shardTo = confTo->getShardId(txn, fullnsTo);
        const ShardId& shardFrom = confFrom->getShardId(txn, fullnsFrom);

        uassert(13137,
                "Source and destination collections must be on same shard",
                shardFrom == shardTo);

        return adminPassthrough(txn, confFrom.get(), cmdObj, result);
    }
} renameCollectionCmd;

class CopyDBCmd : public PublicGridCommand {
public:
    CopyDBCmd() : PublicGridCommand("copydb") {}
    virtual bool adminOnly() const {
        return true;
    }
    virtual Status checkAuthForCommand(ClientBasic* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) {
        return copydb::checkAuthForCopydbCommand(client, dbname, cmdObj);
    }
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    bool run(OperationContext* txn,
             const string& dbName,
             BSONObj& cmdObj,
             int,
             string& errmsg,
             BSONObjBuilder& result) {
        const string todb = cmdObj.getStringField("todb");
        uassert(ErrorCodes::EmptyFieldName, "missing todb argument", !todb.empty());
        uassert(ErrorCodes::InvalidNamespace,
                "invalid todb argument",
                NamespaceString::validDBName(todb, NamespaceString::DollarInDbNameBehavior::Allow));

        auto scopedToDb = uassertStatusOK(ScopedShardDatabase::getOrCreate(txn, todb));
        uassert(ErrorCodes::IllegalOperation,
                "cannot copy to a sharded database",
                !scopedToDb.db()->isShardingEnabled());

        const string fromhost = cmdObj.getStringField("fromhost");
        if (!fromhost.empty()) {
            return adminPassthrough(txn, scopedToDb.db(), cmdObj, result);
        } else {
            const string fromdb = cmdObj.getStringField("fromdb");
            uassert(13399, "need a fromdb argument", !fromdb.empty());

            shared_ptr<DBConfig> confFrom =
                uassertStatusOK(grid.catalogCache()->getDatabase(txn, fromdb));

            uassert(13400, "don't know where source DB is", confFrom);
            uassert(13401, "cant copy from sharded DB", !confFrom->isShardingEnabled());

            BSONObjBuilder b;
            BSONForEach(e, cmdObj) {
                if (strcmp(e.fieldName(), "fromhost") != 0) {
                    b.append(e);
                }
            }

            {
                const auto& shard = grid.shardRegistry()->getShard(txn, confFrom->getPrimaryId());
                b.append("fromhost", shard->getConnString().toString());
            }
            BSONObj fixed = b.obj();

            return adminPassthrough(txn, scopedToDb.db(), fixed, result);
        }
    }
} clusterCopyDBCmd;

class CollectionStats : public PublicGridCommand {
public:
    CollectionStats() : PublicGridCommand("collStats", "collstats") {}
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {
        ActionSet actions;
        actions.addAction(ActionType::collStats);
        out->push_back(Privilege(parseResourcePattern(dbname, cmdObj), actions));
    }

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    bool run(OperationContext* txn,
             const string& dbName,
             BSONObj& cmdObj,
             int,
             string& errmsg,
             BSONObjBuilder& result) {
        const string fullns = parseNs(dbName, cmdObj);

        auto conf = uassertStatusOK(grid.catalogCache()->getDatabase(txn, dbName));
        if (!conf->isShardingEnabled() || !conf->isSharded(fullns)) {
            result.appendBool("sharded", false);
            result.append("primary", conf->getPrimaryId().toString());

            return passthrough(txn, conf.get(), cmdObj, result);
        }

        result.appendBool("sharded", true);

        shared_ptr<ChunkManager> cm = conf->getChunkManager(txn, fullns);
        massert(12594, "how could chunk manager be null!", cm);

        BSONObjBuilder shardStats;
        map<string, long long> counts;
        map<string, long long> indexSizes;

        long long unscaledCollSize = 0;

        int nindexes = 0;
        bool warnedAboutIndexes = false;

        set<ShardId> shardIds;
        cm->getAllShardIds(&shardIds);
        for (const ShardId& shardId : shardIds) {
            const auto shard = grid.shardRegistry()->getShard(txn, shardId);
            if (!shard) {
                continue;
            }

            BSONObj res;
            {
                ScopedDbConnection conn(shard->getConnString());
                if (!conn->runCommand(dbName, cmdObj, res)) {
                    if (!res["code"].eoo()) {
                        result.append(res["code"]);
                    }
                    errmsg = "failed on shard: " + res.toString();
                    return false;
                }
                conn.done();
            }

            BSONObjIterator j(res);
            // We don't know the order that we will encounter the count and size
            // So we save them until we've iterated through all the fields before
            // updating unscaledCollSize.
            long long shardObjCount;
            long long shardAvgObjSize;
            while (j.more()) {
                BSONElement e = j.next();
                if (str::equals(e.fieldName(), "ns") || str::equals(e.fieldName(), "ok") ||
                    str::equals(e.fieldName(), "lastExtentSize") ||
                    str::equals(e.fieldName(), "paddingFactor")) {
                    continue;
                } else if (str::equals(e.fieldName(), "count") ||
                           str::equals(e.fieldName(), "size") ||
                           str::equals(e.fieldName(), "storageSize") ||
                           str::equals(e.fieldName(), "numExtents") ||
                           str::equals(e.fieldName(), "totalIndexSize")) {
                    counts[e.fieldName()] += e.numberLong();
                    if (str::equals(e.fieldName(), "count")) {
                        shardObjCount = e.numberLong();
                    }
                } else if (str::equals(e.fieldName(), "avgObjSize")) {
                    shardAvgObjSize = e.numberLong();
                } else if (str::equals(e.fieldName(), "indexSizes")) {
                    BSONObjIterator k(e.Obj());
                    while (k.more()) {
                        BSONElement temp = k.next();
                        indexSizes[temp.fieldName()] += temp.numberLong();
                    }
                }
                // no longer used since 2.2
                else if (str::equals(e.fieldName(), "flags")) {
                    if (!result.hasField(e.fieldName()))
                        result.append(e);
                }
                // flags broken out in 2.4+
                else if (str::equals(e.fieldName(), "systemFlags")) {
                    if (!result.hasField(e.fieldName()))
                        result.append(e);
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
                } else if (str::equals(e.fieldName(), "wiredTiger")) {
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

        result.append("ns", fullns);

        for (map<string, long long>::iterator i = counts.begin(); i != counts.end(); ++i)
            result.appendNumber(i->first, i->second);

        {
            BSONObjBuilder ib(result.subobjStart("indexSizes"));
            for (map<string, long long>::iterator i = indexSizes.begin(); i != indexSizes.end();
                 ++i)
                ib.appendNumber(i->first, i->second);
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
    virtual string parseNs(const string& dbname, const BSONObj& cmdObj) const override {
        return parseNsFullyQualified(dbname, cmdObj);
    }
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {
        ActionSet actions;
        actions.addAction(ActionType::find);
        out->push_back(Privilege(parseResourcePattern(dbname, cmdObj), actions));
    }
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }
    bool run(OperationContext* txn,
             const string& dbName,
             BSONObj& cmdObj,
             int,
             string& errmsg,
             BSONObjBuilder& result) {
        const string fullns = parseNs(dbName, cmdObj);
        const string nsDBName = nsToDatabase(fullns);

        auto conf = uassertStatusOK(grid.catalogCache()->getDatabase(txn, nsDBName));
        if (!conf->isShardingEnabled() || !conf->isSharded(fullns)) {
            return passthrough(txn, conf.get(), cmdObj, result);
        }

        shared_ptr<ChunkManager> cm = conf->getChunkManager(txn, fullns);
        massert(13407, "how could chunk manager be null!", cm);

        BSONObj min = cmdObj.getObjectField("min");
        BSONObj max = cmdObj.getObjectField("max");
        BSONObj keyPattern = cmdObj.getObjectField("keyPattern");

        uassert(13408,
                "keyPattern must equal shard key",
                cm->getShardKeyPattern().toBSON() == keyPattern);
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

        set<ShardId> shardIds;
        cm->getShardIdsForRange(shardIds, min, max);
        for (const ShardId& shardId : shardIds) {
            const auto shard = grid.shardRegistry()->getShard(txn, shardId);
            if (!shard) {
                continue;
            }

            ScopedDbConnection conn(shard->getConnString());
            BSONObj res;
            bool ok = conn->runCommand(conf->name(), cmdObj, res);
            conn.done();

            if (!ok) {
                result.appendElements(res);
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

} DataSizeCmd;

class ConvertToCappedCmd : public NotAllowedOnShardedCollectionCmd {
public:
    ConvertToCappedCmd() : NotAllowedOnShardedCollectionCmd("convertToCapped") {}
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {
        ActionSet actions;
        actions.addAction(ActionType::convertToCapped);
        out->push_back(Privilege(parseResourcePattern(dbname, cmdObj), actions));
    }

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

} convertToCappedCmd;

class GroupCmd : public NotAllowedOnShardedCollectionCmd {
public:
    GroupCmd() : NotAllowedOnShardedCollectionCmd("group") {}
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {
        ActionSet actions;
        actions.addAction(ActionType::find);
        out->push_back(Privilege(parseResourcePattern(dbname, cmdObj), actions));
    }

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    virtual bool passOptions() const {
        return true;
    }

    virtual std::string parseNs(const std::string& dbName, const BSONObj& cmdObj) const {
        return dbName + "." + cmdObj.firstElement().embeddedObjectUserCheck()["ns"].valuestrsafe();
    }

    Status explain(OperationContext* txn,
                   const std::string& dbname,
                   const BSONObj& cmdObj,
                   ExplainCommon::Verbosity verbosity,
                   const rpc::ServerSelectionMetadata& serverSelectionMetadata,
                   BSONObjBuilder* out) const {
        // We will time how long it takes to run the commands on the shards.
        Timer timer;

        BSONObj command;
        int options = 0;

        {
            BSONObjBuilder explainCmdBob;
            ClusterExplain::wrapAsExplain(
                cmdObj, verbosity, serverSelectionMetadata, &explainCmdBob, &options);
            command = explainCmdBob.obj();
        }

        const NamespaceString nss(parseNs(dbname, cmdObj));

        // Note that this implementation will not handle targeting retries and fails when the
        // sharding metadata is too stale
        auto status = grid.catalogCache()->getDatabase(txn, nss.db().toString());
        if (!status.isOK()) {
            return Status(status.getStatus().code(),
                          str::stream() << "Passthrough command failed: " << command.toString()
                                        << " on ns "
                                        << nss.ns()
                                        << ". Caused by "
                                        << causedBy(status.getStatus()));
        }

        shared_ptr<DBConfig> conf = status.getValue();
        if (conf->isSharded(nss.ns())) {
            return Status(ErrorCodes::IllegalOperation,
                          str::stream() << "Passthrough command failed: " << command.toString()
                                        << " on ns "
                                        << nss.ns()
                                        << ". Cannot run on sharded namespace.");
        }

        const auto primaryShard = grid.shardRegistry()->getShard(txn, conf->getPrimaryId());

        BSONObj shardResult;
        try {
            ShardConnection conn(primaryShard->getConnString(), "");

            // TODO: this can throw a stale config when mongos is not up-to-date -- fix.
            if (!conn->runCommand(nss.db().toString(), command, shardResult, options)) {
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
        cmdResult.shardTargetId = conf->getPrimaryId();
        cmdResult.result = shardResult;
        cmdResult.target = primaryShard->getConnString();

        return ClusterExplain::buildExplainResult(
            txn, {cmdResult}, ClusterExplain::kSingleShard, timer.millis(), out);
    }

} groupCmd;

class SplitVectorCmd : public NotAllowedOnShardedCollectionCmd {
public:
    SplitVectorCmd() : NotAllowedOnShardedCollectionCmd("splitVector") {}
    virtual bool passOptions() const {
        return true;
    }
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }
    virtual Status checkAuthForCommand(ClientBasic* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) {
        if (!AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
                ResourcePattern::forExactNamespace(NamespaceString(parseNs(dbname, cmdObj))),
                ActionType::splitVector)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }
        return Status::OK();
    }
    virtual bool run(OperationContext* txn,
                     const string& dbName,
                     BSONObj& cmdObj,
                     int options,
                     string& errmsg,
                     BSONObjBuilder& result) {
        string x = parseNs(dbName, cmdObj);
        if (!str::startsWith(x, dbName)) {
            errmsg = str::stream() << "doing a splitVector across dbs isn't supported via mongos";
            return false;
        }
        return NotAllowedOnShardedCollectionCmd::run(txn, dbName, cmdObj, options, errmsg, result);
    }
    virtual std::string parseNs(const string& dbname, const BSONObj& cmdObj) const {
        return parseNsFullyQualified(dbname, cmdObj);
    }

} splitVectorCmd;

class DistinctCmd : public PublicGridCommand {
public:
    DistinctCmd() : PublicGridCommand("distinct") {}
    virtual void help(stringstream& help) const {
        help << "{ distinct : 'collection name' , key : 'a.b' , query : {} }";
    }
    virtual bool passOptions() const {
        return true;
    }
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {
        ActionSet actions;
        actions.addAction(ActionType::find);
        out->push_back(Privilege(parseResourcePattern(dbname, cmdObj), actions));
    }
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    bool run(OperationContext* txn,
             const string& dbName,
             BSONObj& cmdObj,
             int options,
             string& errmsg,
             BSONObjBuilder& result) {
        const string fullns = parseNs(dbName, cmdObj);

        auto status = grid.catalogCache()->getDatabase(txn, dbName);
        if (!status.isOK()) {
            return appendEmptyResultSet(result, status.getStatus(), fullns);
        }

        shared_ptr<DBConfig> conf = status.getValue();
        if (!conf->isShardingEnabled() || !conf->isSharded(fullns)) {
            return passthrough(txn, conf.get(), cmdObj, options, result);
        }

        shared_ptr<ChunkManager> cm = conf->getChunkManager(txn, fullns);
        massert(10420, "how could chunk manager be null!", cm);

        BSONObj query = getQuery(cmdObj);
        set<ShardId> shardIds;
        cm->getShardIdsForQuery(txn, query, &shardIds);

        set<BSONObj, BSONObjCmp> all;
        int size = 32;

        for (const ShardId& shardId : shardIds) {
            const auto shard = grid.shardRegistry()->getShard(txn, shardId);
            if (!shard) {
                continue;
            }

            ShardConnection conn(shard->getConnString(), fullns);
            BSONObj res;
            bool ok = conn->runCommand(conf->name(), cmdObj, res, options);
            conn.done();

            if (!ok) {
                result.appendElements(res);
                return false;
            }

            BSONObjIterator it(res["values"].embeddedObject());
            while (it.more()) {
                BSONElement nxt = it.next();
                BSONObjBuilder temp(32);
                temp.appendAs(nxt, "");
                all.insert(temp.obj());
            }
        }

        BSONObjBuilder b(size);
        int n = 0;
        for (set<BSONObj, BSONObjCmp>::iterator i = all.begin(); i != all.end(); i++) {
            b.appendAs(i->firstElement(), b.numStr(n++));
        }

        result.appendArray("values", b.obj());
        return true;
    }

    Status explain(OperationContext* txn,
                   const std::string& dbname,
                   const BSONObj& cmdObj,
                   ExplainCommon::Verbosity verbosity,
                   const rpc::ServerSelectionMetadata& serverSelectionMetadata,
                   BSONObjBuilder* out) const {
        const string fullns = parseNs(dbname, cmdObj);

        // Extract the targeting query.
        BSONObj targetingQuery;
        if (BSONElement queryElt = cmdObj["query"]) {
            if (queryElt.type() == BSONType::Object) {
                targetingQuery = queryElt.embeddedObject();
            } else if (queryElt.type() != BSONType::jstNULL) {
                return Status(ErrorCodes::TypeMismatch,
                              str::stream() << "\"query\" had the wrong type. Expected "
                                            << typeName(BSONType::Object)
                                            << " or "
                                            << typeName(BSONType::jstNULL)
                                            << ", found "
                                            << typeName(queryElt.type()));
            }
        }

        BSONObjBuilder explainCmdBob;
        int options = 0;
        ClusterExplain::wrapAsExplain(
            cmdObj, verbosity, serverSelectionMetadata, &explainCmdBob, &options);

        // We will time how long it takes to run the commands on the shards.
        Timer timer;

        vector<Strategy::CommandResult> shardResults;
        Strategy::commandOp(
            txn, dbname, explainCmdBob.obj(), options, fullns, targetingQuery, &shardResults);

        long long millisElapsed = timer.millis();

        const char* mongosStageName = ClusterExplain::getStageNameForReadOp(shardResults, cmdObj);

        return ClusterExplain::buildExplainResult(
            txn, shardResults, mongosStageName, millisElapsed, out);
    }
} disinctCmd;

class FileMD5Cmd : public PublicGridCommand {
public:
    FileMD5Cmd() : PublicGridCommand("filemd5") {}
    virtual void help(stringstream& help) const {
        help << " example: { filemd5 : ObjectId(aaaaaaa) , root : \"fs\" }";
    }

    virtual std::string parseNs(const std::string& dbname, const BSONObj& cmdObj) const {
        std::string collectionName = cmdObj.getStringField("root");
        if (collectionName.empty())
            collectionName = "fs";
        collectionName += ".chunks";
        return NamespaceString(dbname, collectionName).ns();
    }

    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {
        out->push_back(Privilege(parseResourcePattern(dbname, cmdObj), ActionType::find));
    }

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    bool run(OperationContext* txn,
             const string& dbName,
             BSONObj& cmdObj,
             int,
             string& errmsg,
             BSONObjBuilder& result) {
        const string fullns = parseNs(dbName, cmdObj);

        auto conf = uassertStatusOK(grid.catalogCache()->getDatabase(txn, dbName));
        if (!conf->isShardingEnabled() || !conf->isSharded(fullns)) {
            return passthrough(txn, conf.get(), cmdObj, result);
        }

        shared_ptr<ChunkManager> cm = conf->getChunkManager(txn, fullns);
        massert(13091, "how could chunk manager be null!", cm);
        if (cm->getShardKeyPattern().toBSON() == BSON("files_id" << 1)) {
            BSONObj finder = BSON("files_id" << cmdObj.firstElement());

            vector<Strategy::CommandResult> results;
            Strategy::commandOp(txn, dbName, cmdObj, 0, fullns, finder, &results);
            verify(results.size() == 1);  // querying on shard key so should only talk to one shard
            BSONObj res = results.begin()->result;

            result.appendElements(res);
            return res["ok"].trueValue();
        } else if (cm->getShardKeyPattern().toBSON() == BSON("files_id" << 1 << "n" << 1)) {
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
                BSONObjBuilder bb;
                bb.appendElements(cmdObj);
                bb.appendBool("partialOk", true);
                bb.append("startAt", n);
                if (!lastResult.isEmpty()) {
                    bb.append(lastResult["md5state"]);
                }
                BSONObj shardCmd = bb.obj();

                BSONObj finder = BSON("files_id" << cmdObj.firstElement() << "n" << n);

                vector<Strategy::CommandResult> results;
                try {
                    Strategy::commandOp(txn, dbName, shardCmd, 0, fullns, finder, &results);
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

                    log() << "Sharded filemd5 failed: " << result.asTempObj();

                    errmsg =
                        string("sharded filemd5 failed because: ") + res["errmsg"].valuestrsafe();
                    return false;
                }

                uassert(16246,
                        "Shard " + conf->name() +
                            " is too old to support GridFS sharded by {files_id:1, n:1}",
                        res.hasField("md5state"));

                lastResult = res;
                int nNext = res["numChunks"].numberInt();

                if (n == nNext) {
                    // no new data means we've reached the end of the file
                    result.appendElements(res);
                    return true;
                }

                verify(nNext > n);
                n = nNext;
            }

            verify(0);
        }

        // We could support arbitrary shard keys by sending commands to all shards but I don't think
        // we should
        errmsg =
            "GridFS fs.chunks collection must be sharded on either {files_id:1} or {files_id:1, "
            "n:1}";
        return false;
    }
} fileMD5Cmd;

class Geo2dFindNearCmd : public PublicGridCommand {
public:
    Geo2dFindNearCmd() : PublicGridCommand("geoNear") {}
    void help(stringstream& h) const {
        h << "http://dochub.mongodb.org/core/geo#GeospatialIndexing-geoNearCommand";
    }
    virtual bool passOptions() const {
        return true;
    }
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {
        ActionSet actions;
        actions.addAction(ActionType::find);
        out->push_back(Privilege(parseResourcePattern(dbname, cmdObj), actions));
    }
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    bool run(OperationContext* txn,
             const string& dbName,
             BSONObj& cmdObj,
             int options,
             string& errmsg,
             BSONObjBuilder& result) {
        const string fullns = parseNs(dbName, cmdObj);

        auto conf = uassertStatusOK(grid.catalogCache()->getDatabase(txn, dbName));
        if (!conf->isShardingEnabled() || !conf->isSharded(fullns)) {
            return passthrough(txn, conf.get(), cmdObj, options, result);
        }

        shared_ptr<ChunkManager> cm = conf->getChunkManager(txn, fullns);
        massert(13500, "how could chunk manager be null!", cm);

        BSONObj query = getQuery(cmdObj);
        set<ShardId> shardIds;
        cm->getShardIdsForQuery(txn, query, &shardIds);

        // We support both "num" and "limit" options to control limit
        int limit = 100;
        const char* limitName = cmdObj["num"].isNumber() ? "num" : "limit";
        if (cmdObj[limitName].isNumber())
            limit = cmdObj[limitName].numberInt();

        list<shared_ptr<Future::CommandResult>> futures;
        BSONArrayBuilder shardArray;
        for (const ShardId& shardId : shardIds) {
            const auto shard = grid.shardRegistry()->getShard(txn, shardId);
            if (!shard) {
                continue;
            }

            futures.push_back(
                Future::spawnCommand(shard->getConnString().toString(), dbName, cmdObj, options));
            shardArray.append(shardId.toString());
        }

        multimap<double, BSONObj> results;  // TODO: maybe use merge-sort instead
        string nearStr;
        double time = 0;
        double btreelocs = 0;
        double nscanned = 0;
        double objectsLoaded = 0;
        for (list<shared_ptr<Future::CommandResult>>::iterator i = futures.begin();
             i != futures.end();
             i++) {
            shared_ptr<Future::CommandResult> res = *i;
            if (!res->join(txn)) {
                errmsg = res->result()["errmsg"].String();
                if (res->result().hasField("code")) {
                    result.append(res->result()["code"]);
                }
                return false;
            }

            if (res->result().hasField("near")) {
                nearStr = res->result()["near"].String();
            }
            time += res->result()["stats"]["time"].Number();
            if (!res->result()["stats"]["btreelocs"].eoo()) {
                btreelocs += res->result()["stats"]["btreelocs"].Number();
            }
            nscanned += res->result()["stats"]["nscanned"].Number();
            if (!res->result()["stats"]["objectsLoaded"].eoo()) {
                objectsLoaded += res->result()["stats"]["objectsLoaded"].Number();
            }

            BSONForEach(obj, res->result()["results"].embeddedObject()) {
                results.insert(make_pair(obj["dis"].Number(), obj.embeddedObject().getOwned()));
            }

            // TODO: maybe shrink results if size() > limit
        }

        result.append("ns", fullns);
        result.append("near", nearStr);

        int outCount = 0;
        double totalDistance = 0;
        double maxDistance = 0;
        {
            BSONArrayBuilder sub(result.subarrayStart("results"));
            for (multimap<double, BSONObj>::const_iterator it(results.begin()), end(results.end());
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

class ApplyOpsCmd : public PublicGridCommand {
public:
    ApplyOpsCmd() : PublicGridCommand("applyOps") {}
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {
        // applyOps can do pretty much anything, so require all privileges.
        RoleGraph::generateUniversalPrivileges(out);
    }
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }
    virtual bool run(OperationContext* txn,
                     const string& dbName,
                     BSONObj& cmdObj,
                     int,
                     string& errmsg,
                     BSONObjBuilder& result) {
        errmsg = "applyOps not allowed through mongos";
        return false;
    }
} applyOpsCmd;

class CompactCmd : public PublicGridCommand {
public:
    CompactCmd() : PublicGridCommand("compact") {}
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {
        ActionSet actions;
        actions.addAction(ActionType::compact);
        out->push_back(Privilege(parseResourcePattern(dbname, cmdObj), actions));
    }
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }
    virtual bool run(OperationContext* txn,
                     const string& dbName,
                     BSONObj& cmdObj,
                     int,
                     string& errmsg,
                     BSONObjBuilder& result) {
        errmsg = "compact not allowed through mongos";
        return false;
    }
} compactCmd;

class EvalCmd : public PublicGridCommand {
public:
    EvalCmd() : PublicGridCommand("eval", "$eval") {}
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {
        // $eval can do pretty much anything, so require all privileges.
        RoleGraph::generateUniversalPrivileges(out);
    }
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }
    virtual bool run(OperationContext* txn,
                     const string& dbName,
                     BSONObj& cmdObj,
                     int,
                     string& errmsg,
                     BSONObjBuilder& result) {
        RARELY {
            warning() << "the eval command is deprecated" << startupWarningsLog;
        }

        // $eval isn't allowed to access sharded collections, but we need to leave the
        // shard to detect that.
        auto status = grid.catalogCache()->getDatabase(txn, dbName);
        if (!status.isOK()) {
            return appendCommandStatus(result, status.getStatus());
        }

        shared_ptr<DBConfig> conf = status.getValue();
        return passthrough(txn, conf.get(), cmdObj, result);
    }
} evalCmd;

class CmdListCollections final : public PublicGridCommand {
public:
    CmdListCollections() : PublicGridCommand("listCollections") {}

    Status checkAuthForCommand(ClientBasic* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) final {
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
                      str::stream() << "Not authorized to create users on db: " << dbname);
    }

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    bool run(OperationContext* txn,
             const string& dbName,
             BSONObj& cmdObj,
             int options,
             string& errmsg,
             BSONObjBuilder& result) final {
        auto conf = grid.catalogCache()->getDatabase(txn, dbName);
        if (!conf.isOK()) {
            return appendEmptyResultSet(result, conf.getStatus(), dbName + ".$cmd.listCollections");
        }

        return cursorCommandPassthrough(txn, conf.getValue(), cmdObj, options, &result);
    }
} cmdListCollections;

class CmdListIndexes final : public PublicGridCommand {
public:
    CmdListIndexes() : PublicGridCommand("listIndexes") {}
    virtual Status checkAuthForCommand(ClientBasic* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) {
        AuthorizationSession* authzSession = AuthorizationSession::get(client);

        // Check for the listIndexes ActionType on the database, or find on system.indexes for pre
        // 3.0 systems.
        NamespaceString ns(parseNs(dbname, cmdObj));
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

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    bool run(OperationContext* txn,
             const string& dbName,
             BSONObj& cmdObj,
             int options,
             string& errmsg,
             BSONObjBuilder& result) final {
        auto conf = grid.catalogCache()->getDatabase(txn, dbName);
        if (!conf.isOK()) {
            return appendCommandStatus(result, conf.getStatus());
        }

        return cursorCommandPassthrough(txn, conf.getValue(), cmdObj, options, &result);
    }

} cmdListIndexes;

class AvailableQueryOptions : public Command {
public:
    AvailableQueryOptions() : Command("availableQueryOptions", false, "availablequeryoptions") {}

    virtual bool slaveOk() const {
        return true;
    }
    virtual Status checkAuthForCommand(ClientBasic* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) {
        return Status::OK();
    }
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    virtual bool run(OperationContext* txn,
                     const string& dbname,
                     BSONObj& cmdObj,
                     int,
                     string& errmsg,
                     BSONObjBuilder& result) {
        result << "options" << QueryOption_AllSupportedForSharding;
        return true;
    }

} availableQueryOptionsCmd;

}  // namespace
}  // namespace mongo
