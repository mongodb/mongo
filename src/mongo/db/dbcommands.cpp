// dbcommands.cpp

/**
*    Copyright (C) 2012 10gen Inc.
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

#include "mongo/pch.h"

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
#include "mongo/db/commands.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/commands/shutdown.h"
#include "mongo/db/db.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/storage/mmap_v1/dur_stats.h"
#include "mongo/db/index_builder.h"
#include "mongo/db/instance.h"
#include "mongo/db/introspect.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/db/kill_current_op.h"
#include "mongo/db/lasterror.h"
#include "mongo/db/ops/count.h"
#include "mongo/db/ops/insert.h"
#include "mongo/db/query/get_runner.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/repair_database.h"
#include "mongo/db/repl/is_master.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/storage/extent_manager.h"
#include "mongo/db/storage/mmap_v1/dur_transaction.h"
#include "mongo/db/storage/record.h"
#include "mongo/db/structure/catalog/namespace_details.h"
#include "mongo/db/write_concern.h"
#include "mongo/s/d_logic.h"
#include "mongo/s/d_writeback.h"
#include "mongo/s/stale_exception.h"  // for SendStaleConfigException
#include "mongo/scripting/engine.h"
#include "mongo/server.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/lruishmap.h"
#include "mongo/util/md5.hpp"

namespace mongo {

    CmdShutdown cmdShutdown;

    void CmdShutdown::help( stringstream& help ) const {
        help << "shutdown the database.  must be ran against admin db and "
             << "either (1) ran from localhost or (2) authenticated. If "
             << "this is a primary in a replica set and there is no member "
             << "within 10 seconds of its optime, it will not shutdown "
             << "without force : true.  You can also specify timeoutSecs : "
             << "N to wait N seconds for other members to catch up.";
    }

    bool CmdShutdown::run(const string& dbname, BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
        bool force = cmdObj.hasField("force") && cmdObj["force"].trueValue();

        if (!force &&
                theReplSet &&
                theReplSet->getConfig().members.size() > 1 &&
                theReplSet->isPrimary()) {
            long long timeout, now, start;
            timeout = now = start = curTimeMicros64()/1000000;
            if (cmdObj.hasField("timeoutSecs")) {
                timeout += cmdObj["timeoutSecs"].numberLong();
            }

            OpTime lastOp = theReplSet->lastOpTimeWritten;
            OpTime closest = theReplSet->lastOtherOpTime();
            long long int diff = lastOp.getSecs() - closest.getSecs();
            while (now <= timeout && (diff < 0 || diff > 10)) {
                sleepsecs(1);
                now++;

                lastOp = theReplSet->lastOpTimeWritten;
                closest = theReplSet->lastOtherOpTime();
                diff = lastOp.getSecs() - closest.getSecs();
            }

            if (diff < 0 || diff > 10) {
                errmsg = "no secondaries within 10 seconds of my optime";
                result.append("closest", closest.getSecs());
                result.append("difference", diff);
                return false;
            }

            // step down
            theReplSet->stepDown(120);

            log() << "waiting for secondaries to catch up" << endl;

            lastOp = theReplSet->lastOpTimeWritten;
            while (lastOp != closest && now - start < 60) {
                closest = theReplSet->lastOtherOpTime();

                now++;
                sleepsecs(1);
            }

            // regardless of whether they caught up, we'll shut down
        }

        writelocktry wlt( 2 * 60 * 1000 );
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

        virtual std::vector<BSONObj> stopIndexBuilds(Database* db, 
                                                     const BSONObj& cmdObj) {
            invariant(db);
            std::list<std::string> collections;
            db->namespaceIndex().getNamespaces(collections, true /* onlyCollections */);

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

        bool run(const string& dbname, BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
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
                Lock::GlobalWrite lk;
                Client::Context context(dbname);
                DurTransaction txn;

                log() << "dropDatabase " << dbname << " starting" << endl;

                stopIndexBuilds(context.db(), cmdObj);
                dropDatabase(&txn, context.db());

                log() << "dropDatabase " << dbname << " finished";

                if (!fromRepl)
                    logOp(&txn, "c",(dbname + ".$cmd").c_str(), cmdObj);
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

        virtual std::vector<BSONObj> stopIndexBuilds(Database* db, 
                                                     const BSONObj& cmdObj) {
            invariant(db);
            std::list<std::string> collections;
            db->namespaceIndex().getNamespaces(collections, true /* onlyCollections */);

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

        bool run(const string& dbname , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            BSONElement e = cmdObj.firstElement();
            if ( e.numberInt() != 1 ) {
                errmsg = "bad option";
                return false;
            }

            // SERVER-4328 todo don't lock globally. currently syncDataAndTruncateJournal is being
            // called within, and that requires a global lock i believe.
            Lock::GlobalWrite lk;
            Client::Context context( dbname );
            DurTransaction txn;

            log() << "repairDatabase " << dbname;
            std::vector<BSONObj> indexesInProg = stopIndexBuilds(context.db(), cmdObj);

            e = cmdObj.getField( "preserveClonedFilesOnFailure" );
            bool preserveClonedFilesOnFailure = e.isBoolean() && e.boolean();
            e = cmdObj.getField( "backupOriginalFiles" );
            bool backupOriginalFiles = e.isBoolean() && e.boolean();
            Status status =
                repairDatabase( &txn, dbname, preserveClonedFilesOnFailure, backupOriginalFiles );

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

        bool run(const string& dbname, BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            // Needs to be locked exclusively, because creates the system.profile collection
            // in the local database.
            //
            Lock::DBWrite dbXLock(dbname);
            Client::Context ctx(dbname);
            DurTransaction txn;

            BSONElement e = cmdObj.firstElement();
            result.append("was", ctx.db()->getProfilingLevel());
            result.append("slowms", serverGlobalParams.slowMS);

            int p = (int) e.number();
            bool ok = false;

            if ( p == -1 )
                ok = true;
            else if ( p >= 0 && p <= 2 ) {
                ok = ctx.db()->setProfilingLevel( &txn, p , errmsg );
            }

            BSONElement slow = cmdObj["slowms"];
            if ( slow.isNumber() )
                serverGlobalParams.slowMS = slow.numberInt();

            return ok;
        }
    } cmdProfile;

    class CmdGetOpTime : public Command {
    public:
        virtual bool slaveOk() const {
            return true;
        }
        virtual void help( stringstream& help ) const { help << "internal"; }
        virtual bool isWriteCommandForConfigServer() const { return false; }
        CmdGetOpTime() : Command("getoptime") { }
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {} // No auth required

        bool run(const string& dbname, BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            mutex::scoped_lock lk(OpTime::m);
            result.appendDate("optime", OpTime::now(lk).asDate());
            return true;
        }
    } cmdgetoptime;

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

        bool run(const string& dbname , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
            // This doesn't look like it requires exclusive DB lock, because it uses its own diag
            // locking, but originally the lock was set to be WRITE, so preserving the behaviour.
            //
            Lock::DBWrite dbXLock(dbname);
            Client::Context ctx(dbname);

            int was = _diaglog.setLevel( cmdObj.firstElement().numberInt() );
            _diaglog.flush();
            if (!serverGlobalParams.quiet) {
                MONGO_TLOG(0) << "CMD: diagLogging set to " << _diaglog.getLevel() << " from: " << was << endl;
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

        virtual std::vector<BSONObj> stopIndexBuilds(Database* db, 
                                                     const BSONObj& cmdObj) {
            std::string nsToDrop = db->name() + '.' + cmdObj.firstElement().valuestr();

            IndexCatalog::IndexKillCriteria criteria;
            criteria.ns = nsToDrop;
            return IndexBuilder::killMatchingIndexBuilds(db->getCollection(nsToDrop), criteria);
        }

        virtual bool run(const string& dbname , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            const string nsToDrop = dbname + '.' + cmdObj.firstElement().valuestr();
            if (!serverGlobalParams.quiet) {
                MONGO_TLOG(0) << "CMD: drop " << nsToDrop << endl;
            }

            if ( nsToDrop.find( '$' ) != string::npos ) {
                errmsg = "can't drop collection with reserved $ character in name";
                return false;
            }

            Lock::DBWrite dbXLock(dbname);
            Client::Context ctx(nsToDrop);
            DurTransaction txn;
            Database* db = ctx.db();

            Collection* coll = db->getCollection( &txn, nsToDrop );
            // If collection does not exist, short circuit and return.
            if ( !coll ) {
                errmsg = "ns not found";
                return false;
            }

            int numIndexes = coll->getIndexCatalog()->numIndexesTotal();

            stopIndexBuilds(db, cmdObj);

            result.append( "ns", nsToDrop );
            result.append( "nIndexesWas", numIndexes );

            Status s = db->dropCollection( &txn, nsToDrop );

            if ( s.isOK() ) {
                if (!fromRepl)
                    logOp(&txn, "c",(dbname + ".$cmd").c_str(), cmdObj);
                return true;
            }
            
            appendCommandStatus( result, s );

            return false;
        }
    } cmdDrop;

    /* select count(*) */
    class CmdCount : public Command {
    public:
        virtual bool isWriteCommandForConfigServer() const { return false; }
        CmdCount() : Command("count") { }
        virtual bool slaveOk() const {
            // ok on --slave setups
            return replSettings.slave == SimpleSlave;
        }
        virtual bool slaveOverrideOk() const { return true; }
        virtual bool maintenanceOk() const { return false; }
        virtual bool adminOnly() const { return false; }
        virtual void help( stringstream& help ) const { help << "count objects in collection"; }
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            ActionSet actions;
            actions.addAction(ActionType::find);
            out->push_back(Privilege(parseResourcePattern(dbname, cmdObj), actions));
        }

        virtual bool run(const string& dbname, BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
            long long skip = 0;
            if ( cmdObj["skip"].isNumber() ) {
                skip = cmdObj["skip"].numberLong();
                if ( skip < 0 ) {
                    errmsg = "skip value is negative in count query";
                    return false;
                }
            }
            else if ( cmdObj["skip"].ok() ) {
                errmsg = "skip value is not a valid number";
                return false;
            }

            const string ns = parseNs(dbname, cmdObj);

            // This acquires the DB read lock
            //
            Client::ReadContext ctx(ns);

            string err;
            int errCode;
            long long n = runCount(ns, cmdObj, err, errCode);

            long long retVal = n;
            bool ok = true;
            if ( n == -1 ) {
                retVal = 0;
                result.appendBool( "missing" , true );
            }
            else if ( n < 0 ) {
                retVal = 0;
                ok = false;
                if ( !err.empty() ) {
                    errmsg = err;
                    result.append("code", errCode);
                    return false;
                }
            }

            result.append("n", static_cast<double>(retVal));
            return ok;
        }
    } cmdCount;

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
        virtual bool run(const string& dbname , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl ) {
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

            Lock::DBWrite dbXLock(dbname);
            Client::Context ctx(ns);
            DurTransaction txn;

            // Create collection.
            return appendCommandStatus( result,
                                        userCreateNS(&txn, ctx.db(), ns.c_str(), options, !fromRepl) );
        }
    } cmdCreate;

    class CmdListDatabases : public Command {
    public:
        virtual bool slaveOk() const {
            return true;
        }
        virtual bool slaveOverrideOk() const {
            return true;
        }
        virtual bool adminOnly() const {
            return true;
        }
        virtual bool isWriteCommandForConfigServer() const { return false; }
        virtual void help( stringstream& help ) const { help << "list databases on this server"; }
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            ActionSet actions;
            actions.addAction(ActionType::listDatabases);
            out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
        }
        CmdListDatabases() : Command("listDatabases" , true ) {}
        bool run(const string& dbname , BSONObj& jsobj, int, string& errmsg, BSONObjBuilder& result, bool /*fromRepl*/) {
            vector< string > dbNames;
            getDatabaseNames( dbNames );
            vector< BSONObj > dbInfos;

            set<string> seen;
            intmax_t totalSize = 0;
            for ( vector< string >::iterator i = dbNames.begin(); i != dbNames.end(); ++i ) {
                BSONObjBuilder b;
                b.append( "name", *i );

                intmax_t size = dbSize( i->c_str() );
                b.append( "sizeOnDisk", (double) size );
                totalSize += size;
                
                {
                    Client::ReadContext rc( *i + ".system.namespaces" );
                    b.appendBool( "empty", rc.ctx().db()->isEmpty() );
                }
                
                dbInfos.push_back( b.obj() );

                seen.insert( i->c_str() );
            }

            // TODO: erh 1/1/2010 I think this is broken where
            // path != storageGlobalParams.dbpath ??
            set<string> allShortNames;
            {
                Lock::GlobalRead lk;
                dbHolder().getAllShortNames( allShortNames );
            }
            
            for ( set<string>::iterator i = allShortNames.begin(); i != allShortNames.end(); i++ ) {
                string name = *i;

                if ( seen.count( name ) )
                    continue;

                BSONObjBuilder b;
                b.append( "name" , name );
                b.append( "sizeOnDisk" , (double)1.0 );

                {
                    Client::ReadContext ctx( name );
                    b.appendBool( "empty", ctx.ctx().db()->isEmpty() );
                }

                dbInfos.push_back( b.obj() );
            }

            result.append( "databases", dbInfos );
            result.append( "totalSize", double( totalSize ) );
            return true;
        }
    } cmdListDatabases;

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

        bool run(const string& dbname , BSONObj& jsobj, int, string& errmsg, BSONObjBuilder& result, bool /*fromRepl*/) {
            Lock::GlobalWrite globalWriteLock;
            Client::Context ctx(dbname);

            try {
                return dbHolderW().closeAll(storageGlobalParams.dbpath, result, false);
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

        bool run(const string& dbname, BSONObj& jsobj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl ) {
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
            Client::ReadContext ctx(ns);
            Collection* coll = ctx.ctx().db()->getCollection(ns);

            CanonicalQuery* cq;
            if (!CanonicalQuery::canonicalize(ns, query, sort, BSONObj(), &cq).isOK()) {
                uasserted(17240, "Can't canonicalize query " + query.toString());
                return 0;
            }

            Runner* rawRunner;
            if (!getRunner(coll, cq, &rawRunner, QueryPlannerParams::NO_TABLE_SCAN).isOK()) {
                uasserted(17241, "Can't get runner for query " + query.toString());
                return 0;
            }

            auto_ptr<Runner> runner(rawRunner);

            // The runner must be registered to be informed of DiskLoc deletions and NS dropping
            // when we yield the lock below.
            const ScopedRunnerRegistration safety(runner.get());

            const ChunkVersion shardVersionAtStart = shardingState.getVersion(ns);

            BSONObj obj;
            Runner::RunnerState state;
            while (Runner::RUNNER_ADVANCED == (state = runner->getNext(&obj, NULL))) {
                BSONElement ne = obj["n"];
                verify(ne.isNumber());
                int myn = ne.numberInt();
                if ( n != myn ) {
                    if (partialOk) {
                        break; // skipped chunk is probably on another shard
                    }
                    log() << "should have chunk: " << n << " have:" << myn << endl;
                    dumpChunks( ns , query , sort );
                    uassert( 10040 ,  "chunks out of order" , n == myn );
                }

                // make a copy of obj since we access data in it while yielding
                BSONObj owned = obj.getOwned();
                int len;
                const char * data = owned["data"].binDataClean( len );

                // Save state, yield, run the MD5, and reacquire lock.
                runner->saveState();

                try {
                    dbtempreleasecond yield;

                    md5_append( &st , (const md5_byte_t*)(data) , len );
                    n++;
                }
                catch (SendStaleConfigException&) {
                    log() << "metadata changed during filemd5" << endl;
                    break;
                }

                // Have the lock again.  See if we were killed.
                if (!runner->restoreState()) {
                    if (!partialOk) {
                        uasserted(13281, "File deleted during filemd5 command");
                    }
                }

                if (!shardingState.getVersion(ns).isWriteCompatibleWith(shardVersionAtStart)) {
                    // return partial results.  Mongos will get the error at the start of the next
                    // call if it doesn't update first.
                    log() << "Config changed during filemd5 - command will resume " << endl;
                    break;
                }
            }

            if (partialOk)
                result.appendBinData("md5state", sizeof(st), BinDataGeneral, &st);

            // This must be *after* the capture of md5state since it mutates st
            md5_finish(&st, d);

            result.append( "numChunks" , n );
            result.append( "md5" , digestToString( d ) );
            return true;
        }

        void dumpChunks( const string& ns , const BSONObj& query , const BSONObj& sort ) {
            DBDirectClient client;
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

        bool run(const string& dbname, BSONObj& jsobj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl ) {
            Timer timer;

            string ns = jsobj.firstElement().String();
            BSONObj min = jsobj.getObjectField( "min" );
            BSONObj max = jsobj.getObjectField( "max" );
            BSONObj keyPattern = jsobj.getObjectField( "keyPattern" );
            bool estimate = jsobj["estimate"].trueValue();

            Client::ReadContext ctx(ns);

            Collection* collection = ctx.ctx().db()->getCollection( ns );

            if ( !collection || collection->numRecords() == 0 ) {
                result.appendNumber( "size" , 0 );
                result.appendNumber( "numObjects" , 0 );
                result.append( "millis" , timer.millis() );
                return true;
            }

            result.appendBool( "estimate" , estimate );

            auto_ptr<Runner> runner;
            if ( min.isEmpty() && max.isEmpty() ) {
                if ( estimate ) {
                    result.appendNumber( "size" , static_cast<long long>(collection->dataSize()) );
                    result.appendNumber( "numObjects",
                                         static_cast<long long>( collection->numRecords() ) );
                    result.append( "millis" , timer.millis() );
                    return 1;
                }
                runner.reset(InternalPlanner::collectionScan(ns,collection));
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

                runner.reset(InternalPlanner::indexScan(collection, idx, min, max, false));
            }

            long long avgObjSize = collection->dataSize() / collection->numRecords();

            long long maxSize = jsobj["maxSize"].numberLong();
            long long maxObjects = jsobj["maxObjects"].numberLong();

            long long size = 0;
            long long numObjects = 0;

            DiskLoc loc;
            Runner::RunnerState state;
            while (Runner::RUNNER_ADVANCED == (state = runner->getNext(NULL, &loc))) {
                if ( estimate )
                    size += avgObjSize;
                else
                    size += collection->getRecordStore()->recordFor(loc)->netLength();

                numObjects++;

                if ( ( maxSize && size > maxSize ) ||
                        ( maxObjects && numObjects > maxObjects ) ) {
                    result.appendBool( "maxReached" , true );
                    break;
                }
            }

            if (Runner::RUNNER_EOF != state) {
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

    namespace {
        long long getIndexSizeForCollection(string db, string ns, BSONObjBuilder* details=NULL, int scale = 1 ) {
            Lock::assertAtLeastReadLocked(ns);
            Client::Context ctx( ns );

            Collection* coll = ctx.db()->getCollection( ns );
            if ( !coll )
                return 0;

            IndexCatalog::IndexIterator ii =
                coll->getIndexCatalog()->getIndexIterator( true /*includeUnfinishedIndexes*/ );

            long long totalSize = 0;

            while ( ii.more() ) {
                IndexDescriptor* d = ii.next();
                string indNS = d->indexNamespace();
                Collection* indColl = ctx.db()->getCollection( indNS );
                if ( ! indColl ) {
                    log() << "error: have index descriptor ["  << indNS
                          << "] but no entry in the index collection." << endl;
                    continue;
                }
                totalSize += indColl->dataSize();
                if ( details ) {
                    long long const indexSize = indColl->dataSize() / scale;
                    details->appendNumber( d->indexName() , indexSize );
                }
            }
            return totalSize;
        }
    }

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

        bool run(const string& dbname, BSONObj& jsobj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl ) {
            const string ns = dbname + "." + jsobj.firstElement().valuestr();
            Client::ReadContext cx( ns );

            Collection* collection = cx.ctx().db()->getCollection( ns );
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

            int numExtents;
            BSONArrayBuilder extents;
            result.appendNumber( "storageSize",
                                 static_cast<long long>( collection->storageSize( &numExtents , verbose ? &extents : 0  ) / scale ) );
            result.append( "numExtents" , numExtents );
            result.append( "nindexes" , collection->getIndexCatalog()->numIndexesReady() );

            collection->appendCustomStats( &result, scale );

            BSONObjBuilder indexSizes;
            result.appendNumber( "totalIndexSize" , getIndexSizeForCollection(dbname, ns, &indexSizes, scale) / scale );
            result.append("indexSizes", indexSizes.obj());

            if ( verbose )
                result.appendArray( "extents" , extents.arr() );

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

        bool run(const string& dbname, BSONObj& jsobj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl ) {
            const string ns = dbname + "." + jsobj.firstElement().valuestr();

            Lock::DBWrite dbXLock(dbname);
            Client::Context ctx( ns );
            DurTransaction txn;

            Collection* coll = ctx.db()->getCollection( ns );
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
                else if ( str::equals( "usePowerOf2Sizes", e.fieldName() ) ) {
                    bool oldPowerOf2 = coll->isUserFlagSet(NamespaceDetails::Flag_UsePowerOf2Sizes);
                    bool newPowerOf2 = e.trueValue();

                    if ( oldPowerOf2 != newPowerOf2 ) {
                        // change userFlags
                        result.appendBool( "usePowerOf2Sizes_old", oldPowerOf2 );

                        if ( newPowerOf2 )
                            coll->setUserFlag( &txn, NamespaceDetails::Flag_UsePowerOf2Sizes );
                        else
                            coll->clearUserFlag( &txn, NamespaceDetails::Flag_UsePowerOf2Sizes );

                        result.appendBool( "usePowerOf2Sizes_new", newPowerOf2 );
                    }
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
                        coll->getIndexCatalog()->updateTTLSetting( &txn, idx, newExpireSecs.numberLong() );
                        result.appendAs( newExpireSecs , "expireAfterSeconds_new" );
                    }
                }
                else {
                    errmsg = str::stream() << "unknown option to collMod: " << e.fieldName();
                    ok = false;
                }
            }
            
            if (ok && !fromRepl)
                logOp(&txn, "c",(dbname + ".$cmd").c_str(), jsobj);

            return ok;
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

        bool run(const string& dbname, BSONObj& jsobj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl ) {
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
            list<string> collections;

            Client::ReadContext ctx(ns);
            Database* d = ctx.ctx().db();

            if ( d && ( d->isEmpty() || d->getExtentManager().numFiles() == 0 ) )
                d = NULL;

            if ( d )
                d->namespaceIndex().getNamespaces( collections );

            long long ncollections = 0;
            long long objects = 0;
            long long size = 0;
            long long storageSize = 0;
            long long numExtents = 0;
            long long indexes = 0;
            long long indexSize = 0;

            for (list<string>::const_iterator it = collections.begin(); it != collections.end(); ++it) {
                const string ns = *it;

                Collection* collection = d->getCollection( ns );
                if ( !collection ) {
                    errmsg = "missing ns: ";
                    errmsg += ns;
                    return false;
                }

                ncollections += 1;
                objects += collection->numRecords();
                size += collection->dataSize();

                int temp;
                storageSize += collection->storageSize( &temp, NULL );
                numExtents += temp;

                indexes += collection->getIndexCatalog()->numIndexesTotal();
                indexSize += getIndexSizeForCollection(dbname, ns);
            }

            result.append      ( "db" , dbname );
            result.appendNumber( "collections" , ncollections );
            result.appendNumber( "objects" , objects );
            result.append      ( "avgObjSize" , objects == 0 ? 0 : double(size) / double(objects) );
            result.appendNumber( "dataSize" , size / scale );
            result.appendNumber( "storageSize" , storageSize / scale);
            result.appendNumber( "numExtents" , numExtents );
            result.appendNumber( "indexes" , indexes );
            result.appendNumber( "indexSize" , indexSize / scale );
            if ( d ) {
                result.appendNumber( "fileSize" , d->fileSize() / scale );
                result.appendNumber( "nsSizeMB", (int) d->namespaceIndex().fileLength() / 1024 / 1024 );
            }
            else {
                result.appendNumber( "fileSize" , 0 );
            }

            BSONObjBuilder dataFileVersion( result.subobjStart( "dataFileVersion" ) );
            if ( d ) {
                int major, minor;
                d->getFileFormat( &major, &minor );
                dataFileVersion.append( "major", major );
                dataFileVersion.append( "minor", minor );
            }
            dataFileVersion.done();

            if ( d ){
                int freeListSize = 0;
                int64_t freeListSpace = 0;
                d->getExtentManager().freeListStats( &freeListSize, &freeListSpace );

                BSONObjBuilder extentFreeList( result.subobjStart( "extentFreeList" ) );
                extentFreeList.append( "num", freeListSize );
                extentFreeList.appendNumber( "totalSize",
                                             static_cast<long long>( freeListSpace / scale ) );
                extentFreeList.done();
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
        virtual bool run(const string& dbname, BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
            BSONObj info = cc().curop()->info();
            result << "you" << info[ "client" ];
            return true;
        }
    } cmdWhatsMyUri;


    bool _execCommand(Command *c,
                      const string& dbname,
                      BSONObj& cmdObj,
                      int queryOptions,
                      std::string& errmsg,
                      BSONObjBuilder& result,
                      bool fromRepl) {

        try {
            return c->run(dbname, cmdObj, queryOptions, errmsg, result, fromRepl);
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
        MaintenanceModeSetter() : maintenanceModeSet(theReplSet->setMaintenanceMode(true)) {}
        ~MaintenanceModeSetter() {
            if(maintenanceModeSet)
                theReplSet->setMaintenanceMode(false);
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
                                  const std::vector<UserName> &parsedUserNames) :
            _authSession(authSession), _impersonation(false) {
            if (fieldIsPresent) {
                massert(17317, "impersonation unexpectedly active", 
                        !authSession->isImpersonating());
                authSession->setImpersonatedUserNames(parsedUserNames);
                _impersonation = true;
            }
        }
        ~ImpersonationSessionGuard() {
            if (_impersonation) {
                _authSession->clearImpersonatedUserNames();
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
    void Command::execCommand(Command * c ,
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

        // Handle command option impersonatedUsers.
        // This must come before _checkAuthorization(), as there is some command parsing logic
        // in that code path that must not see the impersonated user array element.
        std::vector<UserName> parsedUserNames;
        AuthorizationSession* authSession = client.getAuthorizationSession();
        bool fieldIsPresent = false;
        audit::parseAndRemoveImpersonatedUserField(cmdObj, authSession,
                                                   &parsedUserNames, &fieldIsPresent);
        ImpersonationSessionGuard impersonationSession(authSession, 
                                                       fieldIsPresent, 
                                                       parsedUserNames);

        Status status = _checkAuthorization(c, &client, dbname, cmdObj, fromRepl);
        if (!status.isOK()) {
            appendCommandStatus(result, status);
            return;
        }

        bool canRunHere =
            isMaster( dbname.c_str() ) ||
            c->slaveOk() ||
            ( c->slaveOverrideOk() && ( queryOptions & QueryOption_SlaveOk ) ) ||
            fromRepl;

        if ( ! canRunHere ) {
            result.append( "note" , "from execCommand" );
            appendCommandStatus(result, false, "not master");
            return;
        }

        if ( ! c->maintenanceOk() && theReplSet && ! isMaster( dbname.c_str() ) && ! theReplSet->isSecondary() ) {
            result.append( "note" , "from execCommand" );
            appendCommandStatus(result, false, "node is recovering");
            return;
        }

        if ( c->adminOnly() ) {
            LOG( 2 ) << "command: " << cmdObj << endl;
        }

        client.curop()->setCommand(c);

        if (c->maintenanceMode() && theReplSet) {
            mmSetter.reset(new MaintenanceModeSetter());
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
            killCurrentOp.checkForInterrupt(); // May trigger maxTimeAlwaysTimeOut fail point.
        }
        catch (UserException& e) {
            appendCommandStatus(result, e.toStatus());
            return;
        }

        std::string errmsg;
        bool retval = false;

        client.curop()->ensureStarted();

        retval = _execCommand(c, dbname, cmdObj, queryOptions, errmsg, result, fromRepl);

        appendCommandStatus(result, retval, errmsg);
        
        // For commands from mongos, append some info to help getLastError(w) work.
        if (theReplSet) {
            // Detect mongos connections by looking for setShardVersion to have been run previously
            // on this connection.
            if (shardingState.needCollectionMetadata(dbname)) {
                appendGLEHelperData(result, client.getLastOp(), theReplSet->getElectionId());
            }
        }
        return;
    }


    /* TODO make these all command objects -- legacy stuff here

       usage:
         abc.$cmd.findOne( { ismaster:1 } );

       returns true if ran a cmd
    */
    bool _runCommands(const char *ns, BSONObj& _cmdobj, BufBuilder &b, BSONObjBuilder& anObjBuilder, bool fromRepl, int queryOptions) {
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
            Command::execCommand(c, client, queryOptions, ns, jsobj, anObjBuilder, fromRepl);
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
