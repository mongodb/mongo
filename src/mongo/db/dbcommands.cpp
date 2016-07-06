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

#include <array>
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
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index_builder.h"
#include "mongo/db/instance.h"
#include "mongo/db/introspect.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/lasterror.h"
#include "mongo/db/matcher/extensions_callback_disallow_extensions.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/ops/insert.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/read_concern.h"
#include "mongo/db/repair_database.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/write_concern.h"
#include "mongo/rpc/metadata.h"
#include "mongo/rpc/metadata/config_server_metadata.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/rpc/metadata/server_selection_metadata.h"
#include "mongo/rpc/metadata/sharding_metadata.h"
#include "mongo/rpc/reply_builder_interface.h"
#include "mongo/rpc/request_interface.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/stale_exception.h"
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


    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
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
        if (serverGlobalParams.clusterRole == ClusterRole::ConfigServer && (dbname == "config")) {
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


    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
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

        // TODO (Kal): OldClientContext legacy, needs to be removed
        {
            CurOp::get(txn)->ensureStarted();
            stdx::lock_guard<Client> lk(*txn->getClient());
            CurOp::get(txn)->setNS_inlock(dbname);
        }

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


    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
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
        BSONElement firstElement = cmdObj.firstElement();
        int profilingLevel = firstElement.numberInt();

        // If profilingLevel is 0, 1, or 2, needs to be locked exclusively,
        // because creates the system.profile collection in the local database.

        const bool readOnly = (profilingLevel < 0 || profilingLevel > 2);
        const LockMode dbMode = readOnly ? MODE_S : MODE_X;
        const LockMode transactionMode = readOnly ? MODE_IS : MODE_IX;

        Status status = Status::OK();

        ScopedTransaction transaction(txn, transactionMode);
        AutoGetDb ctx(txn, dbname, dbMode);
        Database* db = ctx.getDb();

        result.append("was", db ? db->getProfilingLevel() : serverGlobalParams.defaultProfile);
        result.append("slowms", serverGlobalParams.slowMS);

        if (!readOnly) {
            if (!db) {
                // When setting the profiling level, create the database if it didn't already exist.
                // When just reading the profiling level, we do not create the database.
                db = dbHolder().openDb(txn, dbname);
            }
            status = db->setProfilingLevel(txn, profilingLevel);
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


    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
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

        // TODO (Kal): OldClientContext legacy, needs to be removed
        {
            CurOp::get(txn)->ensureStarted();
            stdx::lock_guard<Client> lk(*txn->getClient());
            CurOp::get(txn)->setNS_inlock(dbname);
        }

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


    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    virtual bool run(OperationContext* txn,
                     const string& dbname,
                     BSONObj& cmdObj,
                     int,
                     string& errmsg,
                     BSONObjBuilder& result) {
        const NamespaceString nsToDrop = parseNsCollectionRequired(dbname, cmdObj);

        if (NamespaceString::virtualized(nsToDrop.ns())) {
            errmsg = "can't drop a virtual collection";
            return false;
        }

        if ((repl::getGlobalReplicationCoordinator()->getReplicationMode() !=
             repl::ReplicationCoordinator::modeNone) &&
            nsToDrop.isOplog()) {
            errmsg = "can't drop live oplog while replicating";
            return false;
        }

        return appendCommandStatus(result, dropCollection(txn, nsToDrop, result));
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


    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
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


    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
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
            auto qr = stdx::make_unique<QueryRequest>(NamespaceString(ns));
            qr->setFilter(query);
            qr->setSort(sort);

            auto statusWithCQ = CanonicalQuery::canonicalize(
                txn, std::move(qr), ExtensionsCallbackDisallowExtensions());
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
            exec->registerExec(coll);

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

            if (PlanExecutor::DEAD == state || PlanExecutor::FAILURE == state) {
                return appendCommandStatus(result,
                                           Status(ErrorCodes::OperationFailed,
                                                  str::stream()
                                                      << "Executor error during filemd5 command: "
                                                      << WorkingSetCommon::toStatusString(obj)));
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
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
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
        BSONObj obj;
        PlanExecutor::ExecState state;
        while (PlanExecutor::ADVANCED == (state = exec->getNext(&obj, &loc))) {
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

        if (PlanExecutor::FAILURE == state || PlanExecutor::DEAD == state) {
            warning() << "Internal error while reading " << ns << endl;
            return appendCommandStatus(
                result,
                Status(ErrorCodes::OperationFailed,
                       str::stream() << "Executor error while reading during dataSize command: "
                                     << WorkingSetCommon::toStatusString(obj)));
        }

        ostringstream os;
        os << "Finding size for ns: " << ns;
        if (!min.isEmpty()) {
            os << " between " << min << " and " << max;
        }

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
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
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
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
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
        const NamespaceString nss = parseNsCollectionRequired(dbname, jsobj);
        return appendCommandStatus(result, collMod(txn, nss, jsobj, &result));
    }

} collectionModCommand;

class DBStats : public Command {
public:
    DBStats() : Command("dbStats", false, "dbstats") {}

    virtual bool slaveOk() const {
        return true;
    }
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
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

        // TODO (Kal): OldClientContext legacy, needs to be removed
        {
            CurOp::get(txn)->ensureStarted();
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
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
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
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
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

namespace {

// Symbolic names for indexes to make code more readable.
const std::size_t kCmdOptionMaxTimeMSField = 0;
const std::size_t kHelpField = 1;
const std::size_t kShardVersionFieldIdx = 2;
const std::size_t kQueryOptionMaxTimeMSField = 3;

// We make an array of the fields we need so we can call getFields once. This saves repeated
// scans over the command object.
const std::array<StringData, 4> neededFieldNames{QueryRequest::cmdOptionMaxTimeMS,
                                                 Command::kHelpFieldName,
                                                 ChunkVersion::kShardVersionField,
                                                 QueryRequest::queryOptionMaxTimeMS};
}  // namespace

void appendOpTimeMetadata(OperationContext* txn,
                          const rpc::RequestInterface& request,
                          BSONObjBuilder* metadataBob) {
    const bool isShardingAware = ShardingState::get(txn)->enabled();
    const bool isConfig = serverGlobalParams.clusterRole == ClusterRole::ConfigServer;
    repl::ReplicationCoordinator* replCoord = repl::getGlobalReplicationCoordinator();
    const bool isReplSet =
        replCoord->getReplicationMode() == repl::ReplicationCoordinator::modeReplSet;

    if (isReplSet) {
        // Attach our own last opTime.
        repl::OpTime lastOpTimeFromClient =
            repl::ReplClientInfo::forClient(txn->getClient()).getLastOp();
        if (request.getMetadata().hasField(rpc::kReplSetMetadataFieldName)) {
            replCoord->prepareReplMetadata(lastOpTimeFromClient, metadataBob);
        }
        // For commands from mongos, append some info to help getLastError(w) work.
        // TODO: refactor out of here as part of SERVER-18236
        if (isShardingAware || isConfig) {
            rpc::ShardingMetadata(lastOpTimeFromClient, replCoord->getElectionId())
                .writeToMetadata(metadataBob);
        }
    }

    // If we're a shard other than the config shard, attach the last configOpTime we know about.
    if (isShardingAware && !isConfig) {
        auto opTime = grid.configOpTime();
        rpc::ConfigServerMetadata(opTime).writeToMetadata(metadataBob);
    }
}

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

        dassert(replyBuilder->getState() == rpc::ReplyBuilderInterface::State::kCommandReply);

        std::string dbname = request.getDatabase().toString();
        unique_ptr<MaintenanceModeSetter> mmSetter;

        std::array<BSONElement, std::tuple_size<decltype(neededFieldNames)>::value>
            extractedFields{};
        request.getCommandArgs().getFields(neededFieldNames, &extractedFields);

        if (isHelpRequest(extractedFields[kHelpField])) {
            CurOp::get(txn)->ensureStarted();
            // We disable last-error for help requests due to SERVER-11492, because config servers
            // use help requests to determine which commands are database writes, and so must be
            // forwarded to all config servers.
            LastError::get(txn->getClient()).disable();
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
                rpc::ServerSelectionMetadata::get(txn).canRunOnSecondary();

            bool iAmStandalone = !txn->writesAreReplicated();
            bool canRunHere = iAmPrimary || commandCanRunOnSecondary ||
                commandIsOverriddenToRunOnSecondary || iAmStandalone;

            // This logic is clearer if we don't have to invert it.
            if (!canRunHere && command->slaveOverrideOk()) {
                uasserted(ErrorCodes::NotMasterNoSlaveOk, "not master and slaveOk=false");
            }

            uassert(ErrorCodes::NotMaster, "not master", canRunHere);

            if (!command->maintenanceOk() &&
                replCoord->getReplicationMode() == repl::ReplicationCoordinator::modeReplSet &&
                !replCoord->canAcceptWritesForDatabase(dbname) &&
                !replCoord->getMemberState().secondary()) {

                uassert(ErrorCodes::NotMasterOrSecondary,
                        "node is recovering",
                        !replCoord->getMemberState().recovering());
                uassert(ErrorCodes::NotMasterOrSecondary,
                        "node is not in primary or recovering state",
                        replCoord->getMemberState().primary());
                // Check ticket SERVER-21432, slaveOk commands are allowed in drain mode
                uassert(ErrorCodes::NotMasterOrSecondary,
                        "node is in drain mode",
                        commandIsOverriddenToRunOnSecondary || commandCanRunOnSecondary);
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
        int maxTimeMS = uassertStatusOK(
            QueryRequest::parseMaxTimeMS(extractedFields[kCmdOptionMaxTimeMSField]));

        uassert(ErrorCodes::InvalidOptions,
                "no such command option $maxTimeMs; use maxTimeMS instead",
                extractedFields[kQueryOptionMaxTimeMSField].eoo());

        if (maxTimeMS > 0) {
            uassert(40119,
                    "Illegal attempt to set operation deadline within DBDirectClient",
                    !txn->getClient()->isInDirectClient());
            txn->setDeadlineAfterNowBy(Milliseconds{maxTimeMS});
        }

        // Operations are only versioned against the primary. We also make sure not to redo shard
        // version handling if this command was issued via the direct client.
        if (iAmPrimary && !txn->getClient()->isInDirectClient()) {
            // Handle shard version and config optime information that may have been sent along with
            // the command.
            auto& oss = OperationShardingState::get(txn);

            auto commandNS = NamespaceString(command->parseNs(dbname, request.getCommandArgs()));
            oss.initializeShardVersion(commandNS, extractedFields[kShardVersionFieldIdx]);

            auto shardingState = ShardingState::get(txn);
            if (shardingState->enabled()) {
                // TODO(spencer): Do this unconditionally once all nodes are sharding aware
                // by default.
                shardingState->updateConfigServerOpTimeFromMetadata(txn);
            } else {
                massert(
                    34422,
                    str::stream()
                        << "Received a command with sharding chunk version information but this "
                           "node is not sharding aware: "
                        << request.getCommandArgs().jsonString(),
                    !oss.hasShardVersion() ||
                        ChunkVersion::isIgnoredVersion(oss.getShardVersion(commandNS)));
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
    } catch (const DBException& e) {
        // If we got a stale config, wait in case the operation is stuck in a critical section
        if (e.getCode() == ErrorCodes::SendStaleConfig) {
            auto& sce = static_cast<const StaleConfigException&>(e);
            ShardingState::get(txn)->onStaleShardVersion(
                txn, NamespaceString(sce.getns()), sce.getVersionReceived());
        }

        BSONObjBuilder metadataBob;
        appendOpTimeMetadata(txn, request, &metadataBob);

        Command::generateErrorResponse(txn, replyBuilder, e, request, command, metadataBob.done());
    }
}

// This really belongs in commands.cpp, but we need to move it here so we can
// use shardingState and the repl coordinator without changing our entire library
// structure.
// It will be moved back as part of SERVER-18236.
bool Command::run(OperationContext* txn,
                  const rpc::RequestInterface& request,
                  rpc::ReplyBuilderInterface* replyBuilder) {
    auto bytesToReserve = reserveBytesForReply();

// SERVER-22100: In Windows DEBUG builds, the CRT heap debugging overhead, in conjunction with the
// additional memory pressure introduced by reply buffer pre-allocation, causes the concurrency
// suite to run extremely slowly. As a workaround we do not pre-allocate in Windows DEBUG builds.
#ifdef _WIN32
    if (kDebugBuild)
        bytesToReserve = 0;
#endif

    // run expects non-const bsonobj
    BSONObj cmd = request.getCommandArgs();

    // run expects const db std::string (can't bind to temporary)
    const std::string db = request.getDatabase().toString();

    BSONObjBuilder inPlaceReplyBob(replyBuilder->getInPlaceReplyBuilder(bytesToReserve));

    {
        auto readConcernArgsStatus = extractReadConcern(txn, cmd, supportsReadConcern());
        if (!readConcernArgsStatus.isOK()) {
            auto result = appendCommandStatus(inPlaceReplyBob, readConcernArgsStatus.getStatus());
            inPlaceReplyBob.doneFast();
            replyBuilder->setMetadata(rpc::makeEmptyMetadata());
            return result;
        }

        Status rcStatus = waitForReadConcern(txn, readConcernArgsStatus.getValue());
        if (!rcStatus.isOK()) {
            if (rcStatus == ErrorCodes::ExceededTimeLimit) {
                const int debugLevel =
                    serverGlobalParams.clusterRole == ClusterRole::ConfigServer ? 0 : 2;
                LOG(debugLevel) << "Command on database " << db
                                << " timed out waiting for read concern to be satisfied. Command: "
                                << getRedactedCopyForLogging(request.getCommandArgs());
            }

            auto result = appendCommandStatus(inPlaceReplyBob, rcStatus);
            inPlaceReplyBob.doneFast();
            replyBuilder->setMetadata(rpc::makeEmptyMetadata());
            return result;
        }
    }

    auto wcResult = extractWriteConcern(txn, cmd, db, supportsWriteConcern(cmd));
    if (!wcResult.isOK()) {
        auto result = appendCommandStatus(inPlaceReplyBob, wcResult.getStatus());
        inPlaceReplyBob.doneFast();
        replyBuilder->setMetadata(rpc::makeEmptyMetadata());
        return result;
    }

    std::string errmsg;
    bool result;
    if (!supportsWriteConcern(cmd)) {
        // TODO: remove queryOptions parameter from command's run method.
        result = run(txn, db, cmd, 0, errmsg, inPlaceReplyBob);
    } else {
        // Change the write concern while running the command.
        const auto oldWC = txn->getWriteConcern();
        ON_BLOCK_EXIT([&] { txn->setWriteConcern(oldWC); });
        txn->setWriteConcern(wcResult.getValue());

        result = run(txn, db, cmd, 0, errmsg, inPlaceReplyBob);

        // Nothing in run() should change the writeConcern.
        dassert(txn->getWriteConcern().toBSON() == wcResult.getValue().toBSON());

        WriteConcernResult res;
        auto waitForWCStatus =
            waitForWriteConcern(txn,
                                repl::ReplClientInfo::forClient(txn->getClient()).getLastOp(),
                                wcResult.getValue(),
                                &res);
        appendCommandWCStatus(inPlaceReplyBob, waitForWCStatus, res);

        // SERVER-22421: This code is to ensure error response backwards compatibility with the
        // user management commands. This can be removed in 3.6.
        if (!waitForWCStatus.isOK() && isUserManagementCommand(getName())) {
            BSONObj temp = inPlaceReplyBob.asTempObj().copy();
            inPlaceReplyBob.resetToEmpty();
            appendCommandStatus(inPlaceReplyBob, waitForWCStatus);
            inPlaceReplyBob.appendElementsUnique(temp);
        }
    }

    appendCommandStatus(inPlaceReplyBob, result, errmsg);
    inPlaceReplyBob.doneFast();

    BSONObjBuilder metadataBob;
    appendOpTimeMetadata(txn, request, &metadataBob);
    replyBuilder->setMetadata(metadataBob.done());

    return result;
}

void Command::registerError(OperationContext* txn, const DBException& exception) {
    CurOp::get(txn)->debug().exceptionInfo = exception.getInfo();
}

}  // namespace mongo
