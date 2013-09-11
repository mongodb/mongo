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

/* SHARDING: 
   I believe this file is for mongod only.
   See s/commands_public.cpp for mongos.
*/

#include "mongo/pch.h"

#include <time.h>

#include "mongo/base/init.h"
#include "mongo/base/status.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/background.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/commands/shutdown.h"
#include "mongo/db/db.h"
#include "mongo/db/dur_stats.h"
#include "mongo/db/index_builder.h"
#include "mongo/db/index_update.h"
#include "mongo/db/instance.h"
#include "mongo/db/introspect.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/db/kill_current_op.h"
#include "mongo/db/lasterror.h"
#include "mongo/db/ops/count.h"
#include "mongo/db/pdfile.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/query_optimizer.h"
#include "mongo/db/repl/is_master.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/write_concern.h"
#include "mongo/s/d_writeback.h"
#include "mongo/s/stale_exception.h"  // for SendStaleConfigException
#include "mongo/scripting/engine.h"
#include "mongo/server.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/lruishmap.h"
#include "mongo/util/md5.hpp"

namespace mongo {

    extern FailPoint maxTimeAlwaysTimeOut;

    /* reset any errors so that getlasterror comes back clean.

       useful before performing a long series of operations where we want to
       see if any of the operations triggered an error, but don't want to check
       after each op as that woudl be a client/server turnaround.
    */
    class CmdResetError : public Command {
    public:
        virtual LockType locktype() const { return NONE; }
        virtual bool logTheOp() {
            return false;
        }
        virtual bool slaveOk() const {
            return true;
        }
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {} // No auth required
        virtual void help( stringstream& help ) const {
            help << "reset error state (used with getpreverror)";
        }
        CmdResetError() : Command("resetError", false, "reseterror") {}
        bool run(const string& db, BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            LastError *le = lastError.get();
            verify( le );
            le->reset();
            return true;
        }
    } cmdResetError;

    /* set by replica sets if specified in the configuration.
       a pointer is used to avoid any possible locking issues with lockless reading (see below locktype() is NONE
       and would like to keep that)
       (for now, it simply orphans any old copy as config changes should be extremely rare).
       note: once non-null, never goes to null again.
    */
    BSONObj *getLastErrorDefault = 0;

    class CmdGetLastError : public Command {
    public:
        CmdGetLastError() : Command("getLastError", false, "getlasterror") { }
        virtual LockType locktype() const { return NONE;  }
        virtual bool logTheOp()           { return false; }
        virtual bool slaveOk() const      { return true;  }
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {} // No auth required
        virtual void help( stringstream& help ) const {
            help << "return error status of the last operation on this connection\n"
                 << "options:\n"
                 << "  { fsync:true } - fsync before returning, or wait for journal commit if running with --journal\n"
                 << "  { j:true } - wait for journal commit if running with --journal\n"
                 << "  { w:n } - await replication to n servers (including self) before returning\n"
                 << "  { w:'majority' } - await replication to majority of set\n"
                 << "  { wtimeout:m} - timeout for w in m milliseconds";
        }
        bool run(const string& dbname, BSONObj& _cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            LastError *le = lastError.disableForCommand();

            bool err = false;

            if ( le->nPrev != 1 ) {
                err = LastError::noError.appendSelf( result , false );
                le->appendSelfStatus( result );
            }
            else {
                err = le->appendSelf( result , false );
            }

            Client& c = cc();
            c.appendLastOp( result );

            result.appendNumber( "connectionId" , c.getConnectionId() ); // for sharding; also useful in general for debugging

            BSONObj cmdObj = _cmdObj;
            {
                BSONObj::iterator i(_cmdObj);
                i.next();
                if( !i.more() ) {
                    /* empty, use default */
                    BSONObj *def = getLastErrorDefault;
                    if( def )
                        cmdObj = *def;
                }
            }

            return waitForWriteConcern(cmdObj, err, &result, &errmsg);
        }

    } cmdGetLastError;

    class CmdGetPrevError : public Command {
    public:
        virtual LockType locktype() const { return NONE; }
        virtual bool logTheOp() {
            return false;
        }
        virtual void help( stringstream& help ) const {
            help << "check for errors since last reseterror commandcal";
        }
        virtual bool slaveOk() const {
            return true;
        }
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {} // No auth required
        CmdGetPrevError() : Command("getPrevError", false, "getpreverror") {}
        bool run(const string& dbname, BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            LastError *le = lastError.disableForCommand();
            le->appendSelf( result );
            if ( le->valid )
                result.append( "nPrev", le->nPrev );
            else
                result.append( "nPrev", -1 );
            return true;
        }
    } cmdGetPrevError;

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
        virtual bool logTheOp() {
            return true;
        }
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
            out->push_back(Privilege(dbname, actions));
        }

        // this is suboptimal but syncDataAndTruncateJournal is called from dropDatabase, and that 
        // may need a global lock.
        virtual bool lockGlobally() const { return true; }

        virtual LockType locktype() const { return WRITE; }

        virtual std::vector<BSONObj> stopIndexBuilds(const std::string& dbname, 
                                                     const BSONObj& cmdObj) {
            std::string systemIndexes = dbname+".system.indexes";
            std::string toDeleteRegex = "^"+dbname+"\\.";
            BSONObj criteria = BSON("ns" << systemIndexes <<
                                    "op" << "insert" <<
                                    "insert.ns" << BSON("$regex" << toDeleteRegex));
            return IndexBuilder::killMatchingIndexBuilds(criteria);
        }

        CmdDropDatabase() : Command("dropDatabase") {}
        bool run(const string& dbname, BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            // disallow dropping the config database
            if ( cmdLine.configsvr && ( dbname == "config" ) ) {
                errmsg = "Cannot drop 'config' database if mongod started with --configsvr";
                return false;
            }
            BSONElement e = cmdObj.firstElement();
            log() << "dropDatabase " << dbname << " starting" << endl;
            int p = (int) e.number();
            if ( p != 1 )
                return false;
            stopIndexBuilds(dbname, cmdObj);
            dropDatabase(dbname);
            result.append( "dropped" , dbname );
            log() << "dropDatabase " << dbname << " finished" << endl;
            return true;
        }
    } cmdDropDatabase;

    class CmdRepairDatabase : public Command {
    public:
        virtual bool logTheOp() {
            return false;
        }
        virtual bool slaveOk() const {
            return true;
        }
        virtual bool maintenanceMode() const { return true; }
        virtual void help( stringstream& help ) const {
            help << "repair database.  also compacts. note: slow.";
        }
        virtual LockType locktype() const { return WRITE; }
        // SERVER-4328 todo don't lock globally. currently syncDataAndTruncateJournal is being called within, and that requires a global lock i believe.
        virtual bool lockGlobally() const { return true; }
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            ActionSet actions;
            actions.addAction(ActionType::repairDatabase);
            out->push_back(Privilege(dbname, actions));
        }
        CmdRepairDatabase() : Command("repairDatabase") {}

        virtual std::vector<BSONObj> stopIndexBuilds(const std::string& dbname, 
                                                     const BSONObj& cmdObj) {
            std::string systemIndexes = dbname + ".system.indexes";
            BSONObj criteria = BSON("ns" << systemIndexes << "op" << "insert");
            return IndexBuilder::killMatchingIndexBuilds(criteria);
        }

        bool run(const string& dbname , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            BSONElement e = cmdObj.firstElement();
            log() << "repairDatabase " << dbname << endl;
            int p = (int) e.number();

            if ( p != 1 ) {
                errmsg = "bad option";
                return false;
            }

            std::vector<BSONObj> indexesInProg = stopIndexBuilds(dbname, cmdObj);

            e = cmdObj.getField( "preserveClonedFilesOnFailure" );
            bool preserveClonedFilesOnFailure = e.isBoolean() && e.boolean();
            e = cmdObj.getField( "backupOriginalFiles" );
            bool backupOriginalFiles = e.isBoolean() && e.boolean();
            bool ok =
                repairDatabase( dbname, errmsg, preserveClonedFilesOnFailure, backupOriginalFiles );

            IndexBuilder::restoreIndexes(dbname+".system.indexes", indexesInProg);

            return ok;
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
            help << "http://dochub.mongodb.org/core/databaseprofiler";
        }
        virtual LockType locktype() const { return WRITE; }
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            ActionSet actions;
            actions.addAction(ActionType::profileEnable);
            out->push_back(Privilege(dbname, actions));
        }
        CmdProfile() : Command("profile") {}
        bool run(const string& dbname, BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            BSONElement e = cmdObj.firstElement();
            result.append("was", cc().database()->getProfilingLevel());
            result.append("slowms", cmdLine.slowMS );

            int p = (int) e.number();
            bool ok = false;

            if ( p == -1 )
                ok = true;
            else if ( p >= 0 && p <= 2 ) {
                ok = cc().database()->setProfilingLevel( p , errmsg );
            }

            BSONElement slow = cmdObj["slowms"];
            if ( slow.isNumber() )
                cmdLine.slowMS = slow.numberInt();

            return ok;
        }
    } cmdProfile;

    class CmdGetOpTime : public Command {
    public:
        virtual bool slaveOk() const {
            return true;
        }
        virtual void help( stringstream& help ) const { help << "internal"; }
        virtual LockType locktype() const { return NONE; }
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

    /*
    class Cmd : public Command {
    public:
        Cmd() : Command("") { }
        bool adminOnly() const { return true; }
        bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result) {
            return true;
        }
    } cmd;
    */

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
        virtual LockType locktype() const { return WRITE; }
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            ActionSet actions;
            actions.addAction(ActionType::diagLogging);
            out->push_back(Privilege(AuthorizationManager::SERVER_RESOURCE_NAME, actions));
        }
        bool run(const string& dbname , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
            int was = _diaglog.setLevel( cmdObj.firstElement().numberInt() );
            _diaglog.flush();
            if ( !cmdLine.quiet ) {
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
        virtual bool logTheOp() {
            return true;
        }
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
            out->push_back(Privilege(dbname, actions));
        }
        virtual void help( stringstream& help ) const { help << "drop a collection\n{drop : <collectionName>}"; }
        virtual LockType locktype() const { return WRITE; }

        virtual std::vector<BSONObj> stopIndexBuilds(const std::string& dbname, 
                                                     const BSONObj& cmdObj) {
            std::string nsToDrop = dbname + '.' + cmdObj.firstElement().valuestr();
            std::string systemIndexes = dbname+".system.indexes";
            BSONObj criteria = BSON("ns" << systemIndexes << "op" << "insert" <<
                                    "insert.ns" << nsToDrop);

            return IndexBuilder::killMatchingIndexBuilds(criteria);
        }

        virtual bool run(const string& dbname , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
            string nsToDrop = dbname + '.' + cmdObj.firstElement().valuestr();
            NamespaceDetails *d = nsdetails(nsToDrop);
            if ( !cmdLine.quiet ) {
                MONGO_TLOG(0) << "CMD: drop " << nsToDrop << endl;
            }
            if ( d == 0 ) {
                errmsg = "ns not found";
                return false;
            }

            uassert(10039, "can't drop collection with reserved $ character in name",
                    strchr(nsToDrop.c_str(), '$') == 0);

            stopIndexBuilds(dbname, cmdObj);

            dropCollection( nsToDrop, errmsg, result );
            return true;
        }
    } cmdDrop;

    /* select count(*) */
    class CmdCount : public Command {
    public:
        virtual LockType locktype() const { return READ; }
        CmdCount() : Command("count") { }
        virtual bool logTheOp() { return false; }
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
            out->push_back(Privilege(parseNs(dbname, cmdObj), actions));
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

            string ns = parseNs(dbname, cmdObj);
            string err;
            int errCode;
            long long n = runCount(ns.c_str(), cmdObj, err, errCode);
            long long nn = n;
            bool ok = true;
            if ( n == -1 ) {
                nn = 0;
                result.appendBool( "missing" , true );
            }
            else if ( n < 0 ) {
                nn = 0;
                ok = false;
                if ( !err.empty() ) {
                    errmsg = err;
                    return false;
                }
            }
            result.append("n", (double) nn);
            return ok;
        }
    } cmdCount;

    /* create collection */
    class CmdCreate : public Command {
    public:
        CmdCreate() : Command("create") { }
        virtual bool logTheOp() {
            return false;
        }
        virtual bool slaveOk() const {
            return false;
        }
        virtual bool adminOnly() const {
            return false;
        }
        virtual LockType locktype() const { return WRITE; }
        virtual void help( stringstream& help ) const {
            help << "create a collection explicitly\n"
                "{ create: <ns>[, capped: <bool>, size: <collSizeInBytes>, max: <nDocs>] }";
        }
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            ActionSet actions;
            actions.addAction(ActionType::createCollection);
            out->push_back(Privilege(dbname, actions));
        }
        virtual bool run(const string& dbname , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl ) {
            uassert(15888, "must pass name of collection to create", cmdObj.firstElement().valuestrsafe()[0] != '\0');
            string ns = dbname + '.' + cmdObj.firstElement().valuestr();
            string err;
            uassert(14832, "specify size:<n> when capped is true", !cmdObj["capped"].trueValue() || cmdObj["size"].isNumber() || cmdObj.hasField("$nExtents"));
            bool ok = userCreateNS(ns.c_str(), cmdObj, err, ! fromRepl );
            if ( !ok && !err.empty() )
                errmsg = err;
            return ok;
        }
    } cmdCreate;

    /* "dropIndexes" is now the preferred form - "deleteIndexes" deprecated */
    class CmdDropIndexes : public Command {
    public:
        virtual bool logTheOp() {
            return true;
        }
        virtual bool slaveOk() const {
            return false;
        }
        virtual LockType locktype() const { return WRITE; }
        virtual void help( stringstream& help ) const {
            help << "drop indexes for a collection";
        }
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            ActionSet actions;
            actions.addAction(ActionType::dropIndexes);
            out->push_back(Privilege(parseNs(dbname, cmdObj), actions));
        }

        virtual std::vector<BSONObj> stopIndexBuilds(const std::string& dbname, 
                                                     const BSONObj& cmdObj) {
            std::string systemIndexes = dbname+".system.indexes";
            std::string toDeleteNs = dbname+"."+cmdObj.firstElement().valuestr();
            BSONObjBuilder builder;
            builder.append("ns", systemIndexes);
            builder.append("op", "insert");
            builder.append("insert.ns", toDeleteNs);

            // Get index name to drop
            BSONElement toDrop = cmdObj.getField("index");

            if (toDrop.type() == String) {
                // Kill all in-progress indexes
                if (strcmp("*", toDrop.valuestr()) == 0) {
                    BSONObj criteria = builder.done();
                    return IndexBuilder::killMatchingIndexBuilds(criteria);
                }
                // Kill an in-progress index by name
                else {
                    builder.append("insert.name", toDrop.valuestr());
                    BSONObj criteria = builder.done();
                    return IndexBuilder::killMatchingIndexBuilds(criteria);
                }
            }
            // Kill an in-progress index build by index key
            else if (toDrop.type() == Object) {
                builder.append("insert.key", toDrop.Obj());
                BSONObj criteria = builder.done();
                return IndexBuilder::killMatchingIndexBuilds(criteria);
            }

            return std::vector<BSONObj>();
        }

        CmdDropIndexes() : Command("dropIndexes", false, "deleteIndexes") { }
        bool run(const string& dbname, BSONObj& jsobj, int, string& errmsg, BSONObjBuilder& anObjBuilder, bool /*fromRepl*/) {
            BSONElement e = jsobj.firstElement();
            string toDeleteNs = dbname + '.' + e.valuestr();
            NamespaceDetails *d = nsdetails(toDeleteNs);
            if ( !cmdLine.quiet ) {
                MONGO_TLOG(0) << "CMD: dropIndexes " << toDeleteNs << endl;
            }
            if ( d ) {
                stopIndexBuilds(dbname, jsobj);

                BSONElement f = jsobj.getField("index");
                if ( f.type() == String ) {
                    return dropIndexes( d, toDeleteNs.c_str(), f.valuestr(), errmsg, anObjBuilder, false );
                }
                else if ( f.type() == Object ) {
                    int idxId = d->findIndexByKeyPattern( f.embeddedObject() );
                    if ( idxId < 0 ) {
                        errmsg = "can't find index with key:";
                        errmsg += f.embeddedObject().toString();
                        return false;
                    }
                    else {
                        IndexDetails& ii = d->idx( idxId );
                        string iName = ii.indexName();
                        return dropIndexes( d, toDeleteNs.c_str(), iName.c_str() , errmsg, anObjBuilder, false );
                    }
                }
                else {
                    errmsg = "invalid index name spec";
                    return false;
                }
            }
            else {
                errmsg = "ns not found";
                return false;
            }
        }
    } cmdDropIndexes;

    class CmdReIndex : public Command {
    public:
        virtual bool logTheOp() { return false; } // only reindexes on the one node
        virtual bool slaveOk() const { return true; }    // can reindex on a secondary
        virtual LockType locktype() const { return WRITE; }
        virtual void help( stringstream& help ) const {
            help << "re-index a collection";
        }
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            ActionSet actions;
            actions.addAction(ActionType::reIndex);
            out->push_back(Privilege(parseNs(dbname, cmdObj), actions));
        }
        CmdReIndex() : Command("reIndex") { }

        virtual std::vector<BSONObj> stopIndexBuilds(const std::string& dbname, 
                                                     const BSONObj& cmdObj) {
            std::string systemIndexes = dbname + ".system.indexes";
            std::string ns = dbname + '.' + cmdObj["reIndex"].valuestrsafe();
            BSONObj criteria = BSON("ns" << systemIndexes << "op" << "insert" << "insert.ns" << ns);

            return IndexBuilder::killMatchingIndexBuilds(criteria);
        }

        bool run(const string& dbname , BSONObj& jsobj, int, string& errmsg, BSONObjBuilder& result, bool /*fromRepl*/) {
            static DBDirectClient db;

            BSONElement e = jsobj.firstElement();
            string toDeleteNs = dbname + '.' + e.valuestr();
            NamespaceDetails *d = nsdetails(toDeleteNs);
            MONGO_TLOG(0) << "CMD: reIndex " << toDeleteNs << endl;
            BackgroundOperation::assertNoBgOpInProgForNs(toDeleteNs.c_str());

            if ( ! d ) {
                errmsg = "ns not found";
                return false;
            }

            std::vector<BSONObj> indexesInProg = stopIndexBuilds(dbname, jsobj);

            list<BSONObj> all;
            auto_ptr<DBClientCursor> i = db.query( dbname + ".system.indexes" , BSON( "ns" << toDeleteNs ) , 0 , 0 , 0 , QueryOption_SlaveOk );
            BSONObjBuilder b;
            while ( i->more() ) {
                BSONObj o = i->next().removeField("v").getOwned();
                b.append( BSONObjBuilder::numStr( all.size() ) , o );
                all.push_back( o );
            }


            bool ok = dropIndexes( d, toDeleteNs.c_str(), "*" , errmsg, result, true );
            if ( ! ok ) {
                errmsg = "dropIndexes failed";
                return false;
            }

            for ( list<BSONObj>::iterator i=all.begin(); i!=all.end(); i++ ) {
                BSONObj o = *i;
                LOG(1) << "reIndex ns: " << toDeleteNs << " index: " << o << endl;
                string systemIndexesNs =
                    NamespaceString( toDeleteNs ).getSystemIndexesCollection();
                theDataFileMgr.insertWithObjMod( systemIndexesNs.c_str(), o, false, true );
            }

            result.append( "nIndexes" , (int)all.size() );
            result.appendArray( "indexes" , b.obj() );

            IndexBuilder::restoreIndexes(dbname + ".system.indexes", indexesInProg);
            return true;
        }
    } cmdReIndex;

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
        virtual LockType locktype() const { return NONE; }
        virtual void help( stringstream& help ) const { help << "list databases on this server"; }
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            ActionSet actions;
            actions.addAction(ActionType::listDatabases);
            out->push_back(Privilege(AuthorizationManager::SERVER_RESOURCE_NAME, actions));
        }
        CmdListDatabases() : Command("listDatabases" , true ) {}
        bool run(const string& dbname , BSONObj& jsobj, int, string& errmsg, BSONObjBuilder& result, bool /*fromRepl*/) {
            vector< string > dbNames;
            getDatabaseNames( dbNames );
            vector< BSONObj > dbInfos;

            set<string> seen;
            boost::intmax_t totalSize = 0;
            for ( vector< string >::iterator i = dbNames.begin(); i != dbNames.end(); ++i ) {
                BSONObjBuilder b;
                b.append( "name", *i );

                boost::intmax_t size = dbSize( i->c_str() );
                b.append( "sizeOnDisk", (double) size );
                totalSize += size;
                
                {
                    Client::ReadContext rc( *i + ".system.namespaces" );
                    b.appendBool( "empty", rc.ctx().db()->isEmpty() );
                }
                
                dbInfos.push_back( b.obj() );

                seen.insert( i->c_str() );
            }

            // TODO: erh 1/1/2010 I think this is broken where path != dbpath ??
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
        virtual void help( stringstream& help ) const { help << "Close all database files.\nA new request will cause an immediate reopening; thus, this is mostly for testing purposes."; }
        virtual bool adminOnly() const { return true; }
        virtual bool slaveOk() const { return false; }
        virtual LockType locktype() const { return WRITE; }
        virtual bool lockGlobally() const { return true; }
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            ActionSet actions;
            actions.addAction(ActionType::closeAllDatabases);
            out->push_back(Privilege(AuthorizationManager::SERVER_RESOURCE_NAME, actions));
        }
        CmdCloseAllDatabases() : Command( "closeAllDatabases" ) {}
        bool run(const string& dbname , BSONObj& jsobj, int, string& errmsg, BSONObjBuilder& result, bool /*fromRepl*/) {
            bool ok;
            try {
                ok = dbHolderW().closeAll( dbpath , result, false );
            }
            catch(DBException&) { 
                throw;
            }
            catch(...) { 
                log() << "ERROR uncaught exception in command closeAllDatabases" << endl;
                errmsg = "unexpected uncaught exception";
                return false;
            }
            return ok;
        }
    } cmdCloseAllDatabases;

    class CmdFileMD5 : public Command {
    public:
        CmdFileMD5() : Command( "filemd5" ) {}
        virtual bool slaveOk() const {
            return true;
        }
        virtual void help( stringstream& help ) const {
            help << " example: { filemd5 : ObjectId(aaaaaaa) , root : \"fs\" }";
        }
        virtual LockType locktype() const { return READ; }
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            ActionSet actions;
            actions.addAction(ActionType::find);
            out->push_back(Privilege(parseNs(dbname, cmdObj), actions));
        }
        bool run(const string& dbname, BSONObj& jsobj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl ) {
            string ns = dbname;
            ns += ".";
            {
                string root = jsobj.getStringField( "root" );
                if ( root.size() == 0 )
                    root = "fs";
                ns += root;
            }
            ns += ".chunks"; // make this an option in jsobj

            // Check shard version at startup.
            // This will throw before we've done any work if shard version is outdated
            Client::Context ctx (ns);

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

            shared_ptr<Cursor> cursor = getBestGuessCursor(ns.c_str(), query, sort);
            if ( ! cursor ) {
                errmsg = "need an index on { files_id : 1 , n : 1 }";
                return false;
            }
            auto_ptr<ClientCursor> cc (new ClientCursor(QueryOption_NoCursorTimeout, cursor, ns.c_str()));

            while ( cursor->ok() ) {
                if ( ! cursor->matcher()->matchesCurrent( cursor.get() ) ) {
                    log() << "**** NOT MATCHING ****" << endl;
                    PRINT(cursor->current());
                    cursor->advance();
                    continue;
                }

                BSONObj obj = cursor->current();
                cursor->advance();

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

                ClientCursorYieldLock yield (cc.get());
                try {
                    md5_append( &st , (const md5_byte_t*)(data) , len );
                    n++;
                }
                catch (...) {
                    if ( ! yield.stillOk() ) // relocks
                        cc.release();
                    throw;
                }

                try { // SERVER-5752 may make this try unnecessary
                    if ( ! yield.stillOk() ) { // relocks and checks shard version
                        cc.release();
                        if (!partialOk)
                            uasserted(13281, "File deleted during filemd5 command");
                    }
                }
                catch(SendStaleConfigException& e){
                    // return partial results.
                    // Mongos will get the error at the start of the next call if it doesn't update first.
                    log() << "Config changed during filemd5 - command will resume " << endl;

                    // useful for debugging but off by default to avoid looking like a scary error.
                    LOG(1) << "filemd5 stale config exception: " << e.what() << endl;
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
        CmdDatasize() : Command( "dataSize", false, "datasize" ) {}
        virtual bool slaveOk() const { return true; }
        virtual LockType locktype() const { return READ; }
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
            out->push_back(Privilege(parseNs(dbname, cmdObj), actions));
        }
        bool run(const string& dbname, BSONObj& jsobj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl ) {
            Timer timer;

            string ns = jsobj.firstElement().String();
            BSONObj min = jsobj.getObjectField( "min" );
            BSONObj max = jsobj.getObjectField( "max" );
            BSONObj keyPattern = jsobj.getObjectField( "keyPattern" );
            bool estimate = jsobj["estimate"].trueValue();

            Client::Context ctx( ns );
            NamespaceDetails *d = nsdetails(ns);

            if ( ! d || d->numRecords() == 0 ) {
                result.appendNumber( "size" , 0 );
                result.appendNumber( "numObjects" , 0 );
                result.append( "millis" , timer.millis() );
                return true;
            }

            result.appendBool( "estimate" , estimate );

            auto_ptr<Runner> runner;
            if ( min.isEmpty() && max.isEmpty() ) {
                if ( estimate ) {
                    result.appendNumber( "size" , d->dataSize() );
                    result.appendNumber( "numObjects" , d->numRecords() );
                    result.append( "millis" , timer.millis() );
                    return 1;
                }
                runner.reset(InternalPlanner::collectionScan(ns));
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

                const IndexDetails *idx = d->findIndexByPrefix( keyPattern ,
                                                                true );  /* require single key */
                if ( idx == NULL ) {
                    errmsg = "couldn't find valid index containing key pattern";
                    return false;
                }
                // If both min and max non-empty, append MinKey's to make them fit chosen index
                KeyPattern kp( idx->keyPattern() );
                min = Helpers::toKeyFormat( kp.extendRangeBound( min, false ) );
                max = Helpers::toKeyFormat( kp.extendRangeBound( max, false ) );

                runner.reset(InternalPlanner::indexScan(ns, d, d->idxNo(*idx), min, max, false));
            }

            long long avgObjSize = d->dataSize() / d->numRecords();

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
                    size += loc.rec()->netLength();

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

            NamespaceDetails * nsd = nsdetails( ns );
            if ( ! nsd )
                return 0;

            long long totalSize = 0;

            NamespaceDetails::IndexIterator ii = nsd->ii();
            while ( ii.more() ) {
                IndexDetails& d = ii.next();
                string collNS = d.indexNamespace();
                NamespaceDetails * mine = nsdetails( collNS );
                if ( ! mine ) {
                    log() << "error: have index ["  << collNS << "] but no NamespaceDetails" << endl;
                    continue;
                }
                totalSize += mine->dataSize();
                if ( details )
                    details->appendNumber( d.indexName() , mine->dataSize() / scale );
            }
            return totalSize;
        }
    }

    class CollectionStats : public Command {
    public:
        CollectionStats() : Command( "collStats", false, "collstats" ) {}
        virtual bool slaveOk() const { return true; }
        virtual LockType locktype() const { return READ; }
        virtual void help( stringstream &help ) const {
            help << "{ collStats:\"blog.posts\" , scale : 1 } scale divides sizes e.g. for KB use 1024\n"
                    "    avgObjSize - in bytes";
        }
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            ActionSet actions;
            actions.addAction(ActionType::collStats);
            out->push_back(Privilege(parseNs(dbname, cmdObj), actions));
        }
        bool run(const string& dbname, BSONObj& jsobj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl ) {
            string ns = dbname + "." + jsobj.firstElement().valuestr();
            Client::Context cx( ns );

            NamespaceDetails * nsd = nsdetails( ns );
            if ( ! nsd ) {
                errmsg = "ns not found";
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

            long long size = nsd->dataSize() / scale;
            result.appendNumber( "count" , nsd->numRecords() );
            result.appendNumber( "size" , size );
            if( nsd->numRecords() )
                result.append      ( "avgObjSize" , double(size) / double(nsd->numRecords()) );

            int numExtents;
            BSONArrayBuilder extents;

            result.appendNumber( "storageSize" , nsd->storageSize( &numExtents , verbose ? &extents : 0  ) / scale );
            result.append( "numExtents" , numExtents );
            result.append( "nindexes" , nsd->getCompletedIndexCount() );
            result.append( "lastExtentSize" , nsd->lastExtentSize() / scale );
            result.append( "paddingFactor" , nsd->paddingFactor() );
            result.append( "systemFlags" , nsd->systemFlags() );
            result.append( "userFlags" , nsd->userFlags() );

            BSONObjBuilder indexSizes;
            result.appendNumber( "totalIndexSize" , getIndexSizeForCollection(dbname, ns, &indexSizes, scale) / scale );
            result.append("indexSizes", indexSizes.obj());

            if ( nsd->isCapped() ) {
                result.append( "capped" , nsd->isCapped() );
                result.appendNumber( "max" , nsd->maxCappedDocs() );
            }

            if ( verbose )
                result.appendArray( "extents" , extents.arr() );

            return true;
        }
    } cmdCollectionStats;

    class CollectionModCommand : public Command {
    public:
        CollectionModCommand() : Command( "collMod" ){}
        virtual bool slaveOk() const { return false; }
        virtual LockType locktype() const { return WRITE; }
        virtual bool logTheOp() { return true; }
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
            out->push_back(Privilege(parseNs(dbname, cmdObj), actions));
        }
        bool run(const string& dbname, BSONObj& jsobj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl ) {
            string ns = dbname + "." + jsobj.firstElement().valuestr();
            Client::Context ctx( ns );
            NamespaceDetails* nsd = nsdetails( ns );
            if ( ! nsd ) {
                errmsg = "ns does not exist";
                return false;
            }

            bool ok = true;

            BSONForEach( e, jsobj ) {
                if ( str::equals( "collMod", e.fieldName() ) ) {
                    // no-op
                }
                else if ( str::equals( "usePowerOf2Sizes", e.fieldName() ) ) {
                    bool oldPowerOf2 = nsd->isUserFlagSet(NamespaceDetails::Flag_UsePowerOf2Sizes);
                    bool newPowerOf2 = e.trueValue();

                    if ( oldPowerOf2 != newPowerOf2 ) {
                        // change userFlags
                        result.appendBool( "usePowerOf2Sizes_old", oldPowerOf2 );

                        newPowerOf2 ? nsd->setUserFlag( NamespaceDetails::Flag_UsePowerOf2Sizes ) :
                                      nsd->clearUserFlag( NamespaceDetails::Flag_UsePowerOf2Sizes );
                        nsd->syncUserFlags( ns ); // must keep system.namespaces up-to-date

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

                    int idxNo = nsd->findIndexByKeyPattern( keyPattern );
                    if( idxNo < 0 ){
                        errmsg = str::stream() << "cannot find index " << keyPattern
                                               << " for ns " << ns;
                        ok = false;
                        continue;
                    }

                    IndexDetails idx = nsd->idx( idxNo );
                    BSONElement oldExpireSecs = idx.info.obj().getField("expireAfterSeconds");
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
                        nsd->updateTTLIndex( idxNo , newExpireSecs );
                        result.appendAs( newExpireSecs , "expireAfterSeconds_new" );
                    }
                }
                else {
                    errmsg = str::stream() << "unknown option to collMod: " << e.fieldName();
                    ok = false;
                }
            }
            
            return ok;
        }
    } collectionModCommand;

    class DBStats : public Command {
    public:
        DBStats() : Command( "dbStats", false, "dbstats" ) {}
        virtual bool slaveOk() const { return true; }
        virtual LockType locktype() const { return READ; }
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
            out->push_back(Privilege(dbname, actions));
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

            list<string> collections;
            Database* d = cc().database();
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

                NamespaceDetails * nsd = nsdetails( ns );
                if ( ! nsd ) {
                    errmsg = "missing ns: ";
                    errmsg += ns;
                    return false;
                }

                ncollections += 1;
                objects += nsd->numRecords();
                size += nsd->dataSize();

                int temp;
                storageSize += nsd->storageSize( &temp );
                numExtents += temp;

                indexes += nsd->getCompletedIndexCount();
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
            result.appendNumber( "fileSize" , d->fileSize() / scale );
            if( d )
                result.appendNumber( "nsSizeMB", (int) d->namespaceIndex().fileLength() / 1024 / 1024 );

            BSONObjBuilder dataFileVersion( result.subobjStart( "dataFileVersion" ) );
            if ( d && !d->isEmpty() ) {
                DataFileHeader* header = d->getFile( 0 )->getHeader();
                dataFileVersion.append( "major", header->version );
                dataFileVersion.append( "minor", header->versionMinor );
            }
            dataFileVersion.done();

            return true;
        }
    } cmdDBStats;

    /* convertToCapped seems to use this */
    class CmdCloneCollectionAsCapped : public Command {
    public:
        CmdCloneCollectionAsCapped() : Command( "cloneCollectionAsCapped" ) {}
        virtual bool slaveOk() const { return false; }
        virtual LockType locktype() const { return WRITE; }
        virtual void help( stringstream &help ) const {
            help << "{ cloneCollectionAsCapped:<fromName>, toCollection:<toName>, size:<sizeInBytes> }";
        }
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            ActionSet sourceActions;
            sourceActions.addAction(ActionType::find);
            out->push_back(Privilege(parseNs(dbname, cmdObj), sourceActions));

            ActionSet targetActions;
            targetActions.addAction(ActionType::insert);
            targetActions.addAction(ActionType::ensureIndex);
            std::string collection = cmdObj.getStringField("toCollection");
            uassert(16708, "bad 'toCollection' value", !collection.empty());

            std::string targetNs = dbname + "." + collection;

            out->push_back(Privilege(targetNs, targetActions));
        }
        bool run(const string& dbname, BSONObj& jsobj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl ) {
            string from = jsobj.getStringField( "cloneCollectionAsCapped" );
            string to = jsobj.getStringField( "toCollection" );
            long long size = (long long)jsobj.getField( "size" ).number();

            if ( from.empty() || to.empty() || size == 0 ) {
                errmsg = "invalid command spec";
                return false;
            }

            string fromNs = dbname + "." + from;
            string toNs = dbname + "." + to;
            NamespaceDetails *nsd = nsdetails( fromNs );
            massert( 10301 ,  "source collection " + fromNs + " does not exist", nsd );
            long long excessSize = nsd->dataSize() - size * 2; // datasize and extentSize can't be compared exactly, so add some padding to 'size'
            DiskLoc extent = nsd->firstExtent();
            for( ; excessSize > extent.ext()->length && extent != nsd->lastExtent(); extent = extent.ext()->xnext ) {
                excessSize -= extent.ext()->length;
                LOG( 2 ) << "cloneCollectionAsCapped skipping extent of size " << extent.ext()->length << endl;
                LOG( 6 ) << "excessSize: " << excessSize << endl;
            }
            DiskLoc startLoc = extent.ext()->firstRecord;

            CursorId id;
            {
                Runner* runner = InternalPlanner::collectionScan(fromNs, InternalPlanner::FORWARD, startLoc);
                // Takes ownership of runner.  The cc will timeout, eventually...
                ClientCursor *cc = new ClientCursor(runner);
                id = cc->cursorid();
            }

            DBDirectClient client;
            Client::Context ctx( toNs );
            BSONObjBuilder spec;
            spec.appendBool( "capped", true );
            spec.append( "size", double( size ) );
            if (jsobj.hasField("temp"))
                spec.append(jsobj["temp"]);
            if ( !userCreateNS( toNs.c_str(), spec.done(), errmsg, true ) )
                return false;

            auto_ptr< DBClientCursor > c = client.getMore( fromNs, id );
            while( c->more() ) {
                BSONObj obj = c->next();
                theDataFileMgr.insertAndLog( toNs.c_str(), obj, true );
                getDur().commitIfNeeded();
            }

            return true;
        }
    } cmdCloneCollectionAsCapped;

    /* jan2010:
       Converts the given collection to a capped collection w/ the specified size.
       This command is not highly used, and is not currently supported with sharded
       environments.
       */
    class CmdConvertToCapped : public Command {
    public:
        CmdConvertToCapped() : Command( "convertToCapped" ) {}
        virtual bool slaveOk() const { return false; }
        virtual LockType locktype() const { return WRITE; }
        // calls renamecollection which does a global lock, so we must too:
        virtual bool lockGlobally() const { return true; }
        virtual void help( stringstream &help ) const {
            help << "{ convertToCapped:<fromCollectionName>, size:<sizeInBytes> }";
        }
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            ActionSet actions;
            actions.addAction(ActionType::convertToCapped);
            out->push_back(Privilege(parseNs(dbname, cmdObj), actions));
        }
        bool run(const string& dbname, BSONObj& jsobj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl ) {
            BackgroundOperation::assertNoBgOpInProgForDb(dbname.c_str());

            string from = jsobj.getStringField( "convertToCapped" );
            long long size = (long long)jsobj.getField( "size" ).number();

            if ( from.empty() || size == 0 ) {
                errmsg = "invalid command spec";
                return false;
            }

            string shortTmpName = str::stream() << "tmp.convertToCapped." << from;
            string longTmpName = str::stream() << dbname << "." << shortTmpName;

            DBDirectClient client;
            client.dropCollection( longTmpName );

            BSONObj info;
            if ( !client.runCommand( dbname ,
                                     BSON( "cloneCollectionAsCapped" << from << "toCollection" << shortTmpName << "size" << double( size ) << "temp" << true ),
                                     info ) ) {
                errmsg = "cloneCollectionAsCapped failed: " + info.toString();
                return false;
            }

            if ( !client.dropCollection( dbname + "." + from ) ) {
                errmsg = "failed to drop original collection";
                return false;
            }

            if ( !client.runCommand( "admin",
                                     BSON( "renameCollection" << longTmpName <<
                                           "to" << ( dbname + "." + from ) <<
                                           "stayTemp" << false // explicit
                                           ),
                                     info ) ) {
                errmsg = "renameCollection failed: " + info.toString();
                return false;
            }

            return true;
        }
    } cmdConvertToCapped;

    /* Returns client's uri */
    class CmdWhatsMyUri : public Command {
    public:
        CmdWhatsMyUri() : Command("whatsmyuri") { }
        virtual bool slaveOk() const {
            return true;
        }
        virtual LockType locktype() const { return NONE; }
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

    /* For testing only, not for general use. Enabled via command-line */
    class GodInsert : public Command {
    public:
        GodInsert() : Command( "godinsert" ) { }
        virtual bool adminOnly() const { return false; }
        virtual bool logTheOp() { return false; }
        virtual bool slaveOk() const { return true; }
        virtual LockType locktype() const { return NONE; }
        // No auth needed because it only works when enabled via command line.
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {}
        virtual void help( stringstream &help ) const {
            help << "internal. for testing only.";
        }
        virtual bool run(const string& dbname, BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
            string coll = cmdObj[ "godinsert" ].valuestrsafe();
            log() << "test only command godinsert invoked coll:" << coll << endl;
            uassert( 13049, "godinsert must specify a collection", !coll.empty() );
            string ns = dbname + "." + coll;
            BSONObj obj = cmdObj[ "obj" ].embeddedObjectUserCheck();
            {
                Lock::DBWrite lk(ns);
                Client::Context ctx( ns );
                theDataFileMgr.insertWithObjMod( ns.c_str(), obj, false, true );
            }
            return true;
        }
    };

    MONGO_INITIALIZER(RegisterGodInsertCmd)(InitializerContext* context) {
        if (Command::testCommandsEnabled) {
            // Leaked intentionally: a Command registers itself when constructed.
            new GodInsert();
        }
        return Status::OK();
    }
    
    class DBHashCmd : public Command {
    public:
        DBHashCmd() : Command( "dbHash", false, "dbhash" ) {}
        virtual bool slaveOk() const { return true; }
        virtual LockType locktype() const { return READ; }
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            ActionSet actions;
            actions.addAction(ActionType::dbHash);
            out->push_back(Privilege(dbname, actions));
        }
        virtual bool run(const string& dbname , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
            Timer timer;

            set<string> desiredCollections;
            if ( cmdObj["collections"].type() == Array ) {
                BSONObjIterator i( cmdObj["collections"].Obj() );
                while ( i.more() ) {
                    BSONElement e = i.next();
                    if ( e.type() != String ) {
                        errmsg = "collections entries have to be strings";
                        return false;
                    }
                    desiredCollections.insert( e.String() );
                }
            }

            list<string> colls;
            Database* db = cc().database();
            if ( db )
                db->namespaceIndex().getNamespaces( colls );
            colls.sort();

            result.appendNumber( "numCollections" , (long long)colls.size() );
            result.append( "host" , prettyHostName() );

            md5_state_t globalState;
            md5_init(&globalState);

            BSONObjBuilder bb( result.subobjStart( "collections" ) );
            for ( list<string>::iterator i=colls.begin(); i != colls.end(); i++ ) {
                string fullCollectionName = *i;
                string shortCollectionName = fullCollectionName.substr( dbname.size() + 1 );

                if ( shortCollectionName.find( "system." ) == 0 )
                    continue;

                if ( desiredCollections.size() > 0 &&
                     desiredCollections.count( shortCollectionName ) == 0 )
                    continue;

                NamespaceDetails * nsd = nsdetails( fullCollectionName );

                // debug SERVER-761
                NamespaceDetails::IndexIterator ii = nsd->ii();
                while( ii.more() ) {
                    const IndexDetails &idx = ii.next();
                    if ( !idx.head.isValid() || !idx.info.isValid() ) {
                        log() << "invalid index for ns: " << fullCollectionName << " " << idx.head << " " << idx.info;
                        if ( idx.info.isValid() )
                            log() << " " << idx.info.obj();
                        log() << endl;
                    }
                }

                auto_ptr<Runner> runner;
                int idNum = nsd->findIdIndex();
                if ( idNum >= 0 ) {
                    runner.reset(InternalPlanner::indexScan(fullCollectionName,
                                                            nsd,
                                                            idNum,
                                                            BSONObj(),
                                                            BSONObj(),
                                                            false,
                                                            InternalPlanner::FORWARD,
                                                            InternalPlanner::IXSCAN_FETCH));
                }
                else if ( nsd->isCapped() ) {
                    runner.reset(InternalPlanner::collectionScan(fullCollectionName));
                }
                else {
                    log() << "can't find _id index for: " << fullCollectionName << endl;
                    continue;
                }

                md5_state_t st;
                md5_init(&st);

                long long n = 0;
                Runner::RunnerState state;
                BSONObj c;
                verify(NULL != runner.get());
                while (Runner::RUNNER_ADVANCED == (state = runner->getNext(&c, NULL))) {
                    md5_append( &st , (const md5_byte_t*)c.objdata() , c.objsize() );
                    n++;
                }
                if (Runner::RUNNER_EOF != state) {
                    warning() << "error while hashing, db dropped? ns=" << fullCollectionName << endl;
                }
                md5digest d;
                md5_finish(&st, d);
                string hash = digestToString( d );

                bb.append( shortCollectionName, hash );

                md5_append( &globalState , (const md5_byte_t*)hash.c_str() , hash.size() );
            }
            bb.done();

            md5digest d;
            md5_finish(&globalState, d);
            string hash = digestToString( d );

            result.append( "md5" , hash );
            result.appendNumber( "timeMillis", timer.millis() );
            return 1;
        }

    } dbhashCmd;

    /* for diagnostic / testing purposes. Enabled via command line. */
    class CmdSleep : public Command {
    public:
        virtual LockType locktype() const { return NONE; }
        virtual bool adminOnly() const { return true; }
        virtual bool logTheOp() { return false; }
        virtual bool slaveOk() const { return true; }
        virtual void help( stringstream& help ) const {
            help << "internal testing command.  Makes db block (in a read lock) for 100 seconds\n";
            help << "w:true write lock. secs:<seconds>";
        }
        // No auth needed because it only works when enabled via command line.
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {}
        CmdSleep() : Command("sleep") { }
        bool run(const string& ns, BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            log() << "test only command sleep invoked" << endl;
            long long millis = 10 * 1000;

            if (cmdObj["secs"].isNumber() && cmdObj["millis"].isNumber()) {
                millis = cmdObj["secs"].numberLong() * 1000 + cmdObj["millis"].numberLong();
            }
            else if (cmdObj["secs"].isNumber()) {
                millis = cmdObj["secs"].numberLong() * 1000;
            }
            else if (cmdObj["millis"].isNumber()) {
                millis = cmdObj["millis"].numberLong();
            }

            if(cmdObj.getBoolField("w")) {
                Lock::GlobalWrite lk;
                sleepmillis(millis);
            }
            else {
                Lock::GlobalRead lk;
                sleepmillis(millis);
            }

            // Interrupt point for testing (e.g. maxTimeMS).
            killCurrentOp.checkForInterrupt();

            return true;
        }
    };
    MONGO_INITIALIZER(RegisterSleepCmd)(InitializerContext* context) {
        if (Command::testCommandsEnabled) {
            // Leaked intentionally: a Command registers itself when constructed.
            new CmdSleep();
        }
        return Status::OK();
    }

    // Testing only, enabled via command-line.
    class CapTrunc : public Command {
    public:
        CapTrunc() : Command( "captrunc" ) {}
        virtual bool slaveOk() const { return false; }
        virtual LockType locktype() const { return WRITE; }
        // No auth needed because it only works when enabled via command line.
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {}
        virtual bool run(const string& dbname , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
            string coll = cmdObj[ "captrunc" ].valuestrsafe();
            uassert( 13416, "captrunc must specify a collection", !coll.empty() );
            string ns = dbname + "." + coll;
            int n = cmdObj.getIntField( "n" );

            // inclusive range?
            bool inc = cmdObj.getBoolField( "inc" );
            NamespaceDetails *nsd = nsdetails( ns );
            ReverseCappedCursor c( nsd );
            massert( 13417, "captrunc collection not found or empty", c.ok() );
            for( int i = 0; i < n; ++i ) {
                massert( 13418, "captrunc invalid n", c.advance() );
            }
            DiskLoc end = c.currLoc();
            nsd->cappedTruncateAfter( ns.c_str(), end, inc );
            return true;
        }
    };
    MONGO_INITIALIZER(RegisterCapTruncCmd)(InitializerContext* context) {
        if (Command::testCommandsEnabled) {
            // Leaked intentionally: a Command registers itself when constructed.
            new CapTrunc();
        }
        return Status::OK();
    }

    // Testing-only, enabled via command line.
    class EmptyCapped : public Command {
    public:
        EmptyCapped() : Command( "emptycapped" ) {}
        virtual bool slaveOk() const { return false; }
        virtual LockType locktype() const { return WRITE; }
        virtual bool logTheOp() { return true; }
        // No auth needed because it only works when enabled via command line.
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {}

        virtual std::vector<BSONObj> stopIndexBuilds(const std::string& dbname, 
                                                     const BSONObj& cmdObj) {
            std::string systemIndexes = dbname + ".system.indexes";
            std::string coll = cmdObj[ "emptycapped" ].valuestrsafe();
            std::string ns = dbname + '.' + coll;
            BSONObj criteria = BSON("ns" << systemIndexes << "op" << "insert" << "insert.ns" << ns);

            return IndexBuilder::killMatchingIndexBuilds(criteria);
        }

        virtual bool run(const string& dbname , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
            string coll = cmdObj[ "emptycapped" ].valuestrsafe();
            uassert( 13428, "emptycapped must specify a collection", !coll.empty() );
            string ns = dbname + "." + coll;
            NamespaceDetails *nsd = nsdetails( ns );
            massert( 13429, "emptycapped no such collection", nsd );

            std::vector<BSONObj> indexes = stopIndexBuilds(dbname, cmdObj);

            nsd->emptyCappedCollection( ns.c_str() );

            IndexBuilder::restoreIndexes(dbname + ".system.indexes", indexes);

            return true;
        }
    };
    MONGO_INITIALIZER(RegisterEmptyCappedCmd)(InitializerContext* context) {
        if (Command::testCommandsEnabled) {
            // Leaked intentionally: a Command registers itself when constructed.
            new EmptyCapped();
        }
        return Status::OK();
    }

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

        Status status = _checkAuthorization(c, &client, dbname, cmdObj, fromRepl);
        if (!status.isOK()) {
            appendCommandStatus(result, status);
            return;
        }

        if ( cmdObj["help"].trueValue() ) {
            client.curop()->ensureStarted();
            stringstream ss;
            ss << "help for: " << c->name << " ";
            c->help( ss );
            result.append( "help" , ss.str() );
            result.append( "lockType" , c->locktype() );
            appendCommandStatus(result, true, "");
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
        StatusWith<int> maxTimeMS = LiteParsedQuery::parseMaxTimeMS(cmdObj["maxTimeMS"]);
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
        if (MONGO_FAIL_POINT(maxTimeAlwaysTimeOut) && maxTimeMS.getValue() != 0) {
            appendCommandStatus(result, Status(ErrorCodes::ExceededTimeLimit,
                                               "exception: operation exceeded time limit "
                                                   "[maxTimeAlwaysTimeOut]",
                                               0));
            return;
        }

        std::string errmsg;
        bool retval = false;
        if ( c->locktype() == Command::NONE ) {
            verify( !c->lockGlobally() );

            // we also trust that this won't crash
            retval = true;

            if (retval) {
                client.curop()->ensureStarted();
                retval = _execCommand(c, dbname, cmdObj, queryOptions, errmsg, result, fromRepl);
            }
        }
        else if( c->locktype() != Command::WRITE ) { 
            // read lock
            verify( ! c->logTheOp() );
            string ns = c->parseNs(dbname, cmdObj);
            scoped_ptr<Lock::GlobalRead> lk;
            if( c->lockGlobally() )
                lk.reset( new Lock::GlobalRead() );
            Client::ReadContext ctx(ns , dbpath); // read locks
            client.curop()->ensureStarted();
            retval = _execCommand(c, dbname, cmdObj, queryOptions, errmsg, result, fromRepl);
        }
        else {
            dassert( c->locktype() == Command::WRITE );
            bool global = c->lockGlobally();
            DEV {
                if( !global && Lock::isW() ) { 
                    log() << "\ndebug have W lock but w would suffice for command " << c->name << endl;
                }
                if( global && Lock::isLocked() == 'w' ) { 
                    // can't go w->W
                    log() << "need glboal W lock but already have w on command : " << cmdObj.toString() << endl;
                }
            }
            scoped_ptr<Lock::ScopedLock> lk( global ? 
                                             static_cast<Lock::ScopedLock*>( new Lock::GlobalWrite() ) :
                                             static_cast<Lock::ScopedLock*>( new Lock::DBWrite( dbname ) ) );
            client.curop()->ensureStarted();
            Client::Context ctx(dbname, dbpath);
            retval = _execCommand(c, dbname, cmdObj, queryOptions, errmsg, result, fromRepl);
            if ( retval && c->logTheOp() && ! fromRepl ) {
                logOp("c", cmdns, cmdObj);
            }
        }

        appendCommandStatus(result, retval, errmsg);
        return;
    }


    /* TODO make these all command objects -- legacy stuff here

       usage:
         abc.$cmd.findOne( { ismaster:1 } );

       returns true if ran a cmd
    */
    bool _runCommands(const char *ns, BSONObj& _cmdobj, BufBuilder &b, BSONObjBuilder& anObjBuilder, bool fromRepl, int queryOptions) {
        string dbname = nsToDatabase( ns );

        LOG(1) << "run command " << ns << ' ' << _cmdobj << endl;

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
            anObjBuilder.append("bad cmd" , _cmdobj );
        }

        BSONObj x = anObjBuilder.done();
        b.appendBuf(x.objdata(), x.objsize());

        return true;
    }

} // namespace mongo
