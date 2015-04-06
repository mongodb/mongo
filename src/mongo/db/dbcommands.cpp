// dbcommands.cpp

/**
*    Copyright (C) 2012-2014 MongoDB Inc.
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

#include <boost/scoped_ptr.hpp>
#include <time.h>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/init.h"
#include "mongo/base/status.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/audit.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/user_management_commands_parser.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/background.h"
#include "mongo/db/catalog/collection_catalog_entry.h"
#include "mongo/db/catalog/database_catalog_entry.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/commands/shutdown.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/db.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/service_context_d.h"
#include "mongo/db/service_context.h"
#include "mongo/db/index_builder.h"
#include "mongo/db/instance.h"
#include "mongo/db/introspect.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/db/lasterror.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/ops/insert.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/repair_database.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/storage/mmap_v1/dur_stats.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/write_concern.h"
#include "mongo/s/d_state.h"
#include "mongo/s/stale_exception.h"  // for SendStaleConfigException
#include "mongo/scripting/engine.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/md5.hpp"
#include "mongo/util/print.h"

namespace mongo {

    using boost::scoped_ptr;
    using std::auto_ptr;
    using std::endl;
    using std::ostringstream;
    using std::string;
    using std::stringstream;

    CmdShutdown cmdShutdown;

    void CmdShutdown::help( stringstream& help ) const {
        help << "shutdown the database.  must be ran against admin db and "
             << "either (1) ran from localhost or (2) authenticated. If "
             << "this is a primary in a replica set and there is no member "
             << "within 10 seconds of its optime, it will not shutdown "
             << "without force : true.  You can also specify timeoutSecs : "
             << "N to wait N seconds for other members to catch up.";
    }

    bool CmdShutdown::run(OperationContext* txn, const string& dbname, BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
        bool force = cmdObj.hasField("force") && cmdObj["force"].trueValue();

        long long timeoutSecs = 0;
        if (cmdObj.hasField("timeoutSecs")) {
            timeoutSecs = cmdObj["timeoutSecs"].numberLong();
        }

        Status status = repl::getGlobalReplicationCoordinator()->stepDown(
                txn,
                force,
                repl::ReplicationCoordinator::Milliseconds(timeoutSecs * 1000),
                repl::ReplicationCoordinator::Milliseconds(120 * 1000));
        if (!status.isOK() && status.code() != ErrorCodes::NotMaster) { // ignore not master
            return appendCommandStatus(result, status);
        }

        shutdownHelper();
        return true;
    }

    class CmdDropDatabase : public Command {
    public:
        virtual void help( stringstream& help ) const {
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

        virtual bool isWriteCommandForConfigServer() const { return true; }

        virtual std::vector<BSONObj> stopIndexBuilds(OperationContext* opCtx,
                                                     Database* db,
                                                     const BSONObj& cmdObj) {
            invariant(db);
            std::list<std::string> collections;
            db->getDatabaseCatalogEntry()->getCollectionNamespaces(&collections);

            std::vector<BSONObj> allKilledIndexes;
            for (std::list<std::string>::iterator it = collections.begin();
                 it != collections.end();
                 ++it) {
                std::string ns = *it;

                IndexCatalog::IndexKillCriteria criteria;
                criteria.ns = ns;
                std::vector<BSONObj> killedIndexes =
                    IndexBuilder::killMatchingIndexBuilds(db->getCollection(ns), criteria);
                allKilledIndexes.insert(allKilledIndexes.end(),
                                        killedIndexes.begin(),
                                        killedIndexes.end());
            }
            return allKilledIndexes;
        }

        CmdDropDatabase() : Command("dropDatabase") {}

        bool run(OperationContext* txn, const string& dbname, BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            // disallow dropping the config database
            if (serverGlobalParams.configsvr && (dbname == "config")) {
                errmsg = "Cannot drop 'config' database if mongod started with --configsvr";
                return false;
            }

            if ((repl::getGlobalReplicationCoordinator()->getReplicationMode() !=
                 repl::ReplicationCoordinator::modeNone) &&
                (dbname == "local")) {
                errmsg = "Cannot drop 'local' database while replication is active";
                return false;
            }
            BSONElement e = cmdObj.firstElement();
            int p = (int) e.number();
            if ( p != 1 ) {
                errmsg = "have to pass 1 as db parameter";
                return false;
            }

            MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
                ScopedTransaction transaction(txn, MODE_X);
                Lock::GlobalWrite lk(txn->lockState());
                AutoGetDb autoDB(txn, dbname, MODE_X);
                Database* const db = autoDB.getDb();
                if (!db) {
                    // DB doesn't exist, so deem it a success.
                    return true;
                }
                OldClientContext context(txn, dbname);
                if (!fromRepl &&
                    !repl::getGlobalReplicationCoordinator()->canAcceptWritesForDatabase(dbname)) {
                    return appendCommandStatus(result, Status(ErrorCodes::NotMaster, str::stream()
                        << "Not primary while dropping database " << dbname));
                }

                log() << "dropDatabase " << dbname << " starting" << endl;

                stopIndexBuilds(txn, db, cmdObj);
                dropDatabase(txn, db);

                log() << "dropDatabase " << dbname << " finished";

                WriteUnitOfWork wunit(txn);

                if (!fromRepl) {
                    getGlobalServiceContext()->getOpObserver()->onDropDatabase(txn, dbname + ".$cmd");
                }

                wunit.commit();
            } MONGO_WRITE_CONFLICT_RETRY_LOOP_END(txn, "dropDatabase", dbname);

            result.append( "dropped" , dbname );

            return true;
        }
    } cmdDropDatabase;

    class CmdRepairDatabase : public Command {
    public:
        virtual bool slaveOk() const {
            return true;
        }
        virtual bool maintenanceMode() const { return true; }
        virtual void help( stringstream& help ) const {
            help << "repair database.  also compacts. note: slow.";
        }

        virtual bool isWriteCommandForConfigServer() const { return true; }

        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            ActionSet actions;
            actions.addAction(ActionType::repairDatabase);
            out->push_back(Privilege(ResourcePattern::forDatabaseName(dbname), actions));
        }

        CmdRepairDatabase() : Command("repairDatabase") {

        }

        virtual std::vector<BSONObj> stopIndexBuilds(OperationContext* opCtx,
                                                     Database* db,
                                                     const BSONObj& cmdObj) {
            invariant(db);
            std::list<std::string> collections;
            db->getDatabaseCatalogEntry()->getCollectionNamespaces(&collections);

            std::vector<BSONObj> allKilledIndexes;
            for (std::list<std::string>::iterator it = collections.begin();
                 it != collections.end();
                 ++it) {
                std::string ns = *it;

                IndexCatalog::IndexKillCriteria criteria;
                criteria.ns = ns;
                std::vector<BSONObj> killedIndexes = 
                    IndexBuilder::killMatchingIndexBuilds(db->getCollection(ns), criteria);
                allKilledIndexes.insert(allKilledIndexes.end(), 
                                        killedIndexes.begin(), 
                                        killedIndexes.end());
            }
            return allKilledIndexes;
        }

        bool run(OperationContext* txn, const string& dbname , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            BSONElement e = cmdObj.firstElement();
            if ( e.numberInt() != 1 ) {
                errmsg = "bad option";
                return false;
            }

            // TODO: SERVER-4328 Don't lock globally
            ScopedTransaction transaction(txn, MODE_X);
            Lock::GlobalWrite lk(txn->lockState());
            OldClientContext context(txn,  dbname );

            log() << "repairDatabase " << dbname;
            std::vector<BSONObj> indexesInProg = stopIndexBuilds(txn, context.db(), cmdObj);

            e = cmdObj.getField( "preserveClonedFilesOnFailure" );
            bool preserveClonedFilesOnFailure = e.isBoolean() && e.boolean();
            e = cmdObj.getField( "backupOriginalFiles" );
            bool backupOriginalFiles = e.isBoolean() && e.boolean();

            StorageEngine* engine = getGlobalServiceContext()->getGlobalStorageEngine();
            Status status = repairDatabase(txn, engine, dbname, preserveClonedFilesOnFailure,
                                           backupOriginalFiles );

            IndexBuilder::restoreIndexes(txn, indexesInProg);

            // Open database before returning
            dbHolder().openDb(txn, dbname);
            return appendCommandStatus( result, status );
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

        virtual void help( stringstream& help ) const {
            help << "enable or disable performance profiling\n";
            help << "{ profile : <n> }\n";
            help << "0=off 1=log slow ops 2=log all\n";
            help << "-1 to get current values\n";
            help << "http://docs.mongodb.org/manual/reference/command/profile/#dbcmd.profile";
        }

        virtual bool isWriteCommandForConfigServer() const { return true; }

        virtual Status checkAuthForCommand(ClientBasic* client,
                                           const std::string& dbname,
                                           const BSONObj& cmdObj) {
            AuthorizationSession* authzSession = client->getAuthorizationSession();

            if (cmdObj.firstElement().numberInt() == -1 && !cmdObj.hasField("slowms")) {
                // If you just want to get the current profiling level you can do so with just
                // read access to system.profile, even if you can't change the profiling level.
                if (authzSession->isAuthorizedForActionsOnResource(
                        ResourcePattern::forExactNamespace(NamespaceString(dbname,
                                                                           "system.profile")),
                        ActionType::find)) {
                    return Status::OK();
                }
            }

            if (authzSession->isAuthorizedForActionsOnResource(
                    ResourcePattern::forDatabaseName(dbname), ActionType::enableProfiler)) {
                return Status::OK();
            }

            return Status(ErrorCodes::Unauthorized, "unauthorized");
        }

        CmdProfile() : Command("profile") {

        }

        bool run(OperationContext* txn,
                 const string& dbname,
                 BSONObj& cmdObj,
                 int options,
                 string& errmsg,
                 BSONObjBuilder& result,
                 bool fromRepl) {

            // Needs to be locked exclusively, because creates the system.profile collection
            // in the local database.
            ScopedTransaction transaction(txn, MODE_IX);
            Lock::DBLock dbXLock(txn->lockState(), dbname, MODE_X);
            OldClientContext ctx(txn, dbname);

            BSONElement e = cmdObj.firstElement();
            result.append("was", ctx.db()->getProfilingLevel());
            result.append("slowms", serverGlobalParams.slowMS);

            int p = (int) e.number();
            Status status = Status::OK();

            if (p == -1)
                status = Status::OK();
            else if ( p >= 0 && p <= 2 ) {
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
        CmdDiagLogging() : Command("diagLogging") { }
        bool adminOnly() const {
            return true;
        }

        void help(stringstream& h) const { h << "http://dochub.mongodb.org/core/monitoring#MonitoringandDiagnostics-DatabaseRecord%2FReplay%28diagLoggingcommand%29"; }

        virtual bool isWriteCommandForConfigServer() const { return true; }

        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            ActionSet actions;
            actions.addAction(ActionType::diagLogging);
            out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
        }

        bool run(OperationContext* txn, const string& dbname , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
            const char* deprecationWarning =
                "CMD diagLogging is deprecated and will be removed in a future release";
            warning() << deprecationWarning << startupWarningsLog;

            // This doesn't look like it requires exclusive DB lock, because it uses its own diag
            // locking, but originally the lock was set to be WRITE, so preserving the behaviour.
            //
            ScopedTransaction transaction(txn, MODE_IX);
            Lock::DBLock dbXLock(txn->lockState(), dbname, MODE_X);
            OldClientContext ctx(txn, dbname);

            int was = _diaglog.setLevel( cmdObj.firstElement().numberInt() );
            _diaglog.flush();
            if (!serverGlobalParams.quiet) {
                LOG(0) << "CMD: diagLogging set to " << _diaglog.getLevel() << " from: " << was << endl;
            }
            result.append( "was" , was );
            result.append( "note", deprecationWarning );
            return true;
        }
    } cmddiaglogging;

    /* drop collection */
    class CmdDrop : public Command {
    public:
        CmdDrop() : Command("drop") { }
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
        virtual void help( stringstream& help ) const { help << "drop a collection\n{drop : <collectionName>}"; }

        virtual bool isWriteCommandForConfigServer() const { return true; }

        virtual std::vector<BSONObj> stopIndexBuilds(OperationContext* opCtx,
                                                     Database* db,
                                                     const BSONObj& cmdObj) {
            const std::string nsToDrop = parseNsCollectionRequired(db->name(), cmdObj);

            IndexCatalog::IndexKillCriteria criteria;
            criteria.ns = nsToDrop;
            return IndexBuilder::killMatchingIndexBuilds(db->getCollection(nsToDrop), criteria);
        }

        virtual bool run(OperationContext* txn, const string& dbname , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            const std::string nsToDrop = parseNsCollectionRequired(dbname, cmdObj);

            if (!serverGlobalParams.quiet) {
                LOG(0) << "CMD: drop " << nsToDrop << endl;
            }

            if ( nsToDrop.find( '$' ) != string::npos ) {
                errmsg = "can't drop collection with reserved $ character in name";
                return false;
            }

            if ((repl::getGlobalReplicationCoordinator()->getReplicationMode() !=
                 repl::ReplicationCoordinator::modeNone) &&
                NamespaceString(nsToDrop).isOplog()) {
                errmsg = "can't drop live oplog while replicating";
                return false;
            }

            MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
                ScopedTransaction transaction(txn, MODE_IX);

                AutoGetDb autoDb(txn, dbname, MODE_X);
                Database* const db = autoDb.getDb();
                Collection* coll = db ? db->getCollection( nsToDrop ) : NULL;

                // If db/collection does not exist, short circuit and return.
                if ( !db || !coll ) {
                    errmsg = "ns not found";
                    return false;
                }
                OldClientContext context(txn, nsToDrop);
                if (!fromRepl &&
                    !repl::getGlobalReplicationCoordinator()->canAcceptWritesForDatabase(dbname)) {
                    return appendCommandStatus(result, Status(ErrorCodes::NotMaster, str::stream()
                        << "Not primary while dropping collection " << nsToDrop));
                }

                int numIndexes = coll->getIndexCatalog()->numIndexesTotal( txn );

                stopIndexBuilds(txn, db, cmdObj);

                result.append( "ns", nsToDrop );
                result.append( "nIndexesWas", numIndexes );

                WriteUnitOfWork wunit(txn);
                Status s = db->dropCollection( txn, nsToDrop );

                if ( !s.isOK() ) {
                    return appendCommandStatus( result, s );
                }

                if ( !fromRepl ) {
                    getGlobalServiceContext()->getOpObserver()->onDropCollection(
                            txn,
                            NamespaceString(nsToDrop));
                }
                wunit.commit();
            } MONGO_WRITE_CONFLICT_RETRY_LOOP_END(txn, "drop", nsToDrop);
            return true;
        }
    } cmdDrop;

    /* create collection */
    class CmdCreate : public Command {
    public:
        CmdCreate() : Command("create") { }
        virtual bool slaveOk() const {
            return false;
        }
        virtual bool adminOnly() const {
            return false;
        }

        virtual bool isWriteCommandForConfigServer() const { return true; }

        virtual void help( stringstream& help ) const {
            help << "create a collection explicitly\n"
                "{ create: <ns>[, capped: <bool>, size: <collSizeInBytes>, max: <nDocs>] }";
        }
        virtual Status checkAuthForCommand(ClientBasic* client,
                                           const std::string& dbname,
                                           const BSONObj& cmdObj) {
            AuthorizationSession* authzSession = client->getAuthorizationSession();
            if (cmdObj["capped"].trueValue()) {
                if (!authzSession->isAuthorizedForActionsOnResource(
                        parseResourcePattern(dbname, cmdObj), ActionType::convertToCapped)) {
                    return Status(ErrorCodes::Unauthorized, "unauthorized");
                }
            }

            // ActionType::createCollection or ActionType::insert are both acceptable
            if (authzSession->isAuthorizedForActionsOnResource(
                    parseResourcePattern(dbname, cmdObj), ActionType::createCollection) ||
                authzSession->isAuthorizedForActionsOnResource(
                    parseResourcePattern(dbname, cmdObj), ActionType::insert)) {
                return Status::OK();
            }

            return Status(ErrorCodes::Unauthorized, "unauthorized");
        }
        virtual bool run(OperationContext* txn, const string& dbname , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl ) {
            BSONObjIterator it(cmdObj);

            // Extract ns from first cmdObj element.
            BSONElement firstElt = it.next();
            uassert(15888,
                    "must pass name of collection to create",
                    firstElt.valuestrsafe()[0] != '\0');

            Status status = userAllowedWriteNS( dbname, firstElt.valuestr() );
            if ( !status.isOK() ) {
                return appendCommandStatus( result, status );
            }

            const std::string ns = dbname + '.' + firstElt.valuestrsafe();

            // Build options object from remaining cmdObj elements.
            BSONObjBuilder optionsBuilder;
            while (it.more()) {
                optionsBuilder.append(it.next());
            }

            BSONObj options = optionsBuilder.obj();
            uassert(14832,
                    "specify size:<n> when capped is true",
                    !options["capped"].trueValue() || options["size"].isNumber() ||
                        options.hasField("$nExtents"));

            MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
                ScopedTransaction transaction(txn, MODE_IX);
                Lock::DBLock dbXLock(txn->lockState(), dbname, MODE_X);
                OldClientContext ctx(txn, ns);
                if (!fromRepl &&
                    !repl::getGlobalReplicationCoordinator()->canAcceptWritesForDatabase(dbname)) {
                    return appendCommandStatus(result, Status(ErrorCodes::NotMaster, str::stream()
                        << "Not primary while creating collection " << ns));
                }

                WriteUnitOfWork wunit(txn);

                // Create collection.
                status =  userCreateNS(txn, ctx.db(), ns.c_str(), options, !fromRepl);
                if ( !status.isOK() ) {
                    return appendCommandStatus( result, status );
                }

                wunit.commit();
            } MONGO_WRITE_CONFLICT_RETRY_LOOP_END(txn, "create", ns);
            return true;
        }
    } cmdCreate;


    class CmdFileMD5 : public Command {
    public:
        CmdFileMD5() : Command( "filemd5" ) {

        }

        virtual bool slaveOk() const {
            return true;
        }

        virtual void help( stringstream& help ) const {
            help << " example: { filemd5 : ObjectId(aaaaaaa) , root : \"fs\" }";
        }

        virtual bool isWriteCommandForConfigServer() const { return false; }

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

        bool run(OperationContext* txn, const string& dbname, BSONObj& jsobj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl ) {
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
                if (!stateElem.eoo()){
                    int len;
                    const char* data = stateElem.binDataClean(len);
                    massert(16247, "md5 state not correct size", len == sizeof(st));
                    memcpy(&st, data, sizeof(st));
                }
                n = jsobj["startAt"].numberInt();
            }

            BSONObj query = BSON( "files_id" << jsobj["filemd5"] << "n" << GTE << n );
            BSONObj sort = BSON( "files_id" << 1 << "n" << 1 );

            MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
                CanonicalQuery* cq;
                if (!CanonicalQuery::canonicalize(ns, query, sort, BSONObj(), &cq).isOK()) {
                    uasserted(17240, "Can't canonicalize query " + query.toString());
                    return 0;
                }

                // Check shard version at startup.
                // This will throw before we've done any work if shard version is outdated
                // We drop and re-acquire these locks every document because md5'ing is expensive
                scoped_ptr<AutoGetCollectionForRead> ctx(new AutoGetCollectionForRead(txn, ns));
                Collection* coll = ctx->getCollection();
                const ChunkVersion shardVersionAtStart = shardingState.getVersion(ns);

                PlanExecutor* rawExec;
                if (!getExecutor(txn, coll, cq, PlanExecutor::YIELD_MANUAL, &rawExec,
                                 QueryPlannerParams::NO_TABLE_SCAN).isOK()) {
                    uasserted(17241, "Can't get executor for query " + query.toString());
                    return 0;
                }

                auto_ptr<PlanExecutor> exec(rawExec);
                // Process notifications when the lock is released/reacquired in the loop below
                exec->registerExec();

                BSONObj obj;
                PlanExecutor::ExecState state;
                while (PlanExecutor::ADVANCED == (state = exec->getNext(&obj, NULL))) {
                    BSONElement ne = obj["n"];
                    verify(ne.isNumber());
                    int myn = ne.numberInt();
                    if ( n != myn ) {
                        if (partialOk) {
                            break; // skipped chunk is probably on another shard
                        }
                        log() << "should have chunk: " << n << " have:" << myn << endl;
                        dumpChunks(txn, ns, query, sort);
                        uassert( 10040 ,  "chunks out of order" , n == myn );
                    }

                    // make a copy of obj since we access data in it while yielding locks
                    BSONObj owned = obj.getOwned();
                    exec->saveState();
                    // UNLOCKED
                    ctx.reset();

                    int len;
                    const char * data = owned["data"].binDataClean( len );
                    // This is potentially an expensive operation, so do it out of the lock
                    md5_append( &st , (const md5_byte_t*)(data) , len );
                    n++;

                    try {
                        // RELOCKED
                        ctx.reset(new AutoGetCollectionForRead(txn, ns));
                    }
                    catch (const SendStaleConfigException& ex) {
                        LOG(1) << "chunk metadata changed during filemd5, will retarget and continue";
                        break;
                    }

                    // Have the lock again. See if we were killed.
                    if (!exec->restoreState(txn)) {
                        if (!partialOk) {
                            uasserted(13281, "File deleted during filemd5 command");
                        }
                    }
                }

                if (partialOk)
                    result.appendBinData("md5state", sizeof(st), BinDataGeneral, &st);

                // This must be *after* the capture of md5state since it mutates st
                md5_finish(&st, d);

                result.append( "numChunks" , n );
                result.append( "md5" , digestToString( d ) );
            } MONGO_WRITE_CONFLICT_RETRY_LOOP_END(txn, "filemd5", dbname);
            return true;
        }

        void dumpChunks(OperationContext* txn,
                        const string& ns,
                        const BSONObj& query,
                        const BSONObj& sort) {
            DBDirectClient client(txn);
            Query q(query);
            q.sort(sort);
            auto_ptr<DBClientCursor> c = client.query(ns, q);
            while(c->more())
                PRINT(c->nextSafe());
        }
    } cmdFileMD5;


    class CmdDatasize : public Command {
        virtual string parseNs(const string& dbname, const BSONObj& cmdObj) const {
            return parseNsFullyQualified(dbname, cmdObj);
        }
    public:
        CmdDatasize() : Command( "dataSize", false, "datasize" ) {

        }

        virtual bool slaveOk() const { return true; }
        virtual bool isWriteCommandForConfigServer() const { return false; }
        virtual void help( stringstream &help ) const {
            help <<
                 "determine data size for a set of data in a certain range"
                 "\nexample: { dataSize:\"blog.posts\", keyPattern:{x:1}, min:{x:10}, max:{x:55} }"
                 "\nmin and max parameters are optional. They must either both be included or both omitted"
                 "\nkeyPattern is an optional parameter indicating an index pattern that would be useful"
                 "for iterating over the min/max bounds. If keyPattern is omitted, it is inferred from "
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

        bool run(OperationContext* txn, const string& dbname, BSONObj& jsobj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl ) {
            Timer timer;

            string ns = jsobj.firstElement().String();
            BSONObj min = jsobj.getObjectField( "min" );
            BSONObj max = jsobj.getObjectField( "max" );
            BSONObj keyPattern = jsobj.getObjectField( "keyPattern" );
            bool estimate = jsobj["estimate"].trueValue();

            AutoGetCollectionForRead ctx(txn, ns);

            Collection* collection = ctx.getCollection();

            if ( !collection || collection->numRecords(txn) == 0 ) {
                result.appendNumber( "size" , 0 );
                result.appendNumber( "numObjects" , 0 );
                result.append( "millis" , timer.millis() );
                return true;
            }

            result.appendBool( "estimate" , estimate );

            auto_ptr<PlanExecutor> exec;
            if ( min.isEmpty() && max.isEmpty() ) {
                if ( estimate ) {
                    result.appendNumber( "size" , static_cast<long long>(collection->dataSize(txn)) );
                    result.appendNumber( "numObjects",
                                         static_cast<long long>( collection->numRecords(txn) ) );
                    result.append( "millis" , timer.millis() );
                    return 1;
                }
                exec.reset(InternalPlanner::collectionScan(txn, ns,collection));
            }
            else if ( min.isEmpty() || max.isEmpty() ) {
                errmsg = "only one of min or max specified";
                return false;
            }
            else {

                if ( keyPattern.isEmpty() ){
                    // if keyPattern not provided, try to infer it from the fields in 'min'
                    keyPattern = Helpers::inferKeyPattern( min );
                }

                IndexDescriptor *idx =
                    collection->getIndexCatalog()->findIndexByPrefix( txn, keyPattern, true );  /* require single key */

                if ( idx == NULL ) {
                    errmsg = "couldn't find valid index containing key pattern";
                    return false;
                }
                // If both min and max non-empty, append MinKey's to make them fit chosen index
                KeyPattern kp( idx->keyPattern() );
                min = Helpers::toKeyFormat( kp.extendRangeBound( min, false ) );
                max = Helpers::toKeyFormat( kp.extendRangeBound( max, false ) );

                exec.reset(InternalPlanner::indexScan(txn, collection, idx, min, max, false));
            }

            long long avgObjSize = collection->dataSize(txn) / collection->numRecords(txn);

            long long maxSize = jsobj["maxSize"].numberLong();
            long long maxObjects = jsobj["maxObjects"].numberLong();

            long long size = 0;
            long long numObjects = 0;

            RecordId loc;
            PlanExecutor::ExecState state;
            while (PlanExecutor::ADVANCED == (state = exec->getNext(NULL, &loc))) {
                if ( estimate )
                    size += avgObjSize;
                else
                    size += collection->getRecordStore()->dataFor(txn, loc).size();

                numObjects++;

                if ( ( maxSize && size > maxSize ) ||
                        ( maxObjects && numObjects > maxObjects ) ) {
                    result.appendBool( "maxReached" , true );
                    break;
                }
            }

            if (PlanExecutor::IS_EOF != state) {
                warning() << "Internal error while reading " << ns << endl;
            }

            ostringstream os;
            os <<  "Finding size for ns: " << ns;
            if ( ! min.isEmpty() ) {
                os << " between " << min << " and " << max;
            }
            logIfSlow( timer , os.str() );

            result.appendNumber( "size", size );
            result.appendNumber( "numObjects" , numObjects );
            result.append( "millis" , timer.millis() );
            return true;
        }

    } cmdDatasize;

    class CollectionStats : public Command {
    public:
        CollectionStats() : Command( "collStats", false, "collstats" ) {

        }

        virtual bool slaveOk() const { return true; }
        virtual bool isWriteCommandForConfigServer() const { return false; }
        virtual void help( stringstream &help ) const {
            help << "{ collStats:\"blog.posts\" , scale : 1 } scale divides sizes e.g. for KB use 1024\n"
                    "    avgObjSize - in bytes";
        }

        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            ActionSet actions;
            actions.addAction(ActionType::collStats);
            out->push_back(Privilege(parseResourcePattern(dbname, cmdObj), actions));
        }

        bool run(OperationContext* txn, const string& dbname, BSONObj& jsobj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl ) {
            int scale = 1;
            if ( jsobj["scale"].isNumber() ) {
                scale = jsobj["scale"].numberInt();
                if ( scale <= 0 ) {
                    errmsg = "scale has to be >= 1";
                    return false;
                }
            }
            else if ( jsobj["scale"].trueValue() ) {
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

            result.append( "ns" , nss );

            long long size = collection->dataSize(txn) / scale;
            long long numRecords = collection->numRecords(txn);
            result.appendNumber( "count" , numRecords );
            result.appendNumber( "size" , size );
            if( numRecords )
                result.append( "avgObjSize" , collection->averageObjectSize(txn) );

            result.appendNumber("storageSize",
                                static_cast<long long>(collection->getRecordStore()
                                                       ->storageSize(txn,
                                                                     &result,
                                                                     verbose ? 1 : 0)) / scale);

            collection->getRecordStore()->appendCustomStats( txn, &result, scale );

            IndexCatalog* indexCatalog = collection->getIndexCatalog();
            result.append( "nindexes" , indexCatalog->numIndexesReady( txn ) );

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
        CollectionModCommand() : Command( "collMod" ) {

        }

        virtual bool slaveOk() const { return false; }
        virtual bool isWriteCommandForConfigServer() const { return true; }
        virtual void help( stringstream &help ) const {
            help <<
                "Sets collection options.\n"
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

        bool run(OperationContext* txn, const string& dbname, BSONObj& jsobj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl ) {
            const std::string ns = parseNsCollectionRequired(dbname, jsobj);

            ScopedTransaction transaction(txn, MODE_IX);
            AutoGetDb autoDb(txn, dbname, MODE_X);
            Database* const db = autoDb.getDb();
            Collection* coll = db ? db->getCollection(ns) : NULL;

            // If db/collection does not exist, short circuit and return.
            if ( !db || !coll ) {
                errmsg = "ns does not exist";
                return false;
            }

            OldClientContext ctx(txn,  ns);
            if (!fromRepl &&
                !repl::getGlobalReplicationCoordinator()->canAcceptWritesForDatabase(dbname)) {
                return appendCommandStatus(result, Status(ErrorCodes::NotMaster, str::stream()
                    << "Not primary while setting collection options on " << ns));
            }

            WriteUnitOfWork wunit(txn);

            bool ok = true;

            BSONForEach( e, jsobj ) {
                if ( str::equals( "collMod", e.fieldName() ) ) {
                    // no-op
                }
                else if ( str::startsWith( e.fieldName(), "$" ) ) {
                    // no-op: ignore top-level fields prefixed with $. They are for the command processor.
                }
                else if ( LiteParsedQuery::cmdOptionMaxTimeMS == e.fieldNameStringData() ) {
                    // no-op
                }
                else if ( str::equals( "index", e.fieldName() ) ) {
                    BSONObj indexObj = e.Obj();
                    BSONObj keyPattern = indexObj.getObjectField( "keyPattern" );

                    if ( keyPattern.isEmpty() ){
                        errmsg = "no keyPattern specified";
                        ok = false;
                        continue;
                    }

                    BSONElement newExpireSecs = indexObj["expireAfterSeconds"];
                    if ( newExpireSecs.eoo() ) {
                        errmsg = "no expireAfterSeconds field";
                        ok = false;
                        continue;
                    }
                    if ( ! newExpireSecs.isNumber() ) {
                        errmsg = "expireAfterSeconds field must be a number";
                        ok = false;
                        continue;
                    }

                    const IndexDescriptor* idx = coll->getIndexCatalog()
                                                     ->findIndexByKeyPattern( txn, keyPattern );
                    if ( idx == NULL ) {
                        errmsg = str::stream() << "cannot find index " << keyPattern
                                               << " for ns " << ns;
                        ok = false;
                        continue;
                    }
                    BSONElement oldExpireSecs = idx->infoObj().getField("expireAfterSeconds");
                    if( oldExpireSecs.eoo() ){
                        errmsg = "no expireAfterSeconds field to update";
                        ok = false;
                        continue;
                    }
                    if( ! oldExpireSecs.isNumber() ) {
                        errmsg = "existing expireAfterSeconds field is not a number";
                        ok = false;
                        continue;
                    }

                    if ( oldExpireSecs != newExpireSecs ) {
                        result.appendAs( oldExpireSecs, "expireAfterSeconds_old" );
                        // Change the value of "expireAfterSeconds" on disk.
                        coll->getCatalogEntry()->updateTTLSetting( txn,
                                                                   idx->indexName(),
                                                                   newExpireSecs.numberLong() );
                        // Notify the index catalog that the definition of this index changed.
                        idx = coll->getIndexCatalog()->refreshEntry( txn, idx );
                        result.appendAs( newExpireSecs , "expireAfterSeconds_new" );
                    }
                }
                else {
                    // As of SERVER-17312 we only support these two options. When SERVER-17320 is
                    // resolved this will need to be enhanced to handle other options.
                    typedef CollectionOptions CO;
                    const StringData name = e.fieldNameStringData();
                    const int flag = (name == "usePowerOf2Sizes") ? CO::Flag_UsePowerOf2Sizes :
                                     (name == "noPadding") ? CO::Flag_NoPadding :
                                     0;
                    if (!flag) {
                        errmsg = str::stream() << "unknown option to collMod: " << name;
                        ok = false;
                        continue;
                    }

                    CollectionCatalogEntry* cce = coll->getCatalogEntry();

                    const int oldFlags = cce->getCollectionOptions(txn).flags;
                    const bool oldSetting = oldFlags & flag;
                    const bool newSetting = e.trueValue();

                    result.appendBool( name.toString() + "_old", oldSetting );
                    result.appendBool( name.toString() + "_new", newSetting );

                    const int newFlags = newSetting
                                       ? (oldFlags | flag) // set flag
                                       : (oldFlags & ~flag); // clear flag

                    // NOTE we do this unconditionally to ensure that we note that the user has
                    // explicitly set flags, even if they are just setting the default.
                    cce->updateFlags(txn, newFlags);

                    const CollectionOptions newOptions = cce->getCollectionOptions(txn);
                    invariant(newOptions.flags == newFlags);
                    invariant(newOptions.flagsSet);
                }
            }

            if (!ok) {
                return false;
            }

            if (!fromRepl) {
                getGlobalServiceContext()->getOpObserver()->onCollMod(txn,
                                                                   (dbname + ".$cmd").c_str(),
                                                                   jsobj);
            }

            wunit.commit();
            return true;
        }

    } collectionModCommand;


    class DBStats : public Command {
    public:
        DBStats() : Command( "dbStats", false, "dbstats" ) {

        }

        virtual bool slaveOk() const { return true; }
        virtual bool isWriteCommandForConfigServer() const { return false; }
        virtual void help( stringstream &help ) const {
            help <<
                "Get stats on a database. Not instantaneous. Slower for databases with large "
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

        bool run(OperationContext* txn, const string& dbname, BSONObj& jsobj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl ) {
            int scale = 1;
            if ( jsobj["scale"].isNumber() ) {
                scale = jsobj["scale"].numberInt();
                if ( scale <= 0 ) {
                    errmsg = "scale has to be > 0";
                    return false;
                }
            }
            else if ( jsobj["scale"].trueValue() ) {
                errmsg = "scale has to be a number > 0";
                return false;
            }

            const string ns = parseNs(dbname, jsobj);

            // TODO: OldClientContext legacy, needs to be removed
            txn->getCurOp()->ensureStarted();
            txn->getCurOp()->setNS(dbname);

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
            }
            else {
                // TODO: OldClientContext legacy, needs to be removed
                txn->getCurOp()->enter(dbname.c_str(), db->getProfilingLevel());

                db->getStats(txn, &result, scale);
            }

            return true;
        }

    } cmdDBStats;

    /* Returns client's uri */
    class CmdWhatsMyUri : public Command {
    public:
        CmdWhatsMyUri() : Command("whatsmyuri") { }
        virtual bool slaveOk() const {
            return true;
        }
        virtual bool isWriteCommandForConfigServer() const { return false; }
        virtual void help( stringstream &help ) const {
            help << "{whatsmyuri:1}";
        }
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {} // No auth required
        virtual bool run(OperationContext* txn, const string& dbname, BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
            result << "you" << txn->getCurOp()->getRemoteString();
            return true;
        }
    } cmdWhatsMyUri;

    class AvailableQueryOptions: public Command {
    public:
        AvailableQueryOptions(): Command("availableQueryOptions",
                                         false,
                                         "availablequeryoptions") {
        }

        virtual bool slaveOk() const { return true; }
        virtual bool isWriteCommandForConfigServer() const { return false; }
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
                         BSONObjBuilder& result,
                         bool) {
            result << "options" << QueryOption_AllSupported;
            return true;
        }
    } availableQueryOptionsCmd;

    bool _execCommand(OperationContext* txn,
                      Command *c,
                      const string& dbname,
                      BSONObj& cmdObj,
                      int queryOptions,
                      std::string& errmsg,
                      BSONObjBuilder& result,
                      bool fromRepl) {

        try {
            return c->run(txn, dbname, cmdObj, queryOptions, errmsg, result, fromRepl);
        }
        catch ( SendStaleConfigException& e ){
            LOG(1) << "command failed because of stale config, can retry" << causedBy( e ) << endl;
            throw;
        }
        catch ( DBException& e ) {

            // TODO: Rethrown errors have issues here, should divorce SendStaleConfigException from the DBException tree

            stringstream ss;
            ss << "exception: " << e.what();
            result.append( "errmsg" , ss.str() );
            result.append( "code" , e.getCode() );
            return false;
        }
    }

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
        MaintenanceModeSetter() :
            maintenanceModeSet(
                    repl::getGlobalReplicationCoordinator()->setMaintenanceMode(true).isOK())
            {}
        ~MaintenanceModeSetter() {
            if (maintenanceModeSet)
                repl::getGlobalReplicationCoordinator()->setMaintenanceMode(false);
        }
    private:
        bool maintenanceModeSet;
    };


    /**
     * RAII class to optionally set an impersonated username list into the authorization session
     * for the duration of the life of this object
     */
    class ImpersonationSessionGuard {
        MONGO_DISALLOW_COPYING(ImpersonationSessionGuard);
    public:
        ImpersonationSessionGuard(AuthorizationSession* authSession,
                                  bool fieldIsPresent,
                                  const std::vector<UserName> &parsedUserNames,
                                  const std::vector<RoleName> &parsedRoleNames):
            _authSession(authSession), _impersonation(false) {
            if (fieldIsPresent) {
                massert(17317, "impersonation unexpectedly active",
                        !authSession->isImpersonating());
                authSession->setImpersonatedUserData(parsedUserNames, parsedRoleNames);
                _impersonation = true;
            }
        }
        ~ImpersonationSessionGuard() {
            if (_impersonation) {
                _authSession->clearImpersonatedUserData();
            }
        }
    private:
        AuthorizationSession* _authSession;
        bool _impersonation;
    };

    namespace {
        void appendGLEHelperData(BSONObjBuilder& bob, const Timestamp& opTime, const OID& oid) {
            BSONObjBuilder subobj(bob.subobjStart(kGLEStatsFieldName));
            subobj.append(kGLEStatsLastOpTimeFieldName, opTime);
            subobj.appendOID(kGLEStatsElectionIdFieldName, const_cast<OID*>(&oid));
            subobj.done();
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
                              Command * c ,
                              int queryOptions,
                              const char *cmdns,
                              BSONObj& cmdObj,
                              BSONObjBuilder& result,
                              bool fromRepl ) {
        std::string dbname = nsToDatabase( cmdns );
        scoped_ptr<MaintenanceModeSetter> mmSetter;

        if ( cmdObj["help"].trueValue() ) {
            txn->getCurOp()->ensureStarted();
            stringstream ss;
            ss << "help for: " << c->name << " ";
            c->help( ss );
            result.append( "help" , ss.str() );
            result.append("lockType", c->isWriteCommandForConfigServer() ? 1 : 0);
            appendCommandStatus(result, true, "");
            return;
        }

        // Handle command option impersonatedUsers and impersonatedRoles.
        // This must come before _checkAuthorization(), as there is some command parsing logic
        // in that code path that must not see the impersonated user and roles array elements.
        std::vector<UserName> parsedUserNames;
        std::vector<RoleName> parsedRoleNames;
        AuthorizationSession* authSession = txn->getClient()->getAuthorizationSession();
        bool rolesFieldIsPresent = false;
        bool usersFieldIsPresent = false;
        audit::parseAndRemoveImpersonatedRolesField(cmdObj,
                                                    authSession,
                                                    &parsedRoleNames,
                                                    &rolesFieldIsPresent);
        audit::parseAndRemoveImpersonatedUsersField(cmdObj,
                                                    authSession,
                                                    &parsedUserNames,
                                                    &usersFieldIsPresent);
        if (rolesFieldIsPresent != usersFieldIsPresent) {
            // If there is a version mismatch between the mongos and the mongod,
            // the mongos may fail to pass the role information, causing an error.
            Status s(ErrorCodes::IncompatibleAuditMetadata,
                    "Audit metadata does not include both user and role information.");
            appendCommandStatus(result, s);
            return;
        }
        ImpersonationSessionGuard impersonationSession(authSession,
                                                       usersFieldIsPresent,
                                                       parsedUserNames,
                                                       parsedRoleNames);

        Status status = _checkAuthorization(c, txn->getClient(), dbname, cmdObj, fromRepl);
        if (!status.isOK()) {
            appendCommandStatus(result, status);
            return;
        }

        repl::ReplicationCoordinator* replCoord = repl::getGlobalReplicationCoordinator();
        bool canRunHere =
            replCoord->canAcceptWritesForDatabase(dbname) ||
            c->slaveOk() ||
            ( c->slaveOverrideOk() && ( queryOptions & QueryOption_SlaveOk ) ) ||
            fromRepl;

        if ( ! canRunHere ) {
            result.append( "note" , "from execCommand" );
            if ( c->slaveOverrideOk() ) {
                appendCommandStatus(result, false, "not master and slaveOk=false");
            }
            else {
                appendCommandStatus(result, false, "not master");
            }
            return;
        }

        if (!c->maintenanceOk()
                && replCoord->getReplicationMode() == repl::ReplicationCoordinator::modeReplSet
                && !replCoord->canAcceptWritesForDatabase(dbname)
                && !replCoord->getMemberState().secondary()) {
            result.append( "note" , "from execCommand" );
            appendCommandStatus(result, false, "node is recovering");
            return;
        }

        if ( c->adminOnly() ) {
            LOG( 2 ) << "command: " << cmdObj << endl;
        }

        txn->getCurOp()->setCommand(c);

        if (c->maintenanceMode()) {
            mmSetter.reset(new MaintenanceModeSetter);
        }

        if (c->shouldAffectCommandCounter()) {
            // If !fromRepl, globalOpCounters need to be incremented.  Otherwise, replOpCounters
            // need to be incremented.
            OpCounters* opCounters = fromRepl ? &replOpCounters : &globalOpCounters;
            opCounters->gotCommand();
        }

        // Handle command option maxTimeMS.
        StatusWith<int> maxTimeMS = LiteParsedQuery::parseMaxTimeMSCommand(cmdObj);
        if (!maxTimeMS.isOK()) {
            appendCommandStatus(result, false, maxTimeMS.getStatus().reason());
            return;
        }
        if (cmdObj.hasField("$maxTimeMS")) {
            appendCommandStatus(result,
                                false,
                                "no such command option $maxTimeMS; use maxTimeMS instead");
            return;
        }

        txn->getCurOp()->setMaxTimeMicros(static_cast<unsigned long long>(maxTimeMS.getValue())
                                          * 1000);
        try {
            txn->checkForInterrupt(); // May trigger maxTimeAlwaysTimeOut fail point.
        }
        catch (UserException& e) {
            appendCommandStatus(result, e.toStatus());
            return;
        }

        std::string errmsg;
        bool retval = false;

        txn->getCurOp()->ensureStarted();

        c->_commandsExecuted.increment();

        retval = _execCommand(txn, c, dbname, cmdObj, queryOptions, errmsg, result, fromRepl);

        if ( !retval ){
            c->_commandsFailed.increment();
        }

        appendCommandStatus(result, retval, errmsg);

        // For commands from mongos, append some info to help getLastError(w) work.
        if (replCoord->getReplicationMode() == repl::ReplicationCoordinator::modeReplSet &&
                shardingState.enabled()) {
            appendGLEHelperData(
                    result,
                    repl::ReplClientInfo::forClient(txn->getClient()).getLastOp(),
                    replCoord->getElectionId());
        }
        return;
    }

    /* TODO make these all command objects -- legacy stuff here

       usage:
         abc.$cmd.findOne( { ismaster:1 } );

       returns true if ran a cmd
    */
    bool _runCommands(OperationContext* txn,
                      const char* ns,
                      BSONObj& _cmdobj,
                      BufBuilder& b,
                      BSONObjBuilder& anObjBuilder,
                      bool fromRepl, int queryOptions) {
        string dbname = nsToDatabase( ns );

        const char *p = strchr(ns, '.');
        if ( !p ) return false;
        if ( strcmp(p, ".$cmd") != 0 ) return false;

        BSONObj jsobj;
        {
            BSONElement e = _cmdobj.firstElement();
            if ( e.type() == Object && (e.fieldName()[0] == '$'
                                         ? str::equals("query", e.fieldName()+1)
                                         : str::equals("query", e.fieldName())))
            {
                jsobj = e.embeddedObject();
                if (_cmdobj.hasField("$maxTimeMS")) {
                    Command::appendCommandStatus(anObjBuilder,
                                                 false,
                                                 "cannot use $maxTimeMS query option with "
                                                    "commands; use maxTimeMS command option "
                                                    "instead");
                    BSONObj x = anObjBuilder.done();
                    b.appendBuf(x.objdata(), x.objsize());
                    return true;
                }
            }
            else {
                jsobj = _cmdobj;
            }
        }

        // Treat the command the same as if it has slaveOk bit on if it has a read
        // preference setting. This is to allow these commands to run on a secondary.
        if (Query::hasReadPreference(_cmdobj)) {
            queryOptions |= QueryOption_SlaveOk;
        }

        BSONElement e = jsobj.firstElement();

        Command * c = e.type() ? Command::findCommand( e.fieldName() ) : 0;

        if ( c ) {
            LOG(2) << "run command " << ns << ' ' << c->getRedactedCopyForLogging(_cmdobj);
            Command::execCommand(txn, c, queryOptions, ns, jsobj, anObjBuilder, fromRepl);
        }
        else {
            // In the absence of a Command object, no redaction is possible. Therefore
            // to avoid displaying potentially sensitive information in the logs,
            // we restrict the log message to the name of the unrecognized command.
            // However, the complete command object will still be echoed to the client.
            string msg = str::stream() << "no such command: " << e.fieldName();
            LOG(2) << msg;
            Command::appendCommandStatus(anObjBuilder, false, msg);
            anObjBuilder.append("code", ErrorCodes::CommandNotFound);
            anObjBuilder.append("bad cmd" , _cmdobj );
            Command::unknownCommands.increment();
        }

        BSONObj x = anObjBuilder.done();
        b.appendBuf(x.objdata(), x.objsize());

        return true;
    }

    bool runCommands(OperationContext* txn,
                     const char* ns,
                     BSONObj& jsobj,
                     CurOp& curop,
                     BufBuilder& b,
                     BSONObjBuilder& anObjBuilder,
                     bool fromRepl,
                     int queryOptions) {
        try {
            return _runCommands(txn, ns, jsobj, b, anObjBuilder, fromRepl, queryOptions);
        }
        catch (const SendStaleConfigException&){
            throw;
        }
        catch (const AssertionException& e) {
            verify( e.getCode() != SendStaleConfigCode && e.getCode() != RecvStaleConfigCode );

            Command::appendCommandStatus(anObjBuilder, e.toStatus());
            curop.debug().exceptionInfo = e.getInfo();
        }
        BSONObj x = anObjBuilder.done();
        b.appendBuf(x.objdata(), x.objsize());
        return true;
    }

} // namespace mongo
