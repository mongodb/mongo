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

#include "mongo/platform/basic.h"

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
#include "mongo/db/clientcursor.h"
#include "mongo/db/catalog/collection_catalog_entry.h"
#include "mongo/db/catalog/database_catalog_entry.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/commands/shutdown.h"
#include "mongo/db/db.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/global_environment_d.h"
#include "mongo/db/index_builder.h"
#include "mongo/db/instance.h"
#include "mongo/db/introspect.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/db/lasterror.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/ops/insert.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/repair_database.h"
#include "mongo/db/repl/repl_coordinator_global.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/mmap_v1/dur_stats.h"
#include "mongo/db/write_concern.h"
#include "mongo/s/d_logic.h"
#include "mongo/s/d_writeback.h"
#include "mongo/s/stale_exception.h"  // for SendStaleConfigException
#include "mongo/scripting/engine.h"
#include "mongo/server.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/md5.hpp"

namespace mongo {

    MONGO_LOG_DEFAULT_COMPONENT_FILE(::mongo::logger::LogComponent::kCommands);

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

        repl::ReplicationCoordinator* replCoord = repl::getGlobalReplicationCoordinator();
        if (!force &&
                replCoord->getReplicationMode() == repl::ReplicationCoordinator::modeReplSet &&
                replCoord->getCurrentMemberState().primary()) {
            long long timeoutSecs = 0;
            if (cmdObj.hasField("timeoutSecs")) {
                timeoutSecs = cmdObj["timeoutSecs"].numberLong();
            }

            Status status = repl::getGlobalReplicationCoordinator()->stepDownAndWaitForSecondary(
                    txn,
                    repl::ReplicationCoordinator::Milliseconds(timeoutSecs * 1000),
                    repl::ReplicationCoordinator::Milliseconds(120 * 1000),
                    repl::ReplicationCoordinator::Milliseconds(60 * 1000));
            if (!status.isOK() && status.code() != ErrorCodes::NotMaster) { // ignore not master
                return appendCommandStatus(result, status);
            }
        }

        writelocktry wlt(txn->lockState(), 2 * 60 * 1000);
        uassert( 13455 , "dbexit timed out getting lock" , wlt.got() );
        return shutdownHelper();
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
                    IndexBuilder::killMatchingIndexBuilds(db->getCollection(opCtx, ns), criteria);
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
            BSONElement e = cmdObj.firstElement();
            int p = (int) e.number();
            if ( p != 1 ) {
                errmsg = "have to pass 1 as db parameter";
                return false;
            }

            {

                // this is suboptimal but syncDataAndTruncateJournal is called from dropDatabase,
                // and that may need a global lock.
                Lock::GlobalWrite lk(txn->lockState());
                Client::Context context(txn, dbname);
                WriteUnitOfWork wunit(txn->recoveryUnit());

                log() << "dropDatabase " << dbname << " starting" << endl;

                stopIndexBuilds(txn, context.db(), cmdObj);
                dropDatabase(txn, context.db());

                log() << "dropDatabase " << dbname << " finished";

                if (!fromRepl)
                    repl::logOp(txn, "c",(dbname + ".$cmd").c_str(), cmdObj);

                wunit.commit();
            }

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
                    IndexBuilder::killMatchingIndexBuilds(db->getCollection(opCtx, ns), criteria);
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

            // SERVER-4328 todo don't lock globally. currently syncDataAndTruncateJournal is being
            // called within, and that requires a global lock i believe.
            Lock::GlobalWrite lk(txn->lockState());
            Client::Context context(txn,  dbname );

            log() << "repairDatabase " << dbname;
            std::vector<BSONObj> indexesInProg = stopIndexBuilds(txn, context.db(), cmdObj);

            e = cmdObj.getField( "preserveClonedFilesOnFailure" );
            bool preserveClonedFilesOnFailure = e.isBoolean() && e.boolean();
            e = cmdObj.getField( "backupOriginalFiles" );
            bool backupOriginalFiles = e.isBoolean() && e.boolean();

            Status status = getGlobalEnvironment()->getGlobalStorageEngine()->repairDatabase(
                txn, dbname, preserveClonedFilesOnFailure, backupOriginalFiles );

            IndexBuilder::restoreIndexes(indexesInProg);

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

        bool run(OperationContext* txn, const string& dbname, BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            // Needs to be locked exclusively, because creates the system.profile collection
            // in the local database.
            //
            Lock::DBWrite dbXLock(txn->lockState(), dbname);
            WriteUnitOfWork wunit(txn->recoveryUnit());
            Client::Context ctx(txn, dbname);

            BSONElement e = cmdObj.firstElement();
            result.append("was", ctx.db()->getProfilingLevel());
            result.append("slowms", serverGlobalParams.slowMS);

            int p = (int) e.number();
            bool ok = false;

            if ( p == -1 )
                ok = true;
            else if ( p >= 0 && p <= 2 ) {
                ok = ctx.db()->setProfilingLevel( txn, p , errmsg );
            }

            BSONElement slow = cmdObj["slowms"];
            if ( slow.isNumber() )
                serverGlobalParams.slowMS = slow.numberInt();

            wunit.commit();
            return ok;
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
            // This doesn't look like it requires exclusive DB lock, because it uses its own diag
            // locking, but originally the lock was set to be WRITE, so preserving the behaviour.
            //
            Lock::DBWrite dbXLock(txn->lockState(), dbname);
            Client::Context ctx(txn, dbname);

            int was = _diaglog.setLevel( cmdObj.firstElement().numberInt() );
            _diaglog.flush();
            if (!serverGlobalParams.quiet) {
                LOG(0) << "CMD: diagLogging set to " << _diaglog.getLevel() << " from: " << was << endl;
            }
            result.append( "was" , was );
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
            std::string nsToDrop = db->name() + '.' + cmdObj.firstElement().valuestr();

            IndexCatalog::IndexKillCriteria criteria;
            criteria.ns = nsToDrop;
            return IndexBuilder::killMatchingIndexBuilds(db->getCollection(opCtx, nsToDrop), criteria);
        }

        virtual bool run(OperationContext* txn, const string& dbname , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            const string nsToDrop = dbname + '.' + cmdObj.firstElement().valuestr();
            if (!serverGlobalParams.quiet) {
                LOG(0) << "CMD: drop " << nsToDrop << endl;
            }

            if ( nsToDrop.find( '$' ) != string::npos ) {
                errmsg = "can't drop collection with reserved $ character in name";
                return false;
            }

            Lock::DBWrite dbXLock(txn->lockState(), dbname);
            WriteUnitOfWork wunit(txn->recoveryUnit());
            Client::Context ctx(txn, nsToDrop);
            Database* db = ctx.db();

            Collection* coll = db->getCollection( txn, nsToDrop );
            // If collection does not exist, short circuit and return.
            if ( !coll ) {
                errmsg = "ns not found";
                return false;
            }

            int numIndexes = coll->getIndexCatalog()->numIndexesTotal();

            stopIndexBuilds(txn, db, cmdObj);

            result.append( "ns", nsToDrop );
            result.append( "nIndexesWas", numIndexes );

            Status s = db->dropCollection( txn, nsToDrop );

            if ( !s.isOK() ) {
                return appendCommandStatus( result, s );
            }
            
            if ( !fromRepl ) {
                repl::logOp(txn, "c",(dbname + ".$cmd").c_str(), cmdObj);
            }
            wunit.commit();
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

            const string ns = dbname + '.' + firstElt.valuestr();

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

            Lock::DBWrite dbXLock(txn->lockState(), dbname);
            WriteUnitOfWork wunit(txn->recoveryUnit());
            Client::Context ctx(txn, ns);

            // Create collection.
            status =  userCreateNS(txn, ctx.db(), ns.c_str(), options, !fromRepl);
            if ( !status.isOK() ) {
                return appendCommandStatus( result, status );
            }

            wunit.commit();
            return true;
        }
    } cmdCreate;


    /* note an access to a database right after this will open it back up - so this is mainly
       for diagnostic purposes.
       */
    class CmdCloseAllDatabases : public Command {
    public:
        virtual void help( stringstream& help ) const { 
            help << "Close all database files." << endl 
                << "A new request will cause an immediate reopening; thus, this is mostly for testing purposes.";
        }

        virtual bool adminOnly() const { return true; }
        virtual bool slaveOk() const { return false; }
        virtual bool isWriteCommandForConfigServer() const { return true; }
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            ActionSet actions;
            actions.addAction(ActionType::closeAllDatabases);
            out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
        }

        CmdCloseAllDatabases() : Command( "closeAllDatabases" ) {

        }

        bool run(OperationContext* txn, const string& dbname , BSONObj& jsobj, int, string& errmsg, BSONObjBuilder& result, bool /*fromRepl*/) {
            Lock::GlobalWrite globalWriteLock(txn->lockState());
            // No WriteUnitOfWork necessary, as no actual writes happen.
            Client::Context ctx(txn, dbname);

            try {
                return dbHolder().closeAll(txn, result, false);
            }
            catch(DBException&) { 
                throw;
            }
            catch(...) { 
                log() << "ERROR uncaught exception in command closeAllDatabases" << endl;
                errmsg = "unexpected uncaught exception";
                return false;
            }
        }

    } cmdCloseAllDatabases;

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

            // Check shard version at startup.
            // This will throw before we've done any work if shard version is outdated
            Client::ReadContext ctx(txn, ns);
            Collection* coll = ctx.ctx().db()->getCollection(txn, ns);

            CanonicalQuery* cq;
            if (!CanonicalQuery::canonicalize(ns, query, sort, BSONObj(), &cq).isOK()) {
                uasserted(17240, "Can't canonicalize query " + query.toString());
                return 0;
            }

            PlanExecutor* rawExec;
            if (!getExecutor(txn, coll, cq, &rawExec, QueryPlannerParams::NO_TABLE_SCAN).isOK()) {
                uasserted(17241, "Can't get executor for query " + query.toString());
                return 0;
            }

            auto_ptr<PlanExecutor> exec(rawExec);

            // The executor must be registered to be informed of DiskLoc deletions and NS dropping
            // when we yield the lock below.
            const ScopedExecutorRegistration safety(exec.get());

            const ChunkVersion shardVersionAtStart = shardingState.getVersion(ns);

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

                // make a copy of obj since we access data in it while yielding
                BSONObj owned = obj.getOwned();
                int len;
                const char * data = owned["data"].binDataClean( len );

                md5_append( &st , (const md5_byte_t*)(data) , len );
                n++;
            }

            if (partialOk)
                result.appendBinData("md5state", sizeof(st), BinDataGeneral, &st);

            // This must be *after* the capture of md5state since it mutates st
            md5_finish(&st, d);

            result.append( "numChunks" , n );
            result.append( "md5" , digestToString( d ) );
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

            Client::ReadContext ctx(txn, ns);

            Collection* collection = ctx.ctx().db()->getCollection( txn, ns );

            if ( !collection || collection->numRecords() == 0 ) {
                result.appendNumber( "size" , 0 );
                result.appendNumber( "numObjects" , 0 );
                result.append( "millis" , timer.millis() );
                return true;
            }

            result.appendBool( "estimate" , estimate );

            auto_ptr<PlanExecutor> exec;
            if ( min.isEmpty() && max.isEmpty() ) {
                if ( estimate ) {
                    result.appendNumber( "size" , static_cast<long long>(collection->dataSize()) );
                    result.appendNumber( "numObjects",
                                         static_cast<long long>( collection->numRecords() ) );
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
                    collection->getIndexCatalog()->findIndexByPrefix( keyPattern, true );  /* require single key */

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

            long long avgObjSize = collection->dataSize() / collection->numRecords();

            long long maxSize = jsobj["maxSize"].numberLong();
            long long maxObjects = jsobj["maxObjects"].numberLong();

            long long size = 0;
            long long numObjects = 0;

            DiskLoc loc;
            PlanExecutor::ExecState state;
            while (PlanExecutor::ADVANCED == (state = exec->getNext(NULL, &loc))) {
                if ( estimate )
                    size += avgObjSize;
                else
                    size += collection->getRecordStore()->dataFor(loc).size();

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
            const string ns = dbname + "." + jsobj.firstElement().valuestr();
            Client::ReadContext cx(txn, ns);
            Database* db = cx.ctx().db();
            Collection* collection = db->getCollection( txn, ns );
            if ( !collection ) {
                errmsg = "Collection [" + ns + "] not found.";
                return false;
            }

            result.append( "ns" , ns.c_str() );

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

            long long size = collection->dataSize() / scale;
            long long numRecords = collection->numRecords();
            result.appendNumber( "count" , numRecords );
            result.appendNumber( "size" , size );
            if( numRecords )
                result.append( "avgObjSize" , collection->averageObjectSize() );

            result.appendNumber( "storageSize",
                                 static_cast<long long>(collection->getRecordStore()->storageSize( txn, &result,
                                                                                                   verbose ? 1 : 0 ) ) / 
                                 scale );
            result.append( "nindexes" , collection->getIndexCatalog()->numIndexesReady() );

            collection->getRecordStore()->appendCustomStats( txn, &result, scale );

            BSONObjBuilder indexSizes;
            result.appendNumber( "totalIndexSize" , db->getIndexSizeForCollection(txn,
                                                                                  collection,
                                                                                  &indexSizes,
                                                                                  scale) / scale );
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
            const string ns = dbname + "." + jsobj.firstElement().valuestr();

            Lock::DBWrite dbXLock(txn->lockState(), dbname);
            WriteUnitOfWork wunit(txn->recoveryUnit());
            Client::Context ctx(txn,  ns );

            Collection* coll = ctx.db()->getCollection( txn, ns );
            if ( !coll ) {
                errmsg = "ns does not exist";
                return false;
            }

            bool ok = true;

            BSONForEach( e, jsobj ) {
                if ( str::equals( "collMod", e.fieldName() ) ) {
                    // no-op
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

                    IndexDescriptor* idx = coll->getIndexCatalog()->findIndexByKeyPattern( keyPattern );
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
                        // change expireAfterSeconds
                        result.appendAs( oldExpireSecs, "expireAfterSeconds_old" );
                        coll->getCatalogEntry()->updateTTLSetting( txn,
                                                                   idx->indexName(),
                                                                   newExpireSecs.numberLong() );
                        result.appendAs( newExpireSecs , "expireAfterSeconds_new" );
                    }
                }
                else {
                    Status s = coll->getRecordStore()->setCustomOption( txn, e, &result );
                    if ( s.isOK() ) {
                        // no-op
                    }
                    else if ( s.code() == ErrorCodes::InvalidOptions ) {
                        errmsg = str::stream() << "unknown option to collMod: " << e.fieldName();
                        ok = false;
                    }
                    else {
                        return appendCommandStatus( result, s );
                    }
                }
            }

            if (!ok) {
                return false;
            }
            
            if (!fromRepl) {
                repl::logOp(txn, "c",(dbname + ".$cmd").c_str(), jsobj);
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
                "Get stats on a database. Not instantaneous. Slower for databases with large .ns files.\n" << 
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

            Client::ReadContext ctx(txn, ns);
            Database* d = ctx.ctx().db();

            d->getStats( txn, &result, scale );

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

    /* Sometimes we cannot set maintenance mode, in which case the call to setMaintenanceMode will
       return false.  This class does not treat that case as an error which means that anybody 
       using it is assuming it is ok to continue execution without maintenance mode.  This 
       assumption needs to be audited and documented. */
    class MaintenanceModeSetter {
    public:
        MaintenanceModeSetter(OperationContext* txn) :
            _txn(txn),
            maintenanceModeSet(
                    repl::getGlobalReplicationCoordinator()->setMaintenanceMode(txn, true))
            {}
        ~MaintenanceModeSetter() {
            if (maintenanceModeSet)
                repl::getGlobalReplicationCoordinator()->setMaintenanceMode(_txn, false);
        } 
    private:
        // Not owned.
        OperationContext* _txn;
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
        void appendGLEHelperData(BSONObjBuilder& bob, const OpTime& opTime, const OID& oid) {
            BSONObjBuilder subobj(bob.subobjStart(kGLEStatsFieldName));
            subobj.appendTimestamp(kGLEStatsLastOpTimeFieldName, opTime.asDate());
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
                              Client& client,
                              int queryOptions,
                              const char *cmdns,
                              BSONObj& cmdObj,
                              BSONObjBuilder& result,
                              bool fromRepl ) {
        std::string dbname = nsToDatabase( cmdns );
        scoped_ptr<MaintenanceModeSetter> mmSetter;

        if ( cmdObj["help"].trueValue() ) {
            client.curop()->ensureStarted();
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
        AuthorizationSession* authSession = client.getAuthorizationSession();
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

        Status status = _checkAuthorization(c, &client, dbname, cmdObj, fromRepl);
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
            appendCommandStatus(result, false, "not master");
            return;
        }

        if (!c->maintenanceOk()
                && replCoord->getReplicationMode() == repl::ReplicationCoordinator::modeReplSet
                && !replCoord->canAcceptWritesForDatabase(dbname)
                && !replCoord->getCurrentMemberState().secondary()) {
            result.append( "note" , "from execCommand" );
            appendCommandStatus(result, false, "node is recovering");
            return;
        }

        if ( c->adminOnly() ) {
            LOG( 2 ) << "command: " << cmdObj << endl;
        }

        client.curop()->setCommand(c);

        if (c->maintenanceMode() &&
                repl::getGlobalReplicationCoordinator()->getReplicationMode() ==
                        repl::ReplicationCoordinator::modeReplSet) {
            mmSetter.reset(new MaintenanceModeSetter(txn));
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

        client.curop()->setMaxTimeMicros(static_cast<unsigned long long>(maxTimeMS.getValue())
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

        client.curop()->ensureStarted();

        retval = _execCommand(txn, c, dbname, cmdObj, queryOptions, errmsg, result, fromRepl);

        appendCommandStatus(result, retval, errmsg);
        
        // For commands from mongos, append some info to help getLastError(w) work.
        if (replCoord->getReplicationMode() == repl::ReplicationCoordinator::modeReplSet) {
            // Detect mongos connections by looking for setShardVersion to have been run previously
            // on this connection.
            if (shardingState.needCollectionMetadata(dbname)) {
                appendGLEHelperData(result, client.getLastOp(), replCoord->getElectionId());
            }
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

        LOG(2) << "run command " << ns << ' ' << _cmdobj << endl;

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

        Client& client = cc();

        BSONElement e = jsobj.firstElement();

        Command * c = e.type() ? Command::findCommand( e.fieldName() ) : 0;

        if ( c ) {
            Command::execCommand(txn, c, client, queryOptions, ns, jsobj, anObjBuilder, fromRepl);
        }
        else {
            Command::appendCommandStatus(anObjBuilder,
                                         false,
                                         str::stream() << "no such cmd: " << e.fieldName());
            anObjBuilder.append("code", ErrorCodes::CommandNotFound);
            anObjBuilder.append("bad cmd" , _cmdobj );
        }

        BSONObj x = anObjBuilder.done();
        b.appendBuf(x.objdata(), x.objsize());

        return true;
    }

} // namespace mongo
