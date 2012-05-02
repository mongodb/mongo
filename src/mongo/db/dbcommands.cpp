// dbcommands.cpp

/**
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
*/

/* SHARDING: 
   I believe this file is for mongod only.
   See s/commnands_public.cpp for mongos.
*/

#include "pch.h"
#include "ops/count.h"
#include "pdfile.h"
#include "jsobj.h"
#include "../bson/util/builder.h"
#include <time.h>
#include "introspect.h"
#include "btree.h"
#include "../util/lruishmap.h"
#include "../util/md5.hpp"
#include "../util/processinfo.h"
#include "../util/ramlog.h"
#include "json.h"
#include "repl.h"
#include "repl_block.h"
#include "replutil.h"
#include "commands.h"
#include "db.h"
#include "instance.h"
#include "lasterror.h"
#include "security.h"
#include "queryoptimizer.h"
#include "../scripting/engine.h"
#include "stats/counters.h"
#include "background.h"
#include "../util/version.h"
#include "../s/d_writeback.h"
#include "dur_stats.h"
#include "../server.h"

namespace mongo {

    namespace dur { 
        void setAgeOutJournalFiles(bool rotate);
    }
    /** @return true if fields found */
    bool setParmsMongodSpecific(const string& dbname, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl ) { 
        BSONElement e = cmdObj["ageOutJournalFiles"];
        if( !e.eoo() ) {
            bool r = e.trueValue();
            log() << "ageOutJournalFiles " << r << endl;
            dur::setAgeOutJournalFiles(r);
            return true;
        }
        return false;
    }

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
        virtual void help( stringstream& help ) const {
            help << "return error status of the last operation on this connection\n"
                 << "options:\n"
                 << "  { fsync:true } - fsync before returning, or wait for journal commit if running with --journal\n"
                 << "  { j:true } - wait for journal commit if running with --journal\n"
                 << "  { w:n } - await replication to n servers (including self) before returning\n"
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

            if ( cmdObj["j"].trueValue() ) { 
                if( !getDur().awaitCommit() ) {
                    // --journal is off
                    result.append("jnote", "journaling not enabled on this server");
                }
                if( cmdObj["fsync"].trueValue() ) { 
                    errmsg = "fsync and j options are not used together";
                    return false;
                }
            }
            else if ( cmdObj["fsync"].trueValue() ) {
                Timer t;
                if( !getDur().awaitCommit() ) {
                    // if get here, not running with --journal
                    log() << "fsync from getlasterror" << endl;
                    result.append( "fsyncFiles" , MemoryMappedFile::flushAll( true ) );
                }
                else {
                    // this perhaps is temp.  how long we wait for the group commit to occur.
                    result.append( "waited", t.millis() );
                }
            }

            if ( err ) {
                // doesn't make sense to wait for replication
                // if there was an error
                return true;
            }

            BSONElement e = cmdObj["w"];
            if ( e.ok() ) {
                int timeout = cmdObj["wtimeout"].numberInt();
                Timer t;

                long long passes = 0;
                char buf[32];
                while ( 1 ) {
                    OpTime op(c.getLastOp());
                    
                    if ( op.isNull() ) {
                        if ( anyReplEnabled() ) {
                            result.append( "wnote" , "no write has been done on this connection" );
                        }
                        else if ( e.isNumber() && e.numberInt() <= 1 ) {
                            // don't do anything
                            // w=1 and no repl, so this is fine
                        }
                        else {
                            // w=2 and no repl
                            result.append( "wnote" , "no replication has been enabled, so w=2+ won't work" );
                            result.append( "err", "norepl" );
                            return true; 
                        }
                        break;
                    }

                    // check this first for w=0 or w=1
                    if ( opReplicatedEnough( op, e ) ) {
                        break;
                    }

                    // if replication isn't enabled (e.g., config servers)
                    if ( ! anyReplEnabled() ) {
                        result.append( "err", "norepl" );
                        return true;
                    }


                    if ( timeout > 0 && t.millis() >= timeout ) {
                        result.append( "wtimeout" , true );
                        errmsg = "timed out waiting for slaves";
                        result.append( "waited" , t.millis() );
                        result.append( "err" , "timeout" );
                        return true;
                    }

                    verify( sprintf( buf , "w block pass: %lld" , ++passes ) < 30 );
                    c.curop()->setMessage( buf );
                    sleepmillis(1);
                    killCurrentOp.checkForInterrupt();
                }
                result.appendNumber( "wtime" , t.millis() );
            }

            result.appendNull( "err" );
            return true;
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

        if (!force && theReplSet && theReplSet->isPrimary()) {
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

        // this is suboptimal but syncDataAndTruncateJournal is called from dropDatabase, and that 
        // may need a global lock.
        virtual bool lockGlobally() const { return true; }

        virtual LockType locktype() const { return WRITE; }
        CmdDropDatabase() : Command("dropDatabase") {}
        bool run(const string& dbname, BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            BSONElement e = cmdObj.firstElement();
            log() << "dropDatabase " << dbname << endl;
            int p = (int) e.number();
            if ( p != 1 )
                return false;
            dropDatabase(dbname);
            result.append( "dropped" , dbname );
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
        CmdRepairDatabase() : Command("repairDatabase") {}
        bool run(const string& dbname , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            BSONElement e = cmdObj.firstElement();
            log() << "repairDatabase " << dbname << endl;
            int p = (int) e.number();
            if ( p != 1 ) {
                errmsg = "bad option";
                return false;
            }
            e = cmdObj.getField( "preserveClonedFilesOnFailure" );
            bool preserveClonedFilesOnFailure = e.isBoolean() && e.boolean();
            e = cmdObj.getField( "backupOriginalFiles" );
            bool backupOriginalFiles = e.isBoolean() && e.boolean();
            return repairDatabase( dbname, errmsg, preserveClonedFilesOnFailure, backupOriginalFiles );
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
            help << "http://www.mongodb.org/display/DOCS/Database+Profiler";
        }
        virtual LockType locktype() const { return WRITE; }
        CmdProfile() : Command("profile") {}
        bool run(const string& dbname, BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            BSONElement e = cmdObj.firstElement();
            result.append("was", cc().database()->profile);
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

    void reportLockStats(BSONObjBuilder& result);
    
    class CmdServerStatus : public Command {
        unsigned long long _started;
    public:
        virtual bool slaveOk() const {
            return true;
        }
        CmdServerStatus() : Command("serverStatus", true) {
            _started = curTimeMillis64();
        }

        virtual LockType locktype() const { return NONE; }

        virtual void help( stringstream& help ) const {
            help << "returns lots of administrative server statistics";
        }

        bool run(const string& dbname, BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            long long start = Listener::getElapsedTimeMillis();
            BSONObjBuilder timeBuilder(128);


            bool authed = cc().getAuthenticationInfo()->isAuthorizedReads("admin");

            result.append("host", prettyHostName() );
            result.append("version", versionString);
            result.append("process","mongod");
            result.append("pid", (int)getpid());
            result.append("uptime",(double) (time(0)-cmdLine.started));
            result.append("uptimeMillis", (long long)(curTimeMillis64()-_started));
            result.append("uptimeEstimate",(double) (start/1000));
            result.appendDate( "localTime" , jsTime() );

            reportLockStats(result);

            {
                BSONObjBuilder t;

                unsigned long long last, start, timeLocked;
                d.dbMutex.info().getTimingInfo(start, timeLocked);
                last = curTimeMicros64();
                double tt = (double) last-start;
                double tl = (double) timeLocked;
                t.append("totalTime", tt);
                t.append("lockTime", tl);
                t.append("ratio", (tt ? tl/tt : 0));

                {
                    BSONObjBuilder ttt( t.subobjStart( "currentQueue" ) );
                    int w=0, r=0;
                    Client::recommendedYieldMicros( &w , &r );
                    ttt.append( "total" , w + r );
                    ttt.append( "readers" , r );
                    ttt.append( "writers" , w );
                    ttt.done();
                }

                {
                    BSONObjBuilder ttt( t.subobjStart( "activeClients" ) );
                    int w=0, r=0;
                    Client::getActiveClientCount( w , r );
                    ttt.append( "total" , w + r );
                    ttt.append( "readers" , r );
                    ttt.append( "writers" , w );
                    ttt.done();
                }



                result.append( "globalLock" , t.obj() );
            }
            timeBuilder.appendNumber( "after basic" , Listener::getElapsedTimeMillis() - start );

            {
                BSONObjBuilder t( result.subobjStart( "mem" ) );

                t.append("bits",  ( sizeof(int*) == 4 ? 32 : 64 ) );

                ProcessInfo p;
                int v = 0;
                if ( p.supported() ) {
                    t.appendNumber( "resident" , p.getResidentSize() );
                    v = p.getVirtualMemorySize();
                    t.appendNumber( "virtual" , v );
                    t.appendBool( "supported" , true );
                }
                else {
                    result.append( "note" , "not all mem info support on this platform" );
                    t.appendBool( "supported" , false );
                }

                timeBuilder.appendNumber( "middle of mem" , Listener::getElapsedTimeMillis() - start );

                int m = (int) (MemoryMappedFile::totalMappedLength() / ( 1024 * 1024 ));
                t.appendNumber( "mapped" , m );
                
                if ( cmdLine.dur ) {
                    m *= 2;
                    t.appendNumber( "mappedWithJournal" , m );
                }
                
                int overhead = v - m - connTicketHolder.used();

                if( overhead > 4000 ) { 
                    t.append("note", "virtual minus mapped is large. could indicate a memory leak");
                    LOGATMOST(60) << "warning: virtual size (" << v << "MB) - mapped size (" << m << "MB) is large (" << overhead << "MB). could indicate a memory leak" << endl;
                }

                t.done();

            }
            timeBuilder.appendNumber( "after mem" , Listener::getElapsedTimeMillis() - start );

            {
                BSONObjBuilder bb( result.subobjStart( "connections" ) );
                bb.append( "current" , connTicketHolder.used() );
                bb.append( "available" , connTicketHolder.available() );
                bb.done();
            }
            timeBuilder.appendNumber( "after connections" , Listener::getElapsedTimeMillis() - start );

            {
                BSONObjBuilder bb( result.subobjStart( "extra_info" ) );
                bb.append("note", "fields vary by platform");
                ProcessInfo p;
                p.getExtraInfo(bb);
                bb.done();
                timeBuilder.appendNumber( "after extra info" , Listener::getElapsedTimeMillis() - start );

            }

            {
                BSONObjBuilder bb( result.subobjStart( "indexCounters" ) );
                globalIndexCounters.append( bb );
                bb.done();
            }

            {
                BSONObjBuilder bb( result.subobjStart( "backgroundFlushing" ) );
                globalFlushCounters.append( bb );
                bb.done();
            }

            {
                BSONObjBuilder bb( result.subobjStart( "cursors" ) );
                ClientCursor::appendStats( bb );
                bb.done();
            }

            {
                BSONObjBuilder bb( result.subobjStart( "network" ) );
                networkCounter.append( bb );
                bb.done();
            }


            timeBuilder.appendNumber( "after counters" , Listener::getElapsedTimeMillis() - start );

            if ( anyReplEnabled() ) {
                BSONObjBuilder bb( result.subobjStart( "repl" ) );
                appendReplicationInfo( bb , authed , cmdObj["repl"].numberInt() );
                bb.done();

                if ( ! _isMaster() ) {
                    result.append( "opcountersRepl" , replOpCounters.getObj() );
                }

            }

            timeBuilder.appendNumber( "after repl" , Listener::getElapsedTimeMillis() - start );

            result.append( "opcounters" , globalOpCounters.getObj() );

            {
                BSONObjBuilder asserts( result.subobjStart( "asserts" ) );
                asserts.append( "regular" , assertionCount.regular );
                asserts.append( "warning" , assertionCount.warning );
                asserts.append( "msg" , assertionCount.msg );
                asserts.append( "user" , assertionCount.user );
                asserts.append( "rollovers" , assertionCount.rollovers );
                asserts.done();
            }

            timeBuilder.appendNumber( "after asserts" , Listener::getElapsedTimeMillis() - start );

            result.append( "writeBacksQueued" , ! writeBackManager.queuesEmpty() );

            if( cmdLine.dur ) {
                result.append("dur", dur::stats.asObj());
            }

            timeBuilder.appendNumber( "after dur" , Listener::getElapsedTimeMillis() - start );

            {
                RamLog* rl = RamLog::get( "warnings" );
                massert(15880, "no ram log for warnings?" , rl);
                
                if (rl->lastWrite() >= time(0)-(10*60)){ // only show warnings from last 10 minutes
                    vector<const char*> lines;
                    rl->get( lines );
                    
                    BSONArrayBuilder arr( result.subarrayStart( "warnings" ) );
                    for ( unsigned i=std::max(0,(int)lines.size()-10); i<lines.size(); i++ )
                        arr.append( lines[i] );
                    arr.done();
                }
            }

            if ( ! authed )
                result.append( "note" , "run against admin for more info" );
            
            timeBuilder.appendNumber( "at end" , Listener::getElapsedTimeMillis() - start );
            if ( Listener::getElapsedTimeMillis() - start > 1000 ) {
                BSONObj t = timeBuilder.obj();
                log() << "serverStatus was very slow: " << t << endl;
                result.append( "timing" , t );
            }

            return true;
        }
    } cmdServerStatus;

    class CmdGetOpTime : public Command {
    public:
        virtual bool slaveOk() const {
            return true;
        }
        virtual void help( stringstream& help ) const { help << "internal"; }
        virtual LockType locktype() const { return NONE; }
        CmdGetOpTime() : Command("getoptime") { }
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
        void help(stringstream& h) const { h << "http://www.mongodb.org/display/DOCS/Monitoring+and+Diagnostics#MonitoringandDiagnostics-DatabaseRecord%2FReplay"; }
        virtual LockType locktype() const { return WRITE; }
        bool run(const string& dbname , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
            int was = _diaglog.setLevel( cmdObj.firstElement().numberInt() );
            _diaglog.flush();
            if ( !cmdLine.quiet )
                tlog() << "CMD: diagLogging set to " << _diaglog.getLevel() << " from: " << was << endl;
            result.append( "was" , was );
            return true;
        }
    } cmddiaglogging;

    /* remove bit from a bit array - actually remove its slot, not a clear
       note: this function does not work with x == 63 -- that is ok
             but keep in mind in the future if max indexes were extended to
             exactly 64 it would be a problem
    */
    unsigned long long removeBit(unsigned long long b, int x) {
        unsigned long long tmp = b;
        return
            (tmp & ((((unsigned long long) 1) << x)-1)) |
            ((tmp >> (x+1)) << x);
    }

    struct DBCommandsUnitTest {
        DBCommandsUnitTest() {
            verify( removeBit(1, 0) == 0 );
            verify( removeBit(2, 0) == 1 );
            verify( removeBit(2, 1) == 0 );
            verify( removeBit(255, 1) == 127 );
            verify( removeBit(21, 2) == 9 );
            verify( removeBit(0x4000000000000001ULL, 62) == 1 );
        }
    } dbc_unittest;

    void assureSysIndexesEmptied(const char *ns, IndexDetails *exceptForIdIndex);
    int removeFromSysIndexes(const char *ns, const char *idxName);

    bool dropIndexes( NamespaceDetails *d, const char *ns, const char *name, string &errmsg, BSONObjBuilder &anObjBuilder, bool mayDeleteIdIndex ) {

        BackgroundOperation::assertNoBgOpInProgForNs(ns);

        d = d->writingWithExtra();
        d->aboutToDeleteAnIndex();

        /* there may be pointers pointing at keys in the btree(s).  kill them. */
        ClientCursor::invalidate(ns);

        // delete a specific index or all?
        if ( *name == '*' && name[1] == 0 ) {
            log(4) << "  d->nIndexes was " << d->nIndexes << '\n';
            anObjBuilder.append("nIndexesWas", (double)d->nIndexes);
            IndexDetails *idIndex = 0;
            if( d->nIndexes ) {
                for ( int i = 0; i < d->nIndexes; i++ ) {
                    if ( !mayDeleteIdIndex && d->idx(i).isIdIndex() ) {
                        idIndex = &d->idx(i);
                    }
                    else {
                        d->idx(i).kill_idx();
                    }
                }
                d->nIndexes = 0;
            }
            if ( idIndex ) {
                d->addIndex(ns) = *idIndex;
                wassert( d->nIndexes == 1 );
            }
            /* assuming here that id index is not multikey: */
            d->multiKeyIndexBits = 0;
            assureSysIndexesEmptied(ns, idIndex);
            anObjBuilder.append("msg", mayDeleteIdIndex ?
                                "indexes dropped for collection" :
                                "non-_id indexes dropped for collection");
        }
        else {
            // delete just one index
            int x = d->findIndexByName(name);
            if ( x >= 0 ) {
                log(4) << "  d->nIndexes was " << d->nIndexes << endl;
                anObjBuilder.append("nIndexesWas", (double)d->nIndexes);

                /* note it is  important we remove the IndexDetails with this
                 call, otherwise, on recreate, the old one would be reused, and its
                 IndexDetails::info ptr would be bad info.
                 */
                IndexDetails *id = &d->idx(x);
                if ( !mayDeleteIdIndex && id->isIdIndex() ) {
                    errmsg = "may not delete _id index";
                    return false;
                }
                id->kill_idx();
                d->multiKeyIndexBits = removeBit(d->multiKeyIndexBits, x);
                d->nIndexes--;
                for ( int i = x; i < d->nIndexes; i++ )
                    d->idx(i) = d->idx(i+1);
            }
            else {
                int n = removeFromSysIndexes(ns, name); // just in case an orphaned listing there - i.e. should have been repaired but wasn't
                if( n ) {
                    log() << "info: removeFromSysIndexes cleaned up " << n << " entries" << endl;
                }
                log() << "dropIndexes: " << name << " not found" << endl;
                errmsg = "index not found";
                return false;
            }
        }
        return true;
    }

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
        virtual void help( stringstream& help ) const { help << "drop a collection\n{drop : <collectionName>}"; }
        virtual LockType locktype() const { return WRITE; }
        virtual bool run(const string& dbname , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
            string nsToDrop = dbname + '.' + cmdObj.firstElement().valuestr();
            NamespaceDetails *d = nsdetails(nsToDrop.c_str());
            if ( !cmdLine.quiet )
                tlog() << "CMD: drop " << nsToDrop << endl;
            if ( d == 0 ) {
                errmsg = "ns not found";
                return false;
            }
            uassert( 10039 ,  "can't drop collection with reserved $ character in name", strchr(nsToDrop.c_str(), '$') == 0 );
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
        virtual bool run(const string& dbname, BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
            string ns = parseNs(dbname, cmdObj);
            string err;
            long long n = runCount(ns.c_str(), cmdObj, err);
            long long nn = n;
            bool ok = true;
            if ( n == -1 ) {
                nn = 0;
                result.appendBool( "missing" , true );
            }
            else if ( n < 0 ) {
                nn = 0;
                ok = false;
                if ( !err.empty() )
                    errmsg = err;
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
        CmdDropIndexes() : Command("dropIndexes", false, "deleteIndexes") { }
        bool run(const string& dbname, BSONObj& jsobj, int, string& errmsg, BSONObjBuilder& anObjBuilder, bool /*fromRepl*/) {
            BSONElement e = jsobj.firstElement();
            string toDeleteNs = dbname + '.' + e.valuestr();
            NamespaceDetails *d = nsdetails(toDeleteNs.c_str());
            if ( !cmdLine.quiet )
                tlog() << "CMD: dropIndexes " << toDeleteNs << endl;
            if ( d ) {
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
        CmdReIndex() : Command("reIndex") { }
        bool run(const string& dbname , BSONObj& jsobj, int, string& errmsg, BSONObjBuilder& result, bool /*fromRepl*/) {
            static DBDirectClient db;

            BSONElement e = jsobj.firstElement();
            string toDeleteNs = dbname + '.' + e.valuestr();
            NamespaceDetails *d = nsdetails(toDeleteNs.c_str());
            tlog() << "CMD: reIndex " << toDeleteNs << endl;
            BackgroundOperation::assertNoBgOpInProgForNs(toDeleteNs.c_str());

            if ( ! d ) {
                errmsg = "ns not found";
                return false;
            }

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
                log(1) << "reIndex ns: " << toDeleteNs << " index: " << o << endl;
                theDataFileMgr.insertWithObjMod( Namespace( toDeleteNs.c_str() ).getSisterNS( "system.indexes" ).c_str() , o , true );
            }

            result.append( "nIndexes" , (int)all.size() );
            result.appendArray( "indexes" , b.obj() );
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
                dbHolder().getAllShortNames( false, allShortNames );
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

            md5digest d;
            md5_state_t st;
            md5_init(&st);

            BSONObj query = BSON( "files_id" << jsobj["filemd5"] );
            BSONObj sort = BSON( "files_id" << 1 << "n" << 1 );

            shared_ptr<Cursor> cursor = NamespaceDetailsTransient::bestGuessCursor(ns.c_str(),
                                                                                   query, sort);
            if ( ! cursor ) {
                errmsg = "need an index on { files_id : 1 , n : 1 }";
                return false;
            }
            auto_ptr<ClientCursor> cc (new ClientCursor(QueryOption_NoCursorTimeout, cursor, ns.c_str()));

            int n = 0;
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
                    log() << "should have chunk: " << n << " have:" << myn << endl;
                    dumpChunks( ns , query , sort );
                    uassert( 10040 ,  "chunks out of order" , n == myn );
                }

                int len;
                const char * data = obj["data"].binDataClean( len );

                ClientCursor::YieldLock yield (cc.get());
                try {
                    md5_append( &st , (const md5_byte_t*)(data) , len );
                    n++;
                }
                catch (...) {
                    if ( ! yield.stillOk() ) // relocks
                        cc.release();
                    throw;
                }

                if ( ! yield.stillOk() ) {
                    cc.release();
                    uasserted(13281, "File deleted during filemd5 command");
                }
            }

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

    static IndexDetails *cmdIndexDetailsForRange( const char *ns, string &errmsg, BSONObj &min, BSONObj &max, BSONObj &keyPattern ) {
        if ( ns[ 0 ] == '\0' || min.isEmpty() || max.isEmpty() ) {
            errmsg = "invalid command syntax (note: min and max are required)";
            return 0;
        }
        return indexDetailsForRange( ns, errmsg, min, max, keyPattern );
    }

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
                 "\nkeyPattern, min, and max parameters are optional."
                 "\nnote: This command may take a while to run";
        }
        bool run(const string& dbname, BSONObj& jsobj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl ) {
            Timer timer;

            string ns = jsobj.firstElement().String();
            BSONObj min = jsobj.getObjectField( "min" );
            BSONObj max = jsobj.getObjectField( "max" );
            BSONObj keyPattern = jsobj.getObjectField( "keyPattern" );
            bool estimate = jsobj["estimate"].trueValue();

            Client::Context ctx( ns );
            NamespaceDetails *d = nsdetails(ns.c_str());

            if ( ! d || d->stats.nrecords == 0 ) {
                result.appendNumber( "size" , 0 );
                result.appendNumber( "numObjects" , 0 );
                result.append( "millis" , timer.millis() );
                return true;
            }

            result.appendBool( "estimate" , estimate );

            shared_ptr<Cursor> c;
            if ( min.isEmpty() && max.isEmpty() ) {
                if ( estimate ) {
                    result.appendNumber( "size" , d->stats.datasize );
                    result.appendNumber( "numObjects" , d->stats.nrecords );
                    result.append( "millis" , timer.millis() );
                    return 1;
                }
                c = theDataFileMgr.findAll( ns.c_str() );
            }
            else if ( min.isEmpty() || max.isEmpty() ) {
                errmsg = "only one of min or max specified";
                return false;
            }
            else {
                IndexDetails *idx = cmdIndexDetailsForRange( ns.c_str(), errmsg, min, max, keyPattern );
                if ( idx == 0 )
                    return false;

                c.reset( BtreeCursor::make( d, d->idxNo(*idx), *idx, min, max, false, 1 ) );
            }

            long long avgObjSize = d->stats.datasize / d->stats.nrecords;

            long long maxSize = jsobj["maxSize"].numberLong();
            long long maxObjects = jsobj["maxObjects"].numberLong();

            long long size = 0;
            long long numObjects = 0;
            while( c->ok() ) {

                if ( estimate )
                    size += avgObjSize;
                else
                    size += c->currLoc().rec()->netLength();

                numObjects++;

                if ( ( maxSize && size > maxSize ) ||
                        ( maxObjects && numObjects > maxObjects ) ) {
                    result.appendBool( "maxReached" , true );
                    break;
                }

                c->advance();
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

            NamespaceDetails * nsd = nsdetails( ns.c_str() );
            if ( ! nsd )
                return 0;

            long long totalSize = 0;

            NamespaceDetails::IndexIterator ii = nsd->ii();
            while ( ii.more() ) {
                IndexDetails& d = ii.next();
                string collNS = d.indexNamespace();
                NamespaceDetails * mine = nsdetails( collNS.c_str() );
                if ( ! mine ) {
                    log() << "error: have index ["  << collNS << "] but no NamespaceDetails" << endl;
                    continue;
                }
                totalSize += mine->stats.datasize;
                if ( details )
                    details->appendNumber( d.indexName() , mine->stats.datasize / scale );
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
        bool run(const string& dbname, BSONObj& jsobj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl ) {
            string ns = dbname + "." + jsobj.firstElement().valuestr();
            Client::Context cx( ns );

            NamespaceDetails * nsd = nsdetails( ns.c_str() );
            if ( ! nsd ) {
                errmsg = "ns not found";
                return false;
            }

            result.append( "ns" , ns.c_str() );

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

            bool verbose = jsobj["verbose"].trueValue();

            long long size = nsd->stats.datasize / scale;
            result.appendNumber( "count" , nsd->stats.nrecords );
            result.appendNumber( "size" , size );
            if( nsd->stats.nrecords )
                result.append      ( "avgObjSize" , double(size) / double(nsd->stats.nrecords) );

            int numExtents;
            BSONArrayBuilder extents;

            result.appendNumber( "storageSize" , nsd->storageSize( &numExtents , verbose ? &extents : 0  ) / scale );
            result.append( "numExtents" , numExtents );
            result.append( "nindexes" , nsd->nIndexes );
            result.append( "lastExtentSize" , nsd->lastExtentSize / scale );
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
        virtual bool slaveOk() const { return true; }
        virtual LockType locktype() const { return WRITE; }
        virtual bool logTheOp() { return true; }
        
        bool run(const string& dbname, BSONObj& jsobj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl ) {
            string ns = dbname + "." + jsobj.firstElement().valuestr();
            Client::Context ctx( ns );
            NamespaceDetails* nsd = nsdetails( ns.c_str() );
            if ( ! nsd ) {
                errmsg = "ns does not exist";
                return false;
            }

            if ( jsobj["usePowerOf2Sizes"].type() ) {
                result.appendBool( "usePowerOf2Sizes_old" , nsd->isUserFlagSet( NamespaceDetails::Flag_UsePowerOf2Sizes ) );
                if ( jsobj["usePowerOf2Sizes"].trueValue() ) {
                    nsd->setUserFlag( NamespaceDetails::Flag_UsePowerOf2Sizes );
                }
                else {
                    nsd->clearUserFlag( NamespaceDetails::Flag_UsePowerOf2Sizes );
                }
            }

            return true;
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
                d->namespaceIndex.getNamespaces( collections );

            long long ncollections = 0;
            long long objects = 0;
            long long size = 0;
            long long storageSize = 0;
            long long numExtents = 0;
            long long indexes = 0;
            long long indexSize = 0;

            for (list<string>::const_iterator it = collections.begin(); it != collections.end(); ++it) {
                const string ns = *it;

                NamespaceDetails * nsd = nsdetails( ns.c_str() );
                if ( ! nsd ) {
                    errmsg = "missing ns: ";
                    errmsg += ns;
                    return false;
                }

                ncollections += 1;
                objects += nsd->stats.nrecords;
                size += nsd->stats.datasize;

                int temp;
                storageSize += nsd->storageSize( &temp );
                numExtents += temp;

                indexes += nsd->nIndexes;
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
                result.appendNumber( "nsSizeMB", (int) d->namespaceIndex.fileLength() / 1024 / 1024 );

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
            NamespaceDetails *nsd = nsdetails( fromNs.c_str() );
            massert( 10301 ,  "source collection " + fromNs + " does not exist", nsd );
            long long excessSize = nsd->stats.datasize - size * 2; // datasize and extentSize can't be compared exactly, so add some padding to 'size'
            DiskLoc extent = nsd->firstExtent;
            for( ; excessSize > extent.ext()->length && extent != nsd->lastExtent; extent = extent.ext()->xnext ) {
                excessSize -= extent.ext()->length;
                log( 2 ) << "cloneCollectionAsCapped skipping extent of size " << extent.ext()->length << endl;
                log( 6 ) << "excessSize: " << excessSize << endl;
            }
            DiskLoc startLoc = extent.ext()->firstRecord;

            CursorId id;
            {
                shared_ptr<Cursor> c = theDataFileMgr.findAll( fromNs.c_str(), startLoc );
                ClientCursor *cc = new ClientCursor(0, c, fromNs.c_str());
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
        virtual bool run(const string& dbname, BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
            BSONObj info = cc().curop()->infoNoauth();
            result << "you" << info[ "client" ];
            return true;
        }
    } cmdWhatsMyUri;

    /* For testing only, not for general use */
    class GodInsert : public Command {
    public:
        GodInsert() : Command( "godinsert" ) { }
        virtual bool adminOnly() const { return false; }
        virtual bool logTheOp() { return false; }
        virtual bool slaveOk() const { return true; }
        virtual LockType locktype() const { return NONE; }
        virtual bool requiresAuth() { return true; }
        virtual void help( stringstream &help ) const {
            help << "internal. for testing only.";
        }
        virtual bool run(const string& dbname, BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {

            AuthenticationInfo *ai = cc().getAuthenticationInfo();
            if ( ! ai->isLocalHost() ) {
                errmsg = "godinsert only works locally";
                return false;
            }

            string coll = cmdObj[ "godinsert" ].valuestrsafe();
            log() << "test only command godinsert invoked coll:" << coll << endl;
            uassert( 13049, "godinsert must specify a collection", !coll.empty() );
            string ns = dbname + "." + coll;
            BSONObj obj = cmdObj[ "obj" ].embeddedObjectUserCheck();
            {
                Lock::DBWrite lk(ns);
                Client::Context ctx( ns );
                theDataFileMgr.insertWithObjMod( ns.c_str(), obj, true );
            }
            return true;
        }
    } cmdGodInsert;
    
    class DBHashCmd : public Command {
    public:
        DBHashCmd() : Command( "dbHash", false, "dbhash" ) {}
        virtual bool slaveOk() const { return true; }
        virtual LockType locktype() const { return READ; }
        virtual bool run(const string& dbname , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
            list<string> colls;
            Database* db = cc().database();
            if ( db )
                db->namespaceIndex.getNamespaces( colls );
            colls.sort();

            result.appendNumber( "numCollections" , (long long)colls.size() );
            result.append( "host" , prettyHostName() );

            md5_state_t globalState;
            md5_init(&globalState);

            BSONObjBuilder bb( result.subobjStart( "collections" ) );
            for ( list<string>::iterator i=colls.begin(); i != colls.end(); i++ ) {
                string c = *i;
                if ( c.find( ".system.profile" ) != string::npos )
                    continue;

                shared_ptr<Cursor> cursor;

                NamespaceDetails * nsd = nsdetails( c.c_str() );

                // debug SERVER-761
                NamespaceDetails::IndexIterator ii = nsd->ii();
                while( ii.more() ) {
                    const IndexDetails &idx = ii.next();
                    if ( !idx.head.isValid() || !idx.info.isValid() ) {
                        log() << "invalid index for ns: " << c << " " << idx.head << " " << idx.info;
                        if ( idx.info.isValid() )
                            log() << " " << idx.info.obj();
                        log() << endl;
                    }
                }

                int idNum = nsd->findIdIndex();
                if ( idNum >= 0 ) {
                    cursor.reset( BtreeCursor::make( nsd , idNum , nsd->idx( idNum ) , BSONObj() , BSONObj() , false , 1 ) );
                }
                else if ( c.find( ".system." ) != string::npos ) {
                    continue;
                }
                else if ( nsd->isCapped() ) {
                    cursor = findTableScan( c.c_str() , BSONObj() );
                }
                else {
                    log() << "can't find _id index for: " << c << endl;
                    continue;
                }

                md5_state_t st;
                md5_init(&st);

                long long n = 0;
                while ( cursor->ok() ) {
                    BSONObj c = cursor->current();
                    md5_append( &st , (const md5_byte_t*)c.objdata() , c.objsize() );
                    n++;
                    cursor->advance();
                }
                md5digest d;
                md5_finish(&st, d);
                string hash = digestToString( d );

                bb.append( c.c_str() + ( dbname.size() + 1 ) , hash );

                md5_append( &globalState , (const md5_byte_t*)hash.c_str() , hash.size() );
            }
            bb.done();

            md5digest d;
            md5_finish(&globalState, d);
            string hash = digestToString( d );

            result.append( "md5" , hash );

            return 1;
        }

    } dbhashCmd;

    /* for diagnostic / testing purposes. */
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
        CmdSleep() : Command("sleep") { }
        bool run(const string& ns, BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            log() << "test only command sleep invoked" << endl;
            int secs = 100;
            if ( cmdObj["secs"].isNumber() )
                secs = cmdObj["secs"].numberInt();
            if( cmdObj.getBoolField("w") ) {
                Lock::GlobalWrite lk;
                sleepsecs(secs);
            }
            else {
                Lock::GlobalRead lk;
                sleepsecs(secs);
            }
            return true;
        }
    } cmdSleep;

    // just for testing
    class CapTrunc : public Command {
    public:
        CapTrunc() : Command( "captrunc" ) {}
        virtual bool slaveOk() const { return false; }
        virtual LockType locktype() const { return WRITE; }
        virtual bool requiresAuth() { return true; }
        virtual bool run(const string& dbname , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
            string coll = cmdObj[ "captrunc" ].valuestrsafe();
            uassert( 13416, "captrunc must specify a collection", !coll.empty() );
            string ns = dbname + "." + coll;
            int n = cmdObj.getIntField( "n" );

            // inclusive range?
            bool inc = cmdObj.getBoolField( "inc" );
            NamespaceDetails *nsd = nsdetails( ns.c_str() );
            ReverseCappedCursor c( nsd );
            massert( 13417, "captrunc collection not found or empty", c.ok() );
            for( int i = 0; i < n; ++i ) {
                massert( 13418, "captrunc invalid n", c.advance() );
            }
            DiskLoc end = c.currLoc();
            nsd->cappedTruncateAfter( ns.c_str(), end, inc );
            return true;
        }
    } capTruncCmd;

    // just for testing
    class EmptyCapped : public Command {
    public:
        EmptyCapped() : Command( "emptycapped" ) {}
        virtual bool slaveOk() const { return false; }
        virtual LockType locktype() const { return WRITE; }
        virtual bool requiresAuth() { return true; }
        virtual bool logTheOp() { return true; }
        virtual bool run(const string& dbname , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
            string coll = cmdObj[ "emptycapped" ].valuestrsafe();
            uassert( 13428, "emptycapped must specify a collection", !coll.empty() );
            string ns = dbname + "." + coll;
            NamespaceDetails *nsd = nsdetails( ns.c_str() );
            massert( 13429, "emptycapped no such collection", nsd );
            nsd->emptyCappedCollection( ns.c_str() );
            return true;
        }
    } emptyCappedCmd;

    bool _execCommand(Command *c, const string& dbname, BSONObj& cmdObj, int queryOptions, BSONObjBuilder& result, bool fromRepl) {

        try {
            string errmsg;
            if ( ! c->run(dbname, cmdObj, queryOptions, errmsg, result, fromRepl ) ) {
                result.append( "errmsg" , errmsg );
                return false;
            }
        }
        catch ( SendStaleConfigException& e ){
            log(1) << "command failed because of stale config, can retry" << causedBy( e ) << endl;
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

        return true;
    }

    /**
     * this handles
     - auth
     - maintenance mode
     - locking
     - context
     then calls run()
    */
    bool execCommand( Command * c ,
                      Client& client , int queryOptions ,
                      const char *cmdns, BSONObj& cmdObj ,
                      BSONObjBuilder& result,
                      bool fromRepl ) {

        string dbname = nsToDatabase( cmdns );

        AuthenticationInfo *ai = client.getAuthenticationInfo();

        if( c->adminOnly() && c->localHostOnlyIfNoAuth( cmdObj ) && noauth && !ai->isLocalHost() ) {
            result.append( "errmsg" ,
                           "unauthorized: this command must run from localhost when running db without auth" );
            log() << "command denied: " << cmdObj.toString() << endl;
            return false;
        }

        if ( c->adminOnly() && ! fromRepl && dbname != "admin" ) {
            result.append( "errmsg" ,  "access denied; use admin db" );
            log() << "command denied: " << cmdObj.toString() << endl;
            return false;
        }

        if ( cmdObj["help"].trueValue() ) {
            client.curop()->ensureStarted();
            stringstream ss;
            ss << "help for: " << c->name << " ";
            c->help( ss );
            result.append( "help" , ss.str() );
            result.append( "lockType" , c->locktype() );
            return true;
        }

        bool canRunHere =
            isMaster( dbname.c_str() ) ||
            c->slaveOk() ||
            ( c->slaveOverrideOk() && ( queryOptions & QueryOption_SlaveOk ) ) ||
            fromRepl;

        if ( ! canRunHere ) {
            result.append( "errmsg" , "not master" );
            result.append( "note" , "from execCommand" );
            return false;
        }

        if ( ! c->maintenanceOk() && theReplSet && ! isMaster( dbname.c_str() ) && ! theReplSet->isSecondary() ) {
            result.append( "errmsg" , "node is recovering" );
            result.append( "note" , "from execCommand" );
            return false;
        }

        if ( c->adminOnly() )
            log( 2 ) << "command: " << cmdObj << endl;

        if (c->maintenanceMode() && theReplSet && theReplSet->isSecondary()) {
            theReplSet->setMaintenanceMode(true);
        }

        bool retval = false;
        if ( c->locktype() == Command::NONE ) {
            verify( !c->lockGlobally() );

            // we also trust that this won't crash
            retval = true;

            if ( c->requiresAuth() ) {
                // test that the user at least as read permissions
                if ( ! client.getAuthenticationInfo()->isAuthorizedReads( dbname ) ) {
                    result.append( "errmsg" , "need to login" );
                    retval = false;
                }
            }

            if (retval) {
                client.curop()->ensureStarted();
                retval = _execCommand(c, dbname , cmdObj , queryOptions, result , fromRepl );
            }
        }
        else if( c->locktype() != Command::WRITE ) { 
            // read lock
            verify( ! c->logTheOp() );
            string ns = c->parseNs(dbname, cmdObj);
            scoped_ptr<Lock::GlobalRead> lk;
            if( c->lockGlobally() )
                lk.reset( new Lock::GlobalRead() );
            Client::ReadContext ctx( ns , dbpath, c->requiresAuth() ); // read locks
            client.curop()->ensureStarted();
            retval = _execCommand(c, dbname , cmdObj , queryOptions, result , fromRepl );
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
            Client::Context ctx( dbname , dbpath , c->requiresAuth() );
            retval = _execCommand(c, dbname , cmdObj , queryOptions, result , fromRepl );
            if ( retval && c->logTheOp() && ! fromRepl ) {
                logOp("c", cmdns, cmdObj);
            }
        }

        if (c->maintenanceMode() && theReplSet) {
            theReplSet->setMaintenanceMode(false);
        }

        return retval;
    }


    /* TODO make these all command objects -- legacy stuff here

       usage:
         abc.$cmd.findOne( { ismaster:1 } );

       returns true if ran a cmd
    */
    bool _runCommands(const char *ns, BSONObj& _cmdobj, BufBuilder &b, BSONObjBuilder& anObjBuilder, bool fromRepl, int queryOptions) {
        string dbname = nsToDatabase( ns );

        if( logLevel >= 1 )
            log() << "run command " << ns << ' ' << _cmdobj << endl;

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
            }
            else {
                jsobj = _cmdobj;
            }
        }

        Client& client = cc();
        bool ok = false;

        BSONElement e = jsobj.firstElement();

        Command * c = e.type() ? Command::findCommand( e.fieldName() ) : 0;

        if ( c ) {
            ok = execCommand( c , client , queryOptions , ns , jsobj , anObjBuilder , fromRepl );
        }
        else {
            anObjBuilder.append("errmsg", str::stream() << "no such cmd: " << e.fieldName() );
            anObjBuilder.append("bad cmd" , _cmdobj );
        }

        // switch to bool, but wait a bit longer before switching?
        // anObjBuilder.append("ok", ok);
        anObjBuilder.append("ok", ok?1.0:0.0);
        BSONObj x = anObjBuilder.done();
        b.appendBuf((void*) x.objdata(), x.objsize());

        return true;
    }

} // namespace mongo
