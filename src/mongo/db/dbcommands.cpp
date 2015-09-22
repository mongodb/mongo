/**
 *    Copyright (C) 2012-2015 MongoDB Inc.
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

#include <boost/optional.hpp>
#include <time.h>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/audit.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/impersonation_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/user_management_commands_parser.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/background.h"
#include "mongo/db/catalog/coll_mod.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog_entry.h"
#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/catalog/drop_collection.h"
#include "mongo/db/catalog/drop_database.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/commands/shutdown.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/index_builder.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/instance.h"
#include "mongo/db/introspect.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/lasterror.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/ops/insert.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/repair_database.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/read_concern_response.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/s/operation_shard_version.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/write_concern.h"
#include "mongo/rpc/request_interface.h"
#include "mongo/rpc/reply_builder_interface.h"
#include "mongo/rpc/metadata.h"
#include "mongo/rpc/metadata/config_server_request_metadata.h"
#include "mongo/rpc/metadata/config_server_response_metadata.h"
#include "mongo/rpc/metadata/server_selection_metadata.h"
#include "mongo/rpc/metadata/sharding_metadata.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/stale_exception.h"  // for SendStaleConfigException
#include "mongo/scripting/engine.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/md5.hpp"
#include "mongo/util/print.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

using std::endl;
using std::ostringstream;
using std::string;
using std::stringstream;
using std::unique_ptr;

// This is a special flag that allows for testing of snapshot behavior by skipping the replication
// related checks and isolating the storage/query side of snapshotting.
bool testingSnapshotBehaviorInIsolation = false;
ExportedServerParameter<bool, ServerParameterType::kStartupOnly> TestingSnapshotBehaviorInIsolation(
    ServerParameterSet::getGlobal(),
    "testingSnapshotBehaviorInIsolation",
    &testingSnapshotBehaviorInIsolation);


class CmdShutdownMongoD : public CmdShutdown {
public:
    virtual void help(stringstream& help) const {
        help << "shutdown the database.  must be ran against admin db and "
             << "either (1) ran from localhost or (2) authenticated. If "
             << "this is a primary in a replica set and there is no member "
             << "within 10 seconds of its optime, it will not shutdown "
             << "without force : true.  You can also specify timeoutSecs : "
             << "N to wait N seconds for other members to catch up.";
    }

    virtual bool run(OperationContext* txn,
                     const string& dbname,
                     BSONObj& cmdObj,
                     int options,
                     string& errmsg,
                     BSONObjBuilder& result) {
        bool force = cmdObj.hasField("force") && cmdObj["force"].trueValue();

        long long timeoutSecs = 0;
        if (cmdObj.hasField("timeoutSecs")) {
            timeoutSecs = cmdObj["timeoutSecs"].numberLong();
        }

        Status status = repl::getGlobalReplicationCoordinator()->stepDown(
            txn, force, Seconds(timeoutSecs), Seconds(120));
        if (!status.isOK() && status.code() != ErrorCodes::NotMaster) {  // ignore not master
            return appendCommandStatus(result, status);
        }

        // Never returns
        shutdownHelper();
        return true;
    }

} cmdShutdownMongoD;

class CmdDropDatabase : public Command {
public:
    virtual void help(stringstream& help) const {
        help << "drop (delete) this database";
    }
    virtual bool slaveOk() const {
        return false;
    }

    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {
        ActionSet actions;
        actions.addAction(ActionType::dropDatabase);
        out->push_back(Privilege(ResourcePattern::forDatabaseName(dbname), actions));
    }

    virtual bool isWriteCommandForConfigServer() const {
        return true;
    }

    CmdDropDatabase() : Command("dropDatabase") {}

    bool run(OperationContext* txn,
             const string& dbname,
             BSONObj& cmdObj,
             int,
             string& errmsg,
             BSONObjBuilder& result) {
        // disallow dropping the config database
        if (serverGlobalParams.configsvr && (dbname == "config")) {
            return appendCommandStatus(result,
                                       Status(ErrorCodes::IllegalOperation,
                                              "Cannot drop 'config' database if mongod started "
                                              "with --configsvr"));
        }

        if ((repl::getGlobalReplicationCoordinator()->getReplicationMode() !=
             repl::ReplicationCoordinator::modeNone) &&
            (dbname == "local")) {
            return appendCommandStatus(result,
                                       Status(ErrorCodes::IllegalOperation,
                                              "Cannot drop 'local' database while replication "
                                              "is active"));
        }
        BSONElement e = cmdObj.firstElement();
        int p = (int)e.number();
        if (p != 1) {
            return appendCommandStatus(
                result, Status(ErrorCodes::IllegalOperation, "have to pass 1 as db parameter"));
        }

        Status status = dropDatabase(txn, dbname);
        if (status == ErrorCodes::NamespaceNotFound) {
            return appendCommandStatus(result, Status::OK());
        }
        if (status.isOK()) {
            result.append("dropped", dbname);
        }
        return appendCommandStatus(result, status);
    }

} cmdDropDatabase;

class CmdRepairDatabase : public Command {
public:
    virtual bool slaveOk() const {
        return true;
    }
    virtual bool maintenanceMode() const {
        return true;
    }
    virtual void help(stringstream& help) const {
        help << "repair database.  also compacts. note: slow.";
    }

    virtual bool isWriteCommandForConfigServer() const {
        return true;
    }

    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {
        ActionSet actions;
        actions.addAction(ActionType::repairDatabase);
        out->push_back(Privilege(ResourcePattern::forDatabaseName(dbname), actions));
    }

    CmdRepairDatabase() : Command("repairDatabase") {}

    bool run(OperationContext* txn,
             const string& dbname,
             BSONObj& cmdObj,
             int,
             string& errmsg,
             BSONObjBuilder& result) {
        BSONElement e = cmdObj.firstElement();
        if (e.numberInt() != 1) {
            errmsg = "bad option";
            return false;
        }

        // TODO: SERVER-4328 Don't lock globally
        ScopedTransaction transaction(txn, MODE_X);
        Lock::GlobalWrite lk(txn->lockState());
        OldClientContext context(txn, dbname);

        log() << "repairDatabase " << dbname;
        BackgroundOperation::assertNoBgOpInProgForDb(dbname);

        e = cmdObj.getField("preserveClonedFilesOnFailure");
        bool preserveClonedFilesOnFailure = e.isBoolean() && e.boolean();
        e = cmdObj.getField("backupOriginalFiles");
        bool backupOriginalFiles = e.isBoolean() && e.boolean();

        StorageEngine* engine = getGlobalServiceContext()->getGlobalStorageEngine();
        bool shouldReplicateWrites = txn->writesAreReplicated();
        txn->setReplicatedWrites(false);
        ON_BLOCK_EXIT(&OperationContext::setReplicatedWrites, txn, shouldReplicateWrites);
        Status status =
            repairDatabase(txn, engine, dbname, preserveClonedFilesOnFailure, backupOriginalFiles);

        // Open database before returning
        dbHolder().openDb(txn, dbname);
        return appendCommandStatus(result, status);
    }
} cmdRepairDatabase;

/* set db profiling level
   todo: how do we handle profiling information put in the db with replication?
         sensibly or not?
*/
class CmdProfile : public Command {
public:
    virtual bool slaveOk() const {
        return true;
    }

    virtual void help(stringstream& help) const {
        help << "enable or disable performance profiling\n";
        help << "{ profile : <n> }\n";
        help << "0=off 1=log slow ops 2=log all\n";
        help << "-1 to get current values\n";
        help << "http://docs.mongodb.org/manual/reference/command/profile/#dbcmd.profile";
    }

    virtual bool isWriteCommandForConfigServer() const {
        return true;
    }

    virtual Status checkAuthForCommand(ClientBasic* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) {
        AuthorizationSession* authzSession = AuthorizationSession::get(client);

        if (cmdObj.firstElement().numberInt() == -1 && !cmdObj.hasField("slowms")) {
            // If you just want to get the current profiling level you can do so with just
            // read access to system.profile, even if you can't change the profiling level.
            if (authzSession->isAuthorizedForActionsOnResource(
                    ResourcePattern::forExactNamespace(NamespaceString(dbname, "system.profile")),
                    ActionType::find)) {
                return Status::OK();
            }
        }

        if (authzSession->isAuthorizedForActionsOnResource(ResourcePattern::forDatabaseName(dbname),
                                                           ActionType::enableProfiler)) {
            return Status::OK();
        }

        return Status(ErrorCodes::Unauthorized, "unauthorized");
    }

    CmdProfile() : Command("profile") {}

    bool run(OperationContext* txn,
             const string& dbname,
             BSONObj& cmdObj,
             int options,
             string& errmsg,
             BSONObjBuilder& result) {
        // Needs to be locked exclusively, because creates the system.profile collection
        // in the local database.
        ScopedTransaction transaction(txn, MODE_IX);
        Lock::DBLock dbXLock(txn->lockState(), dbname, MODE_X);
        OldClientContext ctx(txn, dbname);

        BSONElement e = cmdObj.firstElement();
        result.append("was", ctx.db()->getProfilingLevel());
        result.append("slowms", serverGlobalParams.slowMS);

        int p = (int)e.number();
        Status status = Status::OK();

        if (p == -1)
            status = Status::OK();
        else if (p >= 0 && p <= 2) {
            status = ctx.db()->setProfilingLevel(txn, p);
        }

        const BSONElement slow = cmdObj["slowms"];
        if (slow.isNumber()) {
            serverGlobalParams.slowMS = slow.numberInt();
        }

        if (!status.isOK()) {
            errmsg = status.reason();
        }

        return status.isOK();
    }

} cmdProfile;

class CmdDiagLogging : public Command {
public:
    virtual bool slaveOk() const {
        return true;
    }
    CmdDiagLogging() : Command("diagLogging") {}
    bool adminOnly() const {
        return true;
    }

    void help(stringstream& h) const {
        h << "http://dochub.mongodb.org/core/"
             "monitoring#MonitoringandDiagnostics-DatabaseRecord%2FReplay%28diagLoggingcommand%29";
    }

    virtual bool isWriteCommandForConfigServer() const {
        return true;
    }

    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {
        ActionSet actions;
        actions.addAction(ActionType::diagLogging);
        out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
    }

    bool run(OperationContext* txn,
             const string& dbname,
             BSONObj& cmdObj,
             int,
             string& errmsg,
             BSONObjBuilder& result) {
        const char* deprecationWarning =
            "CMD diagLogging is deprecated and will be removed in a future release";
        warning() << deprecationWarning << startupWarningsLog;

        // This doesn't look like it requires exclusive DB lock, because it uses its own diag
        // locking, but originally the lock was set to be WRITE, so preserving the behaviour.
        //
        ScopedTransaction transaction(txn, MODE_IX);
        Lock::DBLock dbXLock(txn->lockState(), dbname, MODE_X);
        OldClientContext ctx(txn, dbname);

        int was = _diaglog.setLevel(cmdObj.firstElement().numberInt());
        _diaglog.flush();
        if (!serverGlobalParams.quiet) {
            LOG(0) << "CMD: diagLogging set to " << _diaglog.getLevel() << " from: " << was << endl;
        }
        result.append("was", was);
        result.append("note", deprecationWarning);
        return true;
    }
} cmddiaglogging;

/* drop collection */
class CmdDrop : public Command {
public:
    CmdDrop() : Command("drop") {}
    virtual bool slaveOk() const {
        return false;
    }
    virtual bool adminOnly() const {
        return false;
    }
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {
        ActionSet actions;
        actions.addAction(ActionType::dropCollection);
        out->push_back(Privilege(parseResourcePattern(dbname, cmdObj), actions));
    }
    virtual void help(stringstream& help) const {
        help << "drop a collection\n{drop : <collectionName>}";
    }

    virtual bool isWriteCommandForConfigServer() const {
        return true;
    }

    virtual bool run(OperationContext* txn,
                     const string& dbname,
                     BSONObj& cmdObj,
                     int,
                     string& errmsg,
                     BSONObjBuilder& result) {
        const std::string nsToDrop = parseNsCollectionRequired(dbname, cmdObj);

        if (nsToDrop.find('$') != string::npos) {
            errmsg = "can't drop collection with reserved $ character in name";
            return false;
        }

        if ((repl::getGlobalReplicationCoordinator()->getReplicationMode() !=
             repl::ReplicationCoordinator::modeNone) &&
            NamespaceString(nsToDrop).isOplog()) {
            errmsg = "can't drop live oplog while replicating";
            return false;
        }

        return appendCommandStatus(result, dropCollection(txn, NamespaceString(nsToDrop), result));
    }

} cmdDrop;

/* create collection */
class CmdCreate : public Command {
public:
    CmdCreate() : Command("create") {}
    virtual bool slaveOk() const {
        return false;
    }
    virtual bool adminOnly() const {
        return false;
    }

    virtual bool isWriteCommandForConfigServer() const {
        return true;
    }

    virtual void help(stringstream& help) const {
        help << "create a collection explicitly\n"
                "{ create: <ns>[, capped: <bool>, size: <collSizeInBytes>, max: <nDocs>] }";
    }
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
    virtual bool run(OperationContext* txn,
                     const string& dbname,
                     BSONObj& cmdObj,
                     int,
                     string& errmsg,
                     BSONObjBuilder& result) {
        if (cmdObj.hasField("autoIndexId")) {
            const char* deprecationWarning =
                "the autoIndexId option is deprecated and will be removed in a future release";
            warning() << deprecationWarning;
            result.append("note", deprecationWarning);
        }
        return appendCommandStatus(result, createCollection(txn, dbname, cmdObj));
    }
} cmdCreate;


class CmdFileMD5 : public Command {
public:
    CmdFileMD5() : Command("filemd5") {}

    virtual bool slaveOk() const {
        return true;
    }

    virtual void help(stringstream& help) const {
        help << " example: { filemd5 : ObjectId(aaaaaaa) , root : \"fs\" }";
    }

    virtual bool isWriteCommandForConfigServer() const {
        return false;
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

    bool run(OperationContext* txn,
             const string& dbname,
             BSONObj& jsobj,
             int,
             string& errmsg,
             BSONObjBuilder& result) {
        const std::string ns = parseNs(dbname, jsobj);

        md5digest d;
        md5_state_t st;
        md5_init(&st);

        int n = 0;

        bool partialOk = jsobj["partialOk"].trueValue();
        if (partialOk) {
            // WARNING: This code depends on the binary layout of md5_state. It will not be
            // compatible with different md5 libraries or work correctly in an environment with
            // mongod's of different endians. It is ok for mongos to be a different endian since
            // it just passes the buffer through to another mongod.
            BSONElement stateElem = jsobj["md5state"];
            if (!stateElem.eoo()) {
                int len;
                const char* data = stateElem.binDataClean(len);
                massert(16247, "md5 state not correct size", len == sizeof(st));
                memcpy(&st, data, sizeof(st));
            }
            n = jsobj["startAt"].numberInt();
        }

        BSONObj query = BSON("files_id" << jsobj["filemd5"] << "n" << GTE << n);
        BSONObj sort = BSON("files_id" << 1 << "n" << 1);

        MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
            auto statusWithCQ =
                CanonicalQuery::canonicalize(NamespaceString(ns), query, sort, BSONObj());
            if (!statusWithCQ.isOK()) {
                uasserted(17240, "Can't canonicalize query " + query.toString());
                return 0;
            }
            unique_ptr<CanonicalQuery> cq = std::move(statusWithCQ.getValue());

            // Check shard version at startup.
            // This will throw before we've done any work if shard version is outdated
            // We drop and re-acquire these locks every document because md5'ing is expensive
            unique_ptr<AutoGetCollectionForRead> ctx(new AutoGetCollectionForRead(txn, ns));
            Collection* coll = ctx->getCollection();

            auto statusWithPlanExecutor = getExecutor(txn,
                                                      coll,
                                                      std::move(cq),
                                                      PlanExecutor::YIELD_MANUAL,
                                                      QueryPlannerParams::NO_TABLE_SCAN);
            if (!statusWithPlanExecutor.isOK()) {
                uasserted(17241, "Can't get executor for query " + query.toString());
                return 0;
            }

            unique_ptr<PlanExecutor> exec = std::move(statusWithPlanExecutor.getValue());
            // Process notifications when the lock is released/reacquired in the loop below
            exec->registerExec();

            BSONObj obj;
            PlanExecutor::ExecState state;
            while (PlanExecutor::ADVANCED == (state = exec->getNext(&obj, NULL))) {
                BSONElement ne = obj["n"];
                verify(ne.isNumber());
                int myn = ne.numberInt();
                if (n != myn) {
                    if (partialOk) {
                        break;  // skipped chunk is probably on another shard
                    }
                    log() << "should have chunk: " << n << " have:" << myn << endl;
                    dumpChunks(txn, ns, query, sort);
                    uassert(10040, "chunks out of order", n == myn);
                }

                // make a copy of obj since we access data in it while yielding locks
                BSONObj owned = obj.getOwned();
                exec->saveState();
                // UNLOCKED
                ctx.reset();

                int len;
                const char* data = owned["data"].binDataClean(len);
                // This is potentially an expensive operation, so do it out of the lock
                md5_append(&st, (const md5_byte_t*)(data), len);
                n++;

                try {
                    // RELOCKED
                    ctx.reset(new AutoGetCollectionForRead(txn, ns));
                } catch (const SendStaleConfigException& ex) {
                    LOG(1) << "chunk metadata changed during filemd5, will retarget and continue";
                    break;
                }

                // Have the lock again. See if we were killed.
                if (!exec->restoreState()) {
                    if (!partialOk) {
                        uasserted(13281, "File deleted during filemd5 command");
                    }
                }
            }

            if (partialOk)
                result.appendBinData("md5state", sizeof(st), BinDataGeneral, &st);

            // This must be *after* the capture of md5state since it mutates st
            md5_finish(&st, d);

            result.append("numChunks", n);
            result.append("md5", digestToString(d));
        }
        MONGO_WRITE_CONFLICT_RETRY_LOOP_END(txn, "filemd5", dbname);
        return true;
    }

    void dumpChunks(OperationContext* txn,
                    const string& ns,
                    const BSONObj& query,
                    const BSONObj& sort) {
        DBDirectClient client(txn);
        Query q(query);
        q.sort(sort);
        unique_ptr<DBClientCursor> c = client.query(ns, q);
        while (c->more())
            PRINT(c->nextSafe());
    }

} cmdFileMD5;


class CmdDatasize : public Command {
    virtual string parseNs(const string& dbname, const BSONObj& cmdObj) const {
        return parseNsFullyQualified(dbname, cmdObj);
    }

public:
    CmdDatasize() : Command("dataSize", false, "datasize") {}

    virtual bool slaveOk() const {
        return true;
    }
    virtual bool isWriteCommandForConfigServer() const {
        return false;
    }
    virtual void help(stringstream& help) const {
        help << "determine data size for a set of data in a certain range"
                "\nexample: { dataSize:\"blog.posts\", keyPattern:{x:1}, min:{x:10}, max:{x:55} }"
                "\nmin and max parameters are optional. They must either both be included or both "
                "omitted"
                "\nkeyPattern is an optional parameter indicating an index pattern that would be "
                "useful"
                "for iterating over the min/max bounds. If keyPattern is omitted, it is inferred "
                "from "
                "the structure of min. "
                "\nnote: This command may take a while to run";
    }

    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {
        ActionSet actions;
        actions.addAction(ActionType::find);
        out->push_back(Privilege(parseResourcePattern(dbname, cmdObj), actions));
    }

    bool run(OperationContext* txn,
             const string& dbname,
             BSONObj& jsobj,
             int,
             string& errmsg,
             BSONObjBuilder& result) {
        Timer timer;

        string ns = jsobj.firstElement().String();
        BSONObj min = jsobj.getObjectField("min");
        BSONObj max = jsobj.getObjectField("max");
        BSONObj keyPattern = jsobj.getObjectField("keyPattern");
        bool estimate = jsobj["estimate"].trueValue();

        AutoGetCollectionForRead ctx(txn, ns);

        Collection* collection = ctx.getCollection();

        if (!collection || collection->numRecords(txn) == 0) {
            result.appendNumber("size", 0);
            result.appendNumber("numObjects", 0);
            result.append("millis", timer.millis());
            return true;
        }

        result.appendBool("estimate", estimate);

        unique_ptr<PlanExecutor> exec;
        if (min.isEmpty() && max.isEmpty()) {
            if (estimate) {
                result.appendNumber("size", static_cast<long long>(collection->dataSize(txn)));
                result.appendNumber("numObjects",
                                    static_cast<long long>(collection->numRecords(txn)));
                result.append("millis", timer.millis());
                return 1;
            }
            exec = InternalPlanner::collectionScan(txn, ns, collection, PlanExecutor::YIELD_MANUAL);
        } else if (min.isEmpty() || max.isEmpty()) {
            errmsg = "only one of min or max specified";
            return false;
        } else {
            if (keyPattern.isEmpty()) {
                // if keyPattern not provided, try to infer it from the fields in 'min'
                keyPattern = Helpers::inferKeyPattern(min);
            }

            IndexDescriptor* idx =
                collection->getIndexCatalog()->findShardKeyPrefixedIndex(txn,
                                                                         keyPattern,
                                                                         true);  // requireSingleKey

            if (idx == NULL) {
                errmsg = "couldn't find valid index containing key pattern";
                return false;
            }
            // If both min and max non-empty, append MinKey's to make them fit chosen index
            KeyPattern kp(idx->keyPattern());
            min = Helpers::toKeyFormat(kp.extendRangeBound(min, false));
            max = Helpers::toKeyFormat(kp.extendRangeBound(max, false));

            exec = InternalPlanner::indexScan(txn,
                                              collection,
                                              idx,
                                              min,
                                              max,
                                              false,  // endKeyInclusive
                                              PlanExecutor::YIELD_MANUAL);
        }

        long long avgObjSize = collection->dataSize(txn) / collection->numRecords(txn);

        long long maxSize = jsobj["maxSize"].numberLong();
        long long maxObjects = jsobj["maxObjects"].numberLong();

        long long size = 0;
        long long numObjects = 0;

        RecordId loc;
        PlanExecutor::ExecState state;
        while (PlanExecutor::ADVANCED == (state = exec->getNext(NULL, &loc))) {
            if (estimate)
                size += avgObjSize;
            else
                size += collection->getRecordStore()->dataFor(txn, loc).size();

            numObjects++;

            if ((maxSize && size > maxSize) || (maxObjects && numObjects > maxObjects)) {
                result.appendBool("maxReached", true);
                break;
            }
        }

        if (PlanExecutor::IS_EOF != state) {
            warning() << "Internal error while reading " << ns << endl;
        }

        ostringstream os;
        os << "Finding size for ns: " << ns;
        if (!min.isEmpty()) {
            os << " between " << min << " and " << max;
        }
        logIfSlow(timer, os.str());

        result.appendNumber("size", size);
        result.appendNumber("numObjects", numObjects);
        result.append("millis", timer.millis());
        return true;
    }

} cmdDatasize;

class CollectionStats : public Command {
public:
    CollectionStats() : Command("collStats", false, "collstats") {}

    virtual bool slaveOk() const {
        return true;
    }
    virtual bool isWriteCommandForConfigServer() const {
        return false;
    }
    virtual void help(stringstream& help) const {
        help
            << "{ collStats:\"blog.posts\" , scale : 1 } scale divides sizes e.g. for KB use 1024\n"
               "    avgObjSize - in bytes";
    }

    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {
        ActionSet actions;
        actions.addAction(ActionType::collStats);
        out->push_back(Privilege(parseResourcePattern(dbname, cmdObj), actions));
    }

    bool run(OperationContext* txn,
             const string& dbname,
             BSONObj& jsobj,
             int,
             string& errmsg,
             BSONObjBuilder& result) {
        int scale = 1;
        if (jsobj["scale"].isNumber()) {
            scale = jsobj["scale"].numberInt();
            if (scale <= 0) {
                errmsg = "scale has to be >= 1";
                return false;
            }
        } else if (jsobj["scale"].trueValue()) {
            errmsg = "scale has to be a number >= 1";
            return false;
        }

        bool verbose = jsobj["verbose"].trueValue();

        const NamespaceString nss(parseNs(dbname, jsobj));

        if (nss.coll().empty()) {
            errmsg = "No collection name specified";
            return false;
        }

        AutoGetCollectionForRead ctx(txn, nss);
        if (!ctx.getDb()) {
            errmsg = "Database [" + nss.db().toString() + "] not found.";
            return false;
        }

        Collection* collection = ctx.getCollection();
        if (!collection) {
            errmsg = "Collection [" + nss.toString() + "] not found.";
            return false;
        }

        result.append("ns", nss.ns());

        long long size = collection->dataSize(txn) / scale;
        long long numRecords = collection->numRecords(txn);
        result.appendNumber("count", numRecords);
        result.appendNumber("size", size);
        if (numRecords)
            result.append("avgObjSize", collection->averageObjectSize(txn));

        result.appendNumber("storageSize",
                            static_cast<long long>(collection->getRecordStore()->storageSize(
                                txn, &result, verbose ? 1 : 0)) /
                                scale);

        collection->getRecordStore()->appendCustomStats(txn, &result, scale);

        IndexCatalog* indexCatalog = collection->getIndexCatalog();
        result.append("nindexes", indexCatalog->numIndexesReady(txn));

        // indexes
        BSONObjBuilder indexDetails;

        IndexCatalog::IndexIterator i = indexCatalog->getIndexIterator(txn, false);
        while (i.more()) {
            const IndexDescriptor* descriptor = i.next();
            IndexAccessMethod* iam = indexCatalog->getIndex(descriptor);
            invariant(iam);

            BSONObjBuilder bob;
            if (iam->appendCustomStats(txn, &bob, scale)) {
                indexDetails.append(descriptor->indexName(), bob.obj());
            }
        }

        result.append("indexDetails", indexDetails.done());

        BSONObjBuilder indexSizes;
        long long indexSize = collection->getIndexSize(txn, &indexSizes, scale);

        result.appendNumber("totalIndexSize", indexSize / scale);
        result.append("indexSizes", indexSizes.obj());

        return true;
    }

} cmdCollectionStats;

class CollectionModCommand : public Command {
public:
    CollectionModCommand() : Command("collMod") {}

    virtual bool slaveOk() const {
        return false;
    }
    virtual bool isWriteCommandForConfigServer() const {
        return true;
    }
    virtual void help(stringstream& help) const {
        help << "Sets collection options.\n"
                "Example: { collMod: 'foo', usePowerOf2Sizes:true }\n"
                "Example: { collMod: 'foo', index: {keyPattern: {a: 1}, expireAfterSeconds: 600} }";
    }

    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {
        ActionSet actions;
        actions.addAction(ActionType::collMod);
        out->push_back(Privilege(parseResourcePattern(dbname, cmdObj), actions));
    }

    bool run(OperationContext* txn,
             const string& dbname,
             BSONObj& jsobj,
             int,
             string& errmsg,
             BSONObjBuilder& result) {
        const std::string ns = parseNsCollectionRequired(dbname, jsobj);
        return appendCommandStatus(result, collMod(txn, NamespaceString(ns), jsobj, &result));
    }

} collectionModCommand;

class DBStats : public Command {
public:
    DBStats() : Command("dbStats", false, "dbstats") {}

    virtual bool slaveOk() const {
        return true;
    }
    virtual bool isWriteCommandForConfigServer() const {
        return false;
    }
    virtual void help(stringstream& help) const {
        help << "Get stats on a database. Not instantaneous. Slower for databases with large "
                ".ns files.\n"
                "Example: { dbStats:1, scale:1 }";
    }

    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {
        ActionSet actions;
        actions.addAction(ActionType::dbStats);
        out->push_back(Privilege(ResourcePattern::forDatabaseName(dbname), actions));
    }

    bool run(OperationContext* txn,
             const string& dbname,
             BSONObj& jsobj,
             int,
             string& errmsg,
             BSONObjBuilder& result) {
        int scale = 1;
        if (jsobj["scale"].isNumber()) {
            scale = jsobj["scale"].numberInt();
            if (scale <= 0) {
                errmsg = "scale has to be > 0";
                return false;
            }
        } else if (jsobj["scale"].trueValue()) {
            errmsg = "scale has to be a number > 0";
            return false;
        }

        const string ns = parseNs(dbname, jsobj);

        // TODO: OldClientContext legacy, needs to be removed
        CurOp::get(txn)->ensureStarted();
        {
            stdx::lock_guard<Client> lk(*txn->getClient());
            CurOp::get(txn)->setNS_inlock(dbname);
        }

        // We lock the entire database in S-mode in order to ensure that the contents will not
        // change for the stats snapshot. This might be unnecessary and if it becomes a
        // performance issue, we can take IS lock and then lock collection-by-collection.
        ScopedTransaction scopedXact(txn, MODE_IS);
        AutoGetDb autoDb(txn, ns, MODE_S);

        result.append("db", ns);

        Database* db = autoDb.getDb();
        if (!db) {
            // TODO: This preserves old behaviour where we used to create an empty database
            // metadata even when the database is accessed for read. Without this several
            // unit-tests will fail, which are fairly easy to fix. If backwards compatibility
            // is not needed for the missing DB case, we can just do the same that's done in
            // CollectionStats.
            result.appendNumber("collections", 0);
            result.appendNumber("objects", 0);
            result.append("avgObjSize", 0);
            result.appendNumber("dataSize", 0);
            result.appendNumber("storageSize", 0);
            result.appendNumber("numExtents", 0);
            result.appendNumber("indexes", 0);
            result.appendNumber("indexSize", 0);
            result.appendNumber("fileSize", 0);
        } else {
            {
                stdx::lock_guard<Client> lk(*txn->getClient());
                // TODO: OldClientContext legacy, needs to be removed
                CurOp::get(txn)->enter_inlock(dbname.c_str(), db->getProfilingLevel());
            }

            db->getStats(txn, &result, scale);
        }

        return true;
    }

} cmdDBStats;

/* Returns client's uri */
class CmdWhatsMyUri : public Command {
public:
    CmdWhatsMyUri() : Command("whatsmyuri") {}
    virtual bool slaveOk() const {
        return true;
    }
    virtual bool isWriteCommandForConfigServer() const {
        return false;
    }
    virtual void help(stringstream& help) const {
        help << "{whatsmyuri:1}";
    }
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {}  // No auth required
    virtual bool run(OperationContext* txn,
                     const string& dbname,
                     BSONObj& cmdObj,
                     int,
                     string& errmsg,
                     BSONObjBuilder& result) {
        result << "you" << txn->getClient()->clientAddress(true /*includePort*/);
        return true;
    }
} cmdWhatsMyUri;

class AvailableQueryOptions : public Command {
public:
    AvailableQueryOptions() : Command("availableQueryOptions", false, "availablequeryoptions") {}

    virtual bool slaveOk() const {
        return true;
    }
    virtual bool isWriteCommandForConfigServer() const {
        return false;
    }
    virtual Status checkAuthForCommand(ClientBasic* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) {
        return Status::OK();
    }

    virtual bool run(OperationContext* txn,
                     const string& dbname,
                     BSONObj& cmdObj,
                     int,
                     string& errmsg,
                     BSONObjBuilder& result) {
        result << "options" << QueryOption_AllSupported;
        return true;
    }
} availableQueryOptionsCmd;

/**
 * Guard object for making a good-faith effort to enter maintenance mode and leave it when it
 * goes out of scope.
 *
 * Sometimes we cannot set maintenance mode, in which case the call to setMaintenanceMode will
 * return a non-OK status.  This class does not treat that case as an error which means that
 * anybody using it is assuming it is ok to continue execution without maintenance mode.
 *
 * TODO: This assumption needs to be audited and documented, or this behavior should be moved
 * elsewhere.
 */
class MaintenanceModeSetter {
public:
    MaintenanceModeSetter()
        : maintenanceModeSet(
              repl::getGlobalReplicationCoordinator()->setMaintenanceMode(true).isOK()) {}
    ~MaintenanceModeSetter() {
        if (maintenanceModeSet)
            repl::getGlobalReplicationCoordinator()->setMaintenanceMode(false);
    }

private:
    bool maintenanceModeSet;
};

/**
 * this handles
 - auth
 - maintenance mode
 - opcounters
 - locking
 - context
 then calls run()
*/
void Command::execCommand(OperationContext* txn,
                          Command* command,
                          const rpc::RequestInterface& request,
                          rpc::ReplyBuilderInterface* replyBuilder) {
    try {
        {
            stdx::lock_guard<Client> lk(*txn->getClient());
            CurOp::get(txn)->setCommand_inlock(command);
        }
        // TODO: move this back to runCommands when mongos supports OperationContext
        // see SERVER-18515 for details.
        uassertStatusOK(rpc::readRequestMetadata(txn, request.getMetadata()));

        dassert(replyBuilder->getState() == rpc::ReplyBuilderInterface::State::kMetadata);

        std::string dbname = request.getDatabase().toString();
        unique_ptr<MaintenanceModeSetter> mmSetter;

        if (isHelpRequest(request)) {
            CurOp::get(txn)->ensureStarted();
            generateHelpResponse(txn, request, replyBuilder, *command);
            return;
        }

        ImpersonationSessionGuard guard(txn);
        uassertStatusOK(
            _checkAuthorization(command, txn->getClient(), dbname, request.getCommandArgs()));

        repl::ReplicationCoordinator* replCoord =
            repl::ReplicationCoordinator::get(txn->getClient()->getServiceContext());
        const bool iAmPrimary = replCoord->canAcceptWritesForDatabase(dbname);

        {
            bool commandCanRunOnSecondary = command->slaveOk();

            bool commandIsOverriddenToRunOnSecondary = command->slaveOverrideOk() &&

                // The $secondaryOk option is set.
                (rpc::ServerSelectionMetadata::get(txn).isSecondaryOk() ||

                 // Or the command has a read preference (may be incorrect, see SERVER-18194).
                 (rpc::ServerSelectionMetadata::get(txn).getReadPreference() != boost::none));

            bool iAmStandalone = !txn->writesAreReplicated();
            bool canRunHere = iAmPrimary || commandCanRunOnSecondary ||
                commandIsOverriddenToRunOnSecondary || iAmStandalone;

            // This logic is clearer if we don't have to invert it.
            if (!canRunHere && command->slaveOverrideOk()) {
                uasserted(ErrorCodes::NotMasterNoSlaveOkCode, "not master and slaveOk=false");
            }

            uassert(ErrorCodes::NotMaster, "not master", canRunHere);

            if (!command->maintenanceOk() &&
                replCoord->getReplicationMode() == repl::ReplicationCoordinator::modeReplSet &&
                !replCoord->canAcceptWritesForDatabase(dbname) &&
                !replCoord->getMemberState().secondary()) {
                uasserted(ErrorCodes::NotMasterOrSecondaryCode, "node is recovering");
            }
        }

        if (command->adminOnly()) {
            LOG(2) << "command: " << request.getCommandName();
        }

        if (command->maintenanceMode()) {
            mmSetter.reset(new MaintenanceModeSetter);
        }

        if (command->shouldAffectCommandCounter()) {
            OpCounters* opCounters = &globalOpCounters;
            opCounters->gotCommand();
        }

        // Handle command option maxTimeMS.
        int maxTimeMS =
            uassertStatusOK(LiteParsedQuery::parseMaxTimeMSCommand(request.getCommandArgs()));

        uassert(ErrorCodes::InvalidOptions,
                "no such command option $maxTimeMs; use maxTimeMS instead",
                !request.getCommandArgs().hasField("$maxTimeMS"));

        CurOp::get(txn)->setMaxTimeMicros(static_cast<unsigned long long>(maxTimeMS) * 1000);

        if (iAmPrimary) {
            // Handle shard version and config optime information that may have been sent along with
            // the command.
            auto& operationShardVersion = OperationShardVersion::get(txn);
            invariant(!operationShardVersion.hasShardVersion());

            auto commandNS = NamespaceString(command->parseNs(dbname, request.getCommandArgs()));
            operationShardVersion.initializeFromCommand(commandNS, request.getCommandArgs());

            auto requestMetadataStatus =
                rpc::ConfigServerRequestMetadata::readFromCommand(request.getCommandArgs());
            auto optime = uassertStatusOK(requestMetadataStatus).getOpTime();
            if (optime.is_initialized()) {
                if (ShardingState::get(txn)->enabled()) {
                    // TODO(spencer): Do this unconditionally once all nodes are sharding aware
                    // by default.
                    grid.shardRegistry()->advanceConfigOpTime(optime.get());
                } else {
                    massert(
                        28807,
                        "Received a command with sharding chunk information but this node is not "
                        "sharding aware",
                        command->name == "setShardVersion");
                }
            } else {
                // If there was top-level shard version information then there must have been
                // config optime information as well.  a 3.0 mongos won't have shard version info
                // at the top level (they have it in a nested "metadata" field) so it won't cause
                // a problem here.
                massert(28818,
                        str::stream()
                            << "Received command with chunk version information but no config "
                               "server optime: " << request.getCommandArgs().jsonString(),
                        !operationShardVersion.hasShardVersion() ||
                            ChunkVersion::isIgnoredVersion(
                                operationShardVersion.getShardVersion(commandNS)));
            }
        }

        // Can throw
        txn->checkForInterrupt();  // May trigger maxTimeAlwaysTimeOut fail point.

        bool retval = false;

        CurOp::get(txn)->ensureStarted();

        command->_commandsExecuted.increment();

        retval = command->run(txn, request, replyBuilder);

        dassert(replyBuilder->getState() == rpc::ReplyBuilderInterface::State::kOutputDocs);

        if (!retval) {
            command->_commandsFailed.increment();
        }
    } catch (const DBException& exception) {
        BSONObj metadata = rpc::makeEmptyMetadata();
        if (ShardingState::get(txn)->enabled()) {
            auto opTime = grid.shardRegistry()->getConfigOpTime();
            BSONObjBuilder metadataBob;
            rpc::ConfigServerMetadata(opTime).writeToMetadata(&metadataBob);
            metadata = metadataBob.obj();
        }

        Command::generateErrorResponse(txn, replyBuilder, exception, request, command, metadata);
    }
}

// This really belongs in commands.cpp, but we need to move it here so we can
// use shardingState and the repl coordinator without changing our entire library
// structure.
// It will be moved back as part of SERVER-18236.
bool Command::run(OperationContext* txn,
                  const rpc::RequestInterface& request,
                  rpc::ReplyBuilderInterface* replyBuilder) {
    BSONObjBuilder replyBuilderBob;

    repl::ReplicationCoordinator* replCoord = repl::getGlobalReplicationCoordinator();

    repl::ReadConcernArgs readConcern;
    {
        // parse and validate ReadConcernArgs
        auto readConcernParseStatus = readConcern.initialize(request.getCommandArgs());
        if (!readConcernParseStatus.isOK()) {
            replyBuilder->setMetadata(rpc::makeEmptyMetadata())
                .setCommandReply(readConcernParseStatus);
            return false;
        }

        if (!supportsReadConcern()) {
            // Only return an error if a non-nullish readConcern was parsed, but do not process
            // readConcern regardless.
            if (!readConcern.getOpTime().isNull() ||
                readConcern.getLevel() != repl::ReadConcernLevel::kLocalReadConcern) {
                replyBuilder->setMetadata(rpc::makeEmptyMetadata())
                    .setCommandReply({ErrorCodes::InvalidOptions,
                                      str::stream()
                                          << "Command " << name << " does not support "
                                          << repl::ReadConcernArgs::kReadConcernFieldName});
                return false;
            }
        } else {
            // Skip waiting for the OpTime when testing snapshot behavior.
            if (!testingSnapshotBehaviorInIsolation) {
                // Wait for readConcern to be satisfied.
                auto readConcernResult = replCoord->waitUntilOpTime(txn, readConcern);
                readConcernResult.appendInfo(&replyBuilderBob);
                if (!readConcernResult.getStatus().isOK()) {
                    replyBuilder->setMetadata(rpc::makeEmptyMetadata())
                        .setCommandReply(readConcernResult.getStatus(), replyBuilderBob.done());
                    return false;
                }
            }

            if ((replCoord->getReplicationMode() ==
                     repl::ReplicationCoordinator::Mode::modeReplSet ||
                 testingSnapshotBehaviorInIsolation) &&
                readConcern.getLevel() == repl::ReadConcernLevel::kMajorityReadConcern) {
                Status status = txn->recoveryUnit()->setReadFromMajorityCommittedSnapshot();

                // Wait until a snapshot is available.
                while (status == ErrorCodes::ReadConcernMajorityNotAvailableYet) {
                    replCoord->waitForNewSnapshot(txn);
                    status = txn->recoveryUnit()->setReadFromMajorityCommittedSnapshot();
                }

                if (!status.isOK()) {
                    replyBuilder->setMetadata(rpc::makeEmptyMetadata()).setCommandReply(status);
                    return false;
                }
            }
        }
    }

    // run expects non-const bsonobj
    BSONObj cmd = request.getCommandArgs();
    // Implementation just forwards to the old method signature for now.
    std::string errmsg;

    // run expects const db std::string (can't bind to temporary)
    const std::string db = request.getDatabase().toString();

    // TODO: remove queryOptions parameter from command's run method.
    bool result = this->run(txn, db, cmd, 0, errmsg, replyBuilderBob);

    BSONObjBuilder metadataBob;

    const bool isShardingAware = ShardingState::get(txn)->enabled();
    bool isReplSet = replCoord->getReplicationMode() == repl::ReplicationCoordinator::modeReplSet;
    if (isReplSet) {
        repl::OpTime lastOpTimeFromClient =
            repl::ReplClientInfo::forClient(txn->getClient()).getLastOp();
        replCoord->prepareReplResponseMetadata(
            request, lastOpTimeFromClient, readConcern, &metadataBob);

        // For commands from mongos, append some info to help getLastError(w) work.
        // TODO: refactor out of here as part of SERVER-18326
        if (isShardingAware || serverGlobalParams.configsvr) {
            rpc::ShardingMetadata(lastOpTimeFromClient, replCoord->getElectionId())
                .writeToMetadata(&metadataBob);
        }
    }

    if (isShardingAware) {
        auto opTime = grid.shardRegistry()->getConfigOpTime();
        rpc::ConfigServerResponseMetadata(opTime).writeToMetadata(&metadataBob);
    }

    auto cmdResponse = replyBuilderBob.done();
    replyBuilder->setMetadata(metadataBob.done());

    if (result) {
        replyBuilder->setCommandReply(std::move(cmdResponse));
    } else {
        // maintain existing behavior of returning all data appended to builder
        // even if command returned false
        replyBuilder->setCommandReply(Status(ErrorCodes::CommandFailed, errmsg),
                                      std::move(cmdResponse));
    }

    return result;
}

void Command::registerError(OperationContext* txn, const DBException& exception) {
    CurOp::get(txn)->debug().exceptionInfo = exception.getInfo();
}

}  // namespace mongo
