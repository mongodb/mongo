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

#include "stdafx.h"
#include "query.h"
#include "pdfile.h"
#include "jsobj.h"
#include "../util/builder.h"
#include <time.h>
#include "introspect.h"
#include "btree.h"
#include "../util/lruishmap.h"
#include "../util/md5.hpp"
#include "../util/processinfo.h"
#include "json.h"
#include "repl.h"
#include "replset.h"
#include "commands.h"
#include "db.h"
#include "instance.h"
#include "lasterror.h"
#include "security.h"
#include "queryoptimizer.h"
#include "../scripting/engine.h"

namespace mongo {

    extern int queryTraceLevel;
    extern int otherTraceLevel;
    extern int opLogging;
    void flushOpLog( stringstream &ss );

    void clean(const char *ns, NamespaceDetails *d) {
        for ( int i = 0; i < Buckets; i++ )
            d->deletedList[i].Null();
    }

    /* { validate: "collectionnamewithoutthedbpart" [, scandata: <bool>] } */
    string validateNS(const char *ns, NamespaceDetails *d, BSONObj *cmdObj) {
        bool scanData = true;
        if( cmdObj && cmdObj->hasElement("scandata") && !cmdObj->getBoolField("scandata") )
            scanData = false;
        bool valid = true;
        stringstream ss;
        ss << "\nvalidate\n";
        ss << "  details: " << hex << d << " ofs:" << nsindex(ns)->detailsOffset(d) << dec << endl;
        if ( d->capped )
            ss << "  capped:" << d->capped << " max:" << d->max << '\n';

        ss << "  firstExtent:" << d->firstExtent.toString() << " ns:" << d->firstExtent.ext()->ns.buf << '\n';
        ss << "  lastExtent:" << d->lastExtent.toString()    << " ns:" << d->lastExtent.ext()->ns.buf << '\n';
        try {
            d->firstExtent.ext()->assertOk();
            d->lastExtent.ext()->assertOk();

			DiskLoc el = d->firstExtent;
			int ne = 0;
			while( !el.isNull() ) {
				Extent *e = el.ext();
				e->assertOk();
				el = e->xnext;
				ne++;
			}
			ss << "  # extents:" << ne << '\n';
        } catch (...) {
            valid=false;
            ss << " extent asserted ";
        }

        ss << "  datasize?:" << d->datasize << " nrecords?:" << d->nrecords << " lastExtentSize:" << d->lastExtentSize << '\n';
        ss << "  padding:" << d->paddingFactor << '\n';
        try {

            try {
                ss << "  first extent:\n";
                d->firstExtent.ext()->dump(ss);
                valid = valid && d->firstExtent.ext()->validates();
            }
            catch (...) {
                ss << "\n    exception firstextent\n" << endl;
            }

            set<DiskLoc> recs;
            if( scanData ) { 
                auto_ptr<Cursor> c = theDataFileMgr.findAll(ns);
                int n = 0;
                long long len = 0;
                long long nlen = 0;
                int outOfOrder = 0;
                DiskLoc cl_last;
                while ( c->ok() ) {
                    n++;

                    DiskLoc cl = c->currLoc();
                    if ( n < 1000000 )
                        recs.insert(cl);
                    if ( d->capped ) {
                        if ( cl < cl_last )
                            outOfOrder++;
                        cl_last = cl;
                    }

                    Record *r = c->_current();
                    len += r->lengthWithHeaders;
                    nlen += r->netLength();
                    c->advance();
                }
                if ( d->capped ) {
                    ss << "  capped outOfOrder:" << outOfOrder;
                    if ( outOfOrder > 1 ) {
                        valid = false;
                        ss << " ???";
                    }
                    else ss << " (OK)";
                    ss << '\n';
                }
                ss << "  " << n << " objects found, nobj:" << d->nrecords << "\n";
                ss << "  " << len << " bytes data w/headers\n";
                ss << "  " << nlen << " bytes data wout/headers\n";
            }

            ss << "  deletedList: ";
            for ( int i = 0; i < Buckets; i++ ) {
                ss << (d->deletedList[i].isNull() ? '0' : '1');
            }
            ss << endl;
            int ndel = 0;
            long long delSize = 0;
            int incorrect = 0;
            for ( int i = 0; i < Buckets; i++ ) {
                DiskLoc loc = d->deletedList[i];
                try {
                    int k = 0;
                    while ( !loc.isNull() ) {
                        if ( recs.count(loc) )
                            incorrect++;
                        ndel++;

                        if ( loc.questionable() ) {
                            if ( loc.a() <= 0 || strstr(ns, "hudsonSmall") == 0 ) {
                                ss << "    ?bad deleted loc: " << loc.toString() << " bucket:" << i << " k:" << k << endl;
                                valid = false;
                                break;
                            }
                        }

                        DeletedRecord *d = loc.drec();
                        delSize += d->lengthWithHeaders;
                        loc = d->nextDeleted;
                        k++;
                    }
                } catch (...) {
                    ss <<"    ?exception in deleted chain for bucket " << i << endl;
                    valid = false;
                }
            }
            ss << "  deleted: n: " << ndel << " size: " << delSize << endl;
            if ( incorrect ) {
                ss << "    ?corrupt: " << incorrect << " records from datafile are in deleted list\n";
                valid = false;
            }

            int idxn = 0;
            try  {
                ss << "  nIndexes:" << d->nIndexes << endl;
                for ( ; idxn < d->nIndexes; idxn++ ) {
                    ss << "    " << d->indexes[idxn].indexNamespace() << " keys:" <<
                    d->indexes[idxn].head.btree()->fullValidate(d->indexes[idxn].head, d->indexes[idxn].keyPattern()) << endl;
                }
            }
            catch (...) {
                ss << "\n    exception during index validate idxn:" << idxn << endl;
                valid=false;
            }

        }
        catch (AssertionException) {
            ss << "\n    exception during validate\n" << endl;
            valid = false;
        }

        if ( !valid )
            ss << " ns corrupt, requires dbchk\n";

        return ss.str();
    }

    class CmdShutdown : public Command {
    public:
        virtual bool requiresAuth() { return true; }
        virtual bool adminOnly() { return true; }
        virtual bool logTheOp() {
            return false;
        }
        virtual bool slaveOk() {
            return true;
        }
        virtual void help( stringstream& help ) const {
            help << "shutdown the database.  must be ran against admin db and either (1) ran from localhost or (2) authenticated.\n";
        }
        CmdShutdown() : Command("shutdown") {}
        bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            if( noauth ) {
                // if running without auth, you must be on localhost
                AuthenticationInfo *ai = authInfo.get();
                if( ai == 0 || !ai->isLocalHost ) {
                    log() << "ignoring shutdown cmd from client, not from localhost and running without auth" << endl;
                    errmsg = "unauthorized [2]";
                    return false;
                }
            }
            log() << "terminating, shutdown command received" << endl;
            dbexit( EXIT_CLEAN );
            return true;
        }
    } cmdShutdown;

    /* reset any errors so that getlasterror comes back clean.

       useful before performing a long series of operations where we want to
       see if any of the operations triggered an error, but don't want to check
       after each op as that woudl be a client/server turnaround.
    */
    class CmdResetError : public Command {
    public:
        virtual bool requiresAuth() { return false; }
        virtual bool logTheOp() {
            return false;
        }
        virtual bool slaveOk() {
            return true;
        }
        virtual void help( stringstream& help ) const {
            help << "reset error state (used with getpreverror)";
        }
        CmdResetError() : Command("reseterror") {}
        bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            LastError *le = lastError.get();
            assert( le );
            le->reset();
            return true;
        }
    } cmdResetError;

    class CmdGetLastError : public Command {
    public:
        virtual bool requiresAuth() { return false; }
        virtual bool logTheOp() {
            return false;
        }
        virtual bool slaveOk() {
            return true;
        }
        virtual void help( stringstream& help ) const {
            help << "return error status of the last operation";
        }
        CmdGetLastError() : Command("getlasterror") {}
        bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            LastError *le = lastError.get();
            assert( le );
            le->nPrev--; // we don't count as an operation
            if ( le->nPrev != 1 )
                LastError::noError.appendSelf( result );
            else
                le->appendSelf( result );
            return true;
        }
    } cmdGetLastError;

    /* for testing purposes only */
    class CmdForceError : public Command {
    public:
        virtual bool logTheOp() {
            return false;
        }
        virtual bool slaveOk() {
            return true;
        }
        CmdForceError() : Command("forceerror") {}
        bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            uassert("forced error", false);
            return true;
        }
    } cmdForceError;

    class CmdGetPrevError : public Command {
    public:
        virtual bool requiresAuth() { return false; }
        virtual bool logTheOp() {
            return false;
        }
        virtual void help( stringstream& help ) const {
            help << "check for errors since last reseterror commandcal";
        }
        virtual bool slaveOk() {
            return true;
        }
        CmdGetPrevError() : Command("getpreverror") {}
        bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            LastError *le = lastError.get();
            assert( le );
            le->nPrev--; // we don't count as an operation
            le->appendSelf( result );
            if ( le->valid )
                result.append( "nPrev", le->nPrev );
            else
                result.append( "nPrev", -1 );
            return true;
        }
    } cmdGetPrevError;

    class CmdSwitchToClientErrors : public Command {
    public:
        virtual bool requiresAuth() { return false; }
        virtual bool logTheOp() {
            return false;
        }
        virtual void help( stringstream& help ) const {
            help << "convert to id based errors rather than connection based";
        }
        virtual bool slaveOk() {
            return true;
        }
        CmdSwitchToClientErrors() : Command("switchtoclienterrors") {}
        bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            if ( lastError.getID() ){
                errmsg = "already in client id mode";
                return false;
            }
            LastError *le = lastError.get();
            assert( le );
            le->nPrev--; 
            le->overridenById = true;
            result << "ok" << 1;
            return true;
        }
    } cmdSwitchToClientErrors;

    class CmdDropDatabase : public Command {
    public:
        virtual bool logTheOp() {
            return true;
        }
        virtual void help( stringstream& help ) const {
            help << "drop (delete) this database";
        }
        virtual bool slaveOk() {
            return false;
        }
        CmdDropDatabase() : Command("dropDatabase") {}
        bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            BSONElement e = cmdObj.findElement(name);
            log() << "dropDatabase " << ns << endl;
            int p = (int) e.number();
            if ( p != 1 )
                return false;
            dropDatabase(ns);
            return true;
        }
    } cmdDropDatabase;

    class CmdRepairDatabase : public Command {
    public:
        virtual bool logTheOp() {
            return false;
        }
        virtual bool slaveOk() {
            return true;
        }
        virtual void help( stringstream& help ) const {
            help << "repair database.  also compacts. note: slow.";
        }
        CmdRepairDatabase() : Command("repairDatabase") {}
        bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            BSONElement e = cmdObj.findElement(name);
            log() << "repairDatabase " << ns << endl;
            int p = (int) e.number();
            if ( p != 1 )
                return false;
            e = cmdObj.findElement( "preserveClonedFilesOnFailure" );
            bool preserveClonedFilesOnFailure = e.isBoolean() && e.boolean();
            e = cmdObj.findElement( "backupOriginalFiles" );
            bool backupOriginalFiles = e.isBoolean() && e.boolean();
            return repairDatabase( ns, errmsg, preserveClonedFilesOnFailure, backupOriginalFiles );
        }
    } cmdRepairDatabase;

    /* set db profiling level
       todo: how do we handle profiling information put in the db with replication?
             sensibly or not?
    */
    class CmdProfile : public Command {
    public:
        virtual bool slaveOk() {
            return true;
        }
        virtual void help( stringstream& help ) const {
            help << "enable or disable performance profiling";
        }
        CmdProfile() : Command("profile") {}
        bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            BSONElement e = cmdObj.findElement(name);
            result.append("was", (double) database->profile);
            int p = (int) e.number();
            bool ok = false;
            if ( p == -1 )
                ok = true;
            else if ( p >= 0 && p <= 2 ) {
                ok = true;
                database->profile = p;
            }
            return ok;
        }
    } cmdProfile;

    /*
       > db.$cmd.findOne({timeinfo:1})
       {
        "totalTime" : 1.33875E8 ,
        "lockTime" : 765625.0 ,
        "ratio" : 0.005718954248366013 ,
        "ok" : 1.0
       }
    */
    class CmdTimeInfo : public Command {
    public:
        virtual bool slaveOk() {
            return true;
        }
        CmdTimeInfo() : Command("timeinfo") {
            started = time(0);
        }
        bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            unsigned long long last, start, timeLocked;
            dbMutexInfo.timingInfo(start, timeLocked);
            last = curTimeMicros64();
            double tt = (double) last-start;
            double tl = (double) timeLocked;
            result.append("totalTime", tt);
            result.append("lockTime", tl);
            result.append("ratio", tl/tt);
            result.append("uptime",(double) (time(0)-started));
            return true;
        }
        time_t started;
    } cmdTimeInfo;

    class CmdMemInfo : public Command {
    public:
        virtual bool slaveOk() {
            return true;
        }
        CmdMemInfo() : Command("meminfo") {
            started = time(0);
        }
        bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            result.append("uptime",(double) (time(0)-started));

            ProcessInfo p;
            if ( ! p.supported() ){
                errmsg = "ProcessInfo not supported on this platform";
                return false;
            }
            
            result << "resident" << p.getResidentSize();
            result << "virtual" << p.getVirtualMemorySize();
            return true;
        }
        time_t started;
    } cmdMemInfo;

    /* just to check if the db has asserted */
    class CmdAssertInfo : public Command {
    public:
        virtual bool slaveOk() {
            return true;
        }
        virtual void help( stringstream& help ) const {
            help << "check if any asserts have occurred on the server";
        }
        CmdAssertInfo() : Command("assertinfo") {}
        bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            result.appendBool("dbasserted", lastAssert[0].isSet() || lastAssert[1].isSet() || lastAssert[2].isSet());
            result.appendBool("asserted", lastAssert[0].isSet() || lastAssert[1].isSet() || lastAssert[2].isSet() || lastAssert[3].isSet());
            result.append("assert", lastAssert[AssertRegular].toString());
            result.append("assertw", lastAssert[AssertW].toString());
            result.append("assertmsg", lastAssert[AssertMsg].toString());
            result.append("assertuser", lastAssert[AssertUser].toString());
            return true;
        }
    } cmdAsserts;

    class CmdGetOpTime : public Command {
    public:
        virtual bool slaveOk() {
            return true;
        }
        CmdGetOpTime() : Command("getoptime") { }
        bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            result.appendDate("optime", OpTime::now().asDate());
            return true;
        }
    } cmdgetoptime;

    /*
    class Cmd : public Command {
    public:
        Cmd() : Command("") { }
        bool adminOnly() { return true; }
        bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result) {
            return true;
        }
    } cmd;
    */

    class CmdOpLogging : public Command {
    public:
        virtual bool slaveOk() {
            return true;
        }
        CmdOpLogging() : Command("opLogging") { }
        bool adminOnly() {
            return true;
        }
        bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool) {
            opLogging = (int) cmdObj.findElement(name).number();
            stringstream ss;
            flushOpLog( ss );
            out() << ss.str() << endl;
            if ( !cmdLine.quiet )
                log() << "CMD: opLogging set to " << opLogging << endl;
            return true;
        }
    } cmdoplogging;

    unsigned removeBit(unsigned b, int x) {
        unsigned tmp = b;
        return 
            (tmp & ((1 << x)-1)) | 
            ((tmp >> (x+1)) << x);
    }

    struct DBCommandsUnitTest {
        DBCommandsUnitTest() {
            assert( removeBit(1, 0) == 0 );
            assert( removeBit(2, 0) == 1 );
            assert( removeBit(2, 1) == 0 );
            assert( removeBit(255, 1) == 127 );
            assert( removeBit(21, 2) == 9 );
        }
    } dbc_unittest;

    bool deleteIndexes( NamespaceDetails *d, const char *ns, const char *name, string &errmsg, BSONObjBuilder &anObjBuilder, bool mayDeleteIdIndex ) {
        
        d->aboutToDeleteAnIndex();
        
        /* there may be pointers pointing at keys in the btree(s).  kill them. */
        ClientCursor::invalidate(ns);
        
        // delete a specific index or all?
        if ( *name == '*' && name[1] == 0 ) {
            log() << "  d->nIndexes was " << d->nIndexes << '\n';
            anObjBuilder.append("nIndexesWas", (double)d->nIndexes);
            IndexDetails *idIndex = 0;
            if( d->nIndexes ) { 
                for ( int i = 0; i < d->nIndexes; i++ ) {
                    if ( !mayDeleteIdIndex && d->indexes[i].isIdIndex() ) {
                        idIndex = &d->indexes[i];
                    } else {
                        d->indexes[i].kill();
                    }
                }
                d->nIndexes = 0;
            }
            if ( idIndex ) {
                d->indexes[ 0 ] = *idIndex;
                d->nIndexes = 1;
            }
            /* assuming here that id index is not multikey: */
            d->multiKeyIndexBits = 0;
            anObjBuilder.append("msg", "all indexes deleted for collection");
        }
        else {
            // delete just one index
            int x = d->findIndexByName(name);
            if ( x >= 0 ) {
                out() << "  d->nIndexes was " << d->nIndexes << endl;
                anObjBuilder.append("nIndexesWas", (double)d->nIndexes);
                
                /* note it is  important we remove the IndexDetails with this
                 call, otherwise, on recreate, the old one would be reused, and its
                 IndexDetails::info ptr would be bad info.
                 */
                IndexDetails *id = &d->indexes[x];
                if ( !mayDeleteIdIndex && id->isIdIndex() ) {
                    errmsg = "may not delete _id index";
                    return false;
                }
                d->indexes[x].kill();
                d->multiKeyIndexBits = removeBit(d->multiKeyIndexBits, x);
                d->nIndexes--;
                for ( int i = x; i < d->nIndexes; i++ )
                    d->indexes[i] = d->indexes[i+1];
            } else {
                log() << "deleteIndexes: " << name << " not found" << endl;
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
        virtual bool slaveOk() {
            return false;
        }
        virtual bool adminOnly() {
            return false;
        }
        virtual bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool) {
            string nsToDrop = database->name + '.' + cmdObj.findElement(name).valuestr();
            NamespaceDetails *d = nsdetails(nsToDrop.c_str());
            if ( !cmdLine.quiet )
                log() << "CMD: drop " << nsToDrop << endl;
            if ( d == 0 ) {
                errmsg = "ns not found";
                return false;
            }
            uassert( "can't drop collection with reserved $ character in name", strchr(nsToDrop.c_str(), '$') == 0 );
            dropCollection( nsToDrop, errmsg, result );
            return true;
        }
    } cmdDrop;

    class CmdQueryTraceLevel : public Command {
    public:
        virtual bool slaveOk() {
            return true;
        }
        CmdQueryTraceLevel() : Command("queryTraceLevel") { }
        bool adminOnly() {
            return true;
        }
        bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool) {
            queryTraceLevel = (int) cmdObj.findElement(name).number();
            return true;
        }
    } cmdquerytracelevel;

    class CmdTraceAll : public Command {
    public:
        virtual bool slaveOk() {
            return true;
        }
        CmdTraceAll() : Command("traceAll") { }
        bool adminOnly() {
            return true;
        }
        bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool) {
            queryTraceLevel = otherTraceLevel = (int) cmdObj.findElement(name).number();
            return true;
        }
    } cmdtraceall;

    /* select count(*) */
    class CmdCount : public Command {
    public:
        CmdCount() : Command("count") { }
        virtual bool logTheOp() {
            return false;
        }
        virtual bool slaveOk() {
			// ok on --slave setups, not ok for nonmaster of a repl pair (unless override)
            return slave == SimpleSlave;
        }
        virtual bool slaveOverrideOk() {
            return true;
        }
        virtual bool adminOnly() {
            return false;
        }
        virtual bool run(const char *_ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool) {
            string ns = database->name + '.' + cmdObj.findElement(name).valuestr();
            string err;
            long long n = runCount(ns.c_str(), cmdObj, err);
            long long nn = n;
            bool ok = true;
            if ( n == -1 ){
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
        virtual bool slaveOk() {
            return false;
        }
        virtual bool adminOnly() {
            return false;
        }
        virtual void help( stringstream& help ) const {
            help << "create a collection";
        }
        virtual bool run(const char *_ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool) {
            string ns = database->name + '.' + cmdObj.findElement(name).valuestr();
            string err;
            bool ok = userCreateNS(ns.c_str(), cmdObj, err, true);
            if ( !ok && !err.empty() )
                errmsg = err;
            return ok;
        }
    } cmdCreate;

    class CmdDeleteIndexes : public Command {
    public:
        virtual bool logTheOp() {
            return true;
        }
        virtual bool slaveOk() {
            return false;
        }
        virtual void help( stringstream& help ) const {
            help << "delete indexes for a collection";
        }
        CmdDeleteIndexes() : Command("deleteIndexes") { }
        bool run(const char *ns, BSONObj& jsobj, string& errmsg, BSONObjBuilder& anObjBuilder, bool /*fromRepl*/) {
            /* note: temp implementation.  space not reclaimed! */
            BSONElement e = jsobj.findElement(name.c_str());
            string toDeleteNs = database->name + '.' + e.valuestr();
            NamespaceDetails *d = nsdetails(toDeleteNs.c_str());
            if ( !cmdLine.quiet )
                log() << "CMD: deleteIndexes " << toDeleteNs << endl;
            if ( d ) {
                BSONElement f = jsobj.findElement("index");
                if ( f.type() == String ) {
                    return deleteIndexes( d, toDeleteNs.c_str(), f.valuestr(), errmsg, anObjBuilder, false );
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
    } cmdDeleteIndexes;
    
    class CmdListDatabases : public Command {
    public:
        virtual bool logTheOp() {
            return false;
        }
        virtual bool slaveOk() {
            return true;
        }
        virtual bool slaveOverrideOk() {
            return true;
        }
        virtual bool adminOnly() {
            return true;
        }
        CmdListDatabases() : Command("listDatabases") {}
        bool run(const char *ns, BSONObj& jsobj, string& errmsg, BSONObjBuilder& result, bool /*fromRepl*/) {
            vector< string > dbNames;
            getDatabaseNames( dbNames );
            vector< BSONObj > dbInfos;
            
            set<string> seen;
            boost::intmax_t totalSize = 0;
            for ( vector< string >::iterator i = dbNames.begin(); i != dbNames.end(); ++i ) {
                BSONObjBuilder b;
                b.append( "name", i->c_str() );
                boost::intmax_t size = dbSize( i->c_str() );
                b.append( "sizeOnDisk", (double) size );
                setClientTempNs( i->c_str() );
                b.appendBool( "empty", clientIsEmpty() );
                totalSize += size;
                dbInfos.push_back( b.obj() );

                seen.insert( i->c_str() );
            }
            
            for ( map<string,Database*>::iterator i = databases.begin(); i != databases.end(); i++ ){
                string name = i->first;
                name = name.substr( 0 , name.find( ":" ) );

                if ( seen.count( name ) )
                    continue;
                
                BSONObjBuilder b;
                b << "name" << name << "sizeOnDisk" << double( 1 );
                setClientTempNs( name.c_str() );
                b.appendBool( "empty", clientIsEmpty() );
                
                dbInfos.push_back( b.obj() );
            }

            result.append( "databases", dbInfos );
            result.append( "totalSize", double( totalSize ) );
            return true;
        }
    } cmdListDatabases;

    class CmdCloseAllDatabases : public Command {
    public:
        virtual bool adminOnly() { return true; }
        virtual bool slaveOk() { return false; }
        CmdCloseAllDatabases() : Command( "closeAllDatabases" ) {}
        bool run(const char *ns, BSONObj& jsobj, string& errmsg, BSONObjBuilder& result, bool /*fromRepl*/) {
            set< string > dbs;
            for ( map<string,Database*>::iterator i = databases.begin(); i != databases.end(); i++ ) {
                string name = i->first;
                name = name.substr( 0 , name.find( ":" ) );
                dbs.insert( name );
            }
            for( set< string >::iterator i = dbs.begin(); i != dbs.end(); ++i ) {
                setClientTempNs( i->c_str() );
                closeClient( i->c_str() );
            }

            return true;
        }
    } cmdCloseAllDatabases;

    class CmdFileMD5 : public Command {
    public:
        CmdFileMD5() : Command( "filemd5" ){}
        virtual bool slaveOk() {
            return true;
        }
        virtual void help( stringstream& help ) const {
            help << " example: { filemd5 : ObjectId(aaaaaaa) , key : { ts : 1 } }";
        }
        bool run(const char *dbname, BSONObj& jsobj, string& errmsg, BSONObjBuilder& result, bool fromRepl ){
            static DBDirectClient db;
            
            string ns = nsToClient( dbname );
            ns += ".";
            {
                string root = jsobj.getStringField( "root" );
                if ( root.size() == 0 )
                    root = "fs";
                ns += root;
            }
            ns += ".chunks"; // make this an option in jsobj
            
            BSONObjBuilder query;
            query.appendAs( jsobj["filemd5"] , "files_id" );
            Query q( query.obj() );
            q.sort( BSON( "files_id" << 1 << "n" << 1 ) ); 

            md5digest d;
            md5_state_t st;
            md5_init(&st);

            dbtemprelease temp;
            
            auto_ptr<DBClientCursor> cursor = db.query( ns.c_str() , q );
            int n = 0;
            while ( cursor->more() ){
                BSONObj c = cursor->next();
                int myn = c.getIntField( "n" );
                if ( n != myn ){
                    log() << "should have chunk: " << n << " have:" << myn << endl;
                    uassert( "chunks out of order" , n == myn );
                }
                
                int len;
                const char * data = c["data"].binData( len );
                md5_append( &st , (const md5_byte_t*)(data + 4) , len - 4 );

                n++;
            }
            md5_finish(&st, d);
            
            result.append( "md5" , digestToString( d ) );                
            return true;
        }
    } cmdFileMD5;
        
    IndexDetails *cmdIndexDetailsForRange( const char *ns, string &errmsg, BSONObj &min, BSONObj &max, BSONObj &keyPattern ) {
        if ( ns[ 0 ] == '\0' || min.isEmpty() || max.isEmpty() ) {
            errmsg = "invalid command syntax (note: min and max are required)";
            return 0;
        }
        return indexDetailsForRange( ns, errmsg, min, max, keyPattern );
    }
        
    class CmdMedianKey : public Command {
    public:
        CmdMedianKey() : Command( "medianKey" ) {}
        virtual bool slaveOk() { return true; }
        virtual void help( stringstream &help ) const {
            help << " example: { medianKey:\"blog.posts\", keyPattern:{x:1}, min:{x:10}, max:{x:55} }\n"
                "NOTE: This command may take awhile to run";
        }
        bool run(const char *dbname, BSONObj& jsobj, string& errmsg, BSONObjBuilder& result, bool fromRepl ){
            const char *ns = jsobj.getStringField( "medianKey" );
            BSONObj min = jsobj.getObjectField( "min" );
            BSONObj max = jsobj.getObjectField( "max" );
            BSONObj keyPattern = jsobj.getObjectField( "keyPattern" );
            
            IndexDetails *id = cmdIndexDetailsForRange( ns, errmsg, min, max, keyPattern );
            if ( id == 0 )
                return false;

            Timer t;
            int num = 0;
            NamespaceDetails *d = nsdetails(ns);
            int idxNo = d->idxNo(*id);
            for( BtreeCursor c( d, idxNo, *id, min, max, false, 1 ); c.ok(); c.advance(), ++num );
            num /= 2;
            BtreeCursor c( d, idxNo, *id, min, max, false, 1 );
            for( ; num; c.advance(), --num );
            int ms = t.millis();
            if ( ms > 100 ) {
                out() << "Finding median for index: " << keyPattern << " between " << min << " and " << max << " took " << ms << "ms." << endl;
            }
            
            if ( !c.ok() ) {
                errmsg = "no index entries in the specified range";
                return false;
            }
            
            result.append( "median", c.prettyKey( c.currKey() ) );
            return true;
        }
    } cmdMedianKey;
    
    class CmdDatasize : public Command {
    public:
        CmdDatasize() : Command( "datasize" ) {}
        virtual bool slaveOk() { return true; }
        virtual void help( stringstream &help ) const {
            help << 
                "\ndetermine data size for a set of data in a certain range"
                "\nexample: { datasize:\"blog.posts\", keyPattern:{x:1}, min:{x:10}, max:{x:55} }"
                "\nkeyPattern, min, and max parameters are optional."
                "\nnot: This command may take a while to run";
        }
        bool run(const char *dbname, BSONObj& jsobj, string& errmsg, BSONObjBuilder& result, bool fromRepl ){
            const char *ns = jsobj.getStringField( "datasize" );
            BSONObj min = jsobj.getObjectField( "min" );
            BSONObj max = jsobj.getObjectField( "max" );
            BSONObj keyPattern = jsobj.getObjectField( "keyPattern" );

            auto_ptr< Cursor > c;
            if ( min.isEmpty() && max.isEmpty() ) {
                setClient( ns );
                c = theDataFileMgr.findAll( ns );
            } else if ( min.isEmpty() || max.isEmpty() ) {
                errmsg = "only one of min or max specified";
                return false;
            } else {            
                IndexDetails *idx = cmdIndexDetailsForRange( ns, errmsg, min, max, keyPattern );
                if ( idx == 0 )
                    return false;
                NamespaceDetails *d = nsdetails(ns);
                c.reset( new BtreeCursor( d, d->idxNo(*idx), *idx, min, max, false, 1 ) );
            }
            
            Timer t;
            long long size = 0;
            long long numObjects = 0;
            while( c->ok() ) {
                size += c->current().objsize();
                c->advance();
                numObjects++;
            }
            int ms = t.millis();
            if ( ms > 100 ) {
                if ( min.isEmpty() ) {
                    out() << "Finding size for ns: " << ns << " took " << ms << "ms." << endl;
                } else {
                    out() << "Finding size for ns: " << ns << " between " << min << " and " << max << " took " << ms << "ms." << endl;
                }
            }
            
            result.append( "size", (double)size );
            result.append( "numObjects" , (double)numObjects );
            return true;
        }
    } cmdDatasize;

    class CollectionStats : public Command {
    public:
        CollectionStats() : Command( "collstats" ) {}
        virtual bool slaveOk() { return true; }
        virtual void help( stringstream &help ) const {
            help << " example: { collstats:\"blog.posts\" } ";
        }
        bool run(const char *dbname, BSONObj& jsobj, string& errmsg, BSONObjBuilder& result, bool fromRepl ){
            string ns = dbname;
            if ( ns.find( "." ) != string::npos )
                ns = ns.substr( 0 , ns.find( "." ) );
            ns += ".";
            ns += jsobj.firstElement().valuestr();
            
            NamespaceDetails * nsd = nsdetails( ns.c_str() );
            if ( ! nsd ){
                errmsg = "ns not found";
                return false;
            }
            
            result.append( "ns" , ns.c_str() );
            
            result.append( "count" , nsd->nrecords );
            result.append( "size" , nsd->datasize );
            result.append( "storageSize" , nsd->storageSize() );
            result.append( "nindexes" , nsd->nIndexes );

            if ( nsd->capped ){
                result.append( "capped" , nsd->capped );
                result.append( "max" , nsd->max );
            }

            return true;
        }
    } cmdCollectionStatis;

    class CmdBuildInfo : public Command {
    public:
        CmdBuildInfo() : Command( "buildinfo" ) {}
        virtual bool slaveOk() { return true; }
        virtual bool adminOnly() { return true; }
        virtual void help( stringstream &help ) const {
            help << "example: { buildinfo:1 }";
        }
        bool run(const char *dbname, BSONObj& jsobj, string& errmsg, BSONObjBuilder& result, bool fromRepl ){
            result << "version" << versionString << "gitVersion" << gitVersion() << "sysInfo" << sysInfo();
            result << "bits" << ( sizeof( int* ) == 4 ? 32 : 64 );
            return true;
        }
    } cmdBuildInfo;
    
    class CmdCloneCollectionAsCapped : public Command {
    public:
        CmdCloneCollectionAsCapped() : Command( "cloneCollectionAsCapped" ) {}
        virtual bool slaveOk() { return false; }
        virtual void help( stringstream &help ) const {
            help << "example: { cloneCollectionAsCapped:<fromName>, toCollection:<toName>, size:<sizeInBytes> }";
        }
        bool run(const char *dbname, BSONObj& jsobj, string& errmsg, BSONObjBuilder& result, bool fromRepl ){
            string from = jsobj.getStringField( "cloneCollectionAsCapped" );
            string to = jsobj.getStringField( "toCollection" );
            long long size = (long long)jsobj.getField( "size" ).number();
            
            if ( from.empty() || to.empty() || size == 0 ) {
                errmsg = "invalid command spec";
                return false;
            }

            char realDbName[256];
            nsToClient( dbname, realDbName );
            
            string fromNs = string( realDbName ) + "." + from;
            string toNs = string( realDbName ) + "." + to;
            massert( "source collection " + fromNs + " does not exist", !setClientTempNs( fromNs.c_str() ) );
            NamespaceDetails *nsd = nsdetails( fromNs.c_str() );
            massert( "source collection " + fromNs + " does not exist", nsd );
            long long excessSize = nsd->datasize - size * 2;
            DiskLoc extent = nsd->firstExtent;
            for( ; excessSize > 0 && extent != nsd->lastExtent; extent = extent.ext()->xnext ) {
                excessSize -= extent.ext()->length;
                if ( excessSize > 0 )
                    log( 2 ) << "cloneCollectionAsCapped skipping extent of size " << extent.ext()->length << endl;
                log( 6 ) << "excessSize: " << excessSize << endl;
            }
            DiskLoc startLoc = extent.ext()->firstRecord;
            
            CursorId id;
            {
                auto_ptr< Cursor > c = theDataFileMgr.findAll( fromNs.c_str(), startLoc );
                ClientCursor *cc = new ClientCursor();
                cc->c = c;
                cc->ns = fromNs;
                cc->matcher.reset( new KeyValJSMatcher( BSONObj(), fromjson( "{$natural:1}" ) ) );
                id = cc->cursorid;
            }
            
            DBDirectClient client;
            setClientTempNs( toNs.c_str() );
            BSONObjBuilder spec;
            spec.appendBool( "capped", true );
            spec.append( "size", double( size ) );
            if ( !userCreateNS( toNs.c_str(), spec.done(), errmsg, true ) )
                return false;
            
            auto_ptr< DBClientCursor > c = client.getMore( fromNs, id );
            while( c->more() ) {
                BSONObj obj = c->next();
                theDataFileMgr.insertAndLog( toNs.c_str(), obj, true );
            }
            
            return true;
        }        
    } cmdCloneCollectionAsCapped;
    
    class CmdConvertToCapped : public Command {
    public:
        CmdConvertToCapped() : Command( "convertToCapped" ) {}
        virtual bool slaveOk() { return false; }
        virtual void help( stringstream &help ) const {
            help << "example: { convertToCapped:<fromCollectionName>, size:<sizeInBytes> }";
        }
        bool run(const char *dbname, BSONObj& jsobj, string& errmsg, BSONObjBuilder& result, bool fromRepl ){
            string from = jsobj.getStringField( "convertToCapped" );
            long long size = (long long)jsobj.getField( "size" ).number();
            
            if ( from.empty() || size == 0 ) {
                errmsg = "invalid command spec";
                return false;
            }
            
            char realDbName[256];
            nsToClient( dbname, realDbName );

            DBDirectClient client;
            client.dropCollection( string( realDbName ) + "." + from + ".$temp_convertToCapped" );

            BSONObj info;
            if ( !client.runCommand( realDbName,
                                    BSON( "cloneCollectionAsCapped" << from << "toCollection" << ( from + ".$temp_convertToCapped" ) << "size" << double( size ) ),
                                    info ) ) {
                errmsg = "cloneCollectionAsCapped failed: " + string(info);
                return false;
            }
            
            if ( !client.dropCollection( string( realDbName ) + "." + from ) ) {
                errmsg = "failed to drop original collection";
                return false;
            }
            
            if ( !client.runCommand( "admin",
                                    BSON( "renameCollection" << ( string( realDbName ) + "." + from + ".$temp_convertToCapped" ) << "to" << ( string( realDbName ) + "." + from ) ),
                                    info ) ) {
                errmsg = "renameCollection failed: " + string(info);
                return false;
            }

            return true;
        }
    } cmdConvertToCapped;

    class GroupCommand : public Command {
    public:
        GroupCommand() : Command("group"){}
        virtual bool slaveOk() { return true; }
        virtual void help( stringstream &help ) const {
            help << "see http://www.mongodb.org/display/DOCS/Aggregation";
        }

        BSONObj getKey( const BSONObj& obj , const BSONObj& keyPattern , ScriptingFunction func , double avgSize , Scope * s ){
            if ( func ){
                BSONObjBuilder b( obj.objsize() + 32 );
                b.append( "0" , obj );
                int res = s->invoke( func , b.obj() );
                uassert( (string)"invoke failed in $keyf: " + s->getError() , res == 0 );
                int type = s->type("return");
                uassert( "return of $key has to be a function" , type == Object );
                return s->getObject( "return" );
            }
            return obj.extractFields( keyPattern , true );
        }
        
        bool group( string realdbname , auto_ptr<DBClientCursor> cursor , 
                    BSONObj keyPattern , string keyFunctionCode , string reduceCode , const char * reduceScope ,
                    BSONObj initial , string finalize ,
                    string& errmsg , BSONObjBuilder& result ){


            auto_ptr<Scope> s = globalScriptEngine->getPooledScope( realdbname );
            s->localConnect( realdbname.c_str() );
            
            if ( reduceScope )
                s->init( reduceScope );

            s->setObject( "$initial" , initial , true );
            
            s->exec( "$reduce = " + reduceCode , "reduce setup" , false , true , true , 100 );
            s->exec( "$arr = [];" , "reduce setup 2" , false , true , true , 100 );
            ScriptingFunction f = s->createFunction(
                "function(){ "
                "  if ( $arr[n] == null ){ "
                "    next = {}; "
                "    Object.extend( next , $key ); "
                "    Object.extend( next , $initial ); "
                "    $arr[n] = next; "
                "    next = null; "
                "  } "
                "  $reduce( obj , $arr[n] ); "
                "}" );

            ScriptingFunction keyFunction = 0;
            if ( keyFunctionCode.size() ){
                keyFunction = s->createFunction( keyFunctionCode.c_str() );
            }


            double keysize = keyPattern.objsize() * 3;
            double keynum = 1;
            
            map<BSONObj,int,BSONObjCmp> map;
            list<BSONObj> blah;
            
            while ( cursor->more() ){
                BSONObj obj = cursor->next();
                BSONObj key = getKey( obj , keyPattern , keyFunction , keysize / keynum , s.get() );
                keysize += key.objsize();
                keynum++;
                
                int& n = map[key];
                if ( n == 0 ){
                    n = map.size();
                    s->setObject( "$key" , key , true );

                    uassert( "group() can't handle more than 10000 unique keys" , n < 10000 );
                }
                
                s->setObject( "obj" , obj , true );
                s->setNumber( "n" , n - 1 );
                if ( s->invoke( f , BSONObj() , 0 , true ) ){
                    throw UserException( (string)"reduce invoke failed: " + s->getError() );
                }
            }
            
            if (!finalize.empty()){
                s->exec( "$finalize = " + finalize , "finalize define" , false , true , true , 100 );
                ScriptingFunction g = s->createFunction(
                    "function(){ "
                    "  for(var i=0; i < $arr.length; i++){ "
                    "  var ret = $finalize($arr[i]); "
                    "  if (ret !== undefined) "
                    "    $arr[i] = ret; "
                    "  } "
                    "}" );
                s->invoke( g , BSONObj() , 0 , true );
            }
            
            result.appendArray( "retval" , s->getObject( "$arr" ) );
            result.append( "count" , keynum - 1 );
            result.append( "keys" , (int)(map.size()) );

            return true;
        }
        
        bool run(const char *dbname, BSONObj& jsobj, string& errmsg, BSONObjBuilder& result, bool fromRepl ){
            static DBDirectClient db;

            /* db.$cmd.findOne( { group : <p> } ) */
            const BSONObj& p = jsobj.firstElement().embeddedObjectUserCheck();
            
            BSONObj q;
            if ( p["cond"].type() == Object )
                q = p["cond"].embeddedObject();
            
            string ns = dbname;
            ns = ns.substr( 0 , ns.size() - 4 );
            string realdbname = ns.substr( 0 , ns.size() - 1 );
            
            if ( p["ns"].type() != String ){
                errmsg = "ns has to be set";
                return false;
            }
            
            ns += p["ns"].valuestr();

            auto_ptr<DBClientCursor> cursor = db.query( ns , q );
            
            BSONObj key;
            string keyf;
            if ( p["key"].type() == Object ){
                key = p["key"].embeddedObjectUserCheck();
                if ( ! p["$keyf"].eoo() ){
                    errmsg = "can't have key and $keyf";
                    return false;
                }
            }
            else if ( p["$keyf"].type() ){
                keyf = p["$keyf"].ascode();
            }
            else { 
                // no key specified, will use entire object as key
            }

            BSONElement reduce = p["$reduce"];

            string finalize;
            if (p["finalize"].type())
                finalize = p["finalize"].ascode();
            
            return group( realdbname , cursor , 
                          key , keyf , reduce.ascode() , reduce.type() != CodeWScope ? 0 : reduce.codeWScopeScopeData() ,
                          p["initial"].embeddedObjectUserCheck() , finalize ,
                          errmsg , result );
        }
        
    } cmdGroup;


    class DistinctCommand : public Command {
    public:
        DistinctCommand() : Command("distinct"){}
        virtual bool slaveOk() { return true; }
        
        virtual void help( stringstream &help ) const {
            help << "{ distinct : 'collection name' , key : 'a.b' }";
        }
        
        bool run(const char *dbname, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl ){
            static DBDirectClient db;
            
            string ns = database->name + '.' + cmdObj.findElement(name).valuestr();            
            string key = cmdObj["key"].valuestrsafe();
            
            BSONObj keyPattern = BSON( key << 1 );
            
            set<BSONObj,BSONObjCmp> map;

            long long size = 0;
            
            auto_ptr<DBClientCursor> cursor = db.query( ns , BSONObj() , 0 , 0 , &keyPattern );
            while ( cursor->more() ){
                BSONObj o = cursor->next();
                BSONObj value = o.extractFields( keyPattern );
                if ( map.insert( value ).second ){
                    size += o.objsize() + 20;
                    uassert( "distinct too big, 4mb cap" , size < 4 * 1024 * 1024 );
                }
            }
            
            BSONObjBuilder b( size );
            int n=0;
            for ( set<BSONObj,BSONObjCmp>::iterator i = map.begin() ; i != map.end(); i++ ){
                b.appendAs( i->firstElement() , b.numStr( n++ ).c_str() );
            }

            result.appendArray( "values" , b.obj() );

            return true;
        }
        
    } distinctCmd;

    
    extern map<string,Command*> *commands;

    /* TODO make these all command objects -- legacy stuff here

       usage:
         abc.$cmd.findOne( { ismaster:1 } );

       returns true if ran a cmd
    */
    bool _runCommands(const char *ns, BSONObj& _cmdobj, stringstream& ss, BufBuilder &b, BSONObjBuilder& anObjBuilder, bool fromRepl, int queryOptions) {
        log(1) << "run command " << ns << ' ' << _cmdobj << endl;

        const char *p = strchr(ns, '.');
        if ( !p ) return false;
        if ( strcmp(p, ".$cmd") != 0 ) return false;

        BSONObj jsobj;
        {
            BSONElement e = _cmdobj.firstElement();
            if ( e.type() == Object && string("query") == e.fieldName() ) {
                jsobj = e.embeddedObject();
            }
            else {
                jsobj = _cmdobj;
            }
        }

        bool ok = false;
        bool valid = false;

        BSONElement e;
        e = jsobj.firstElement();

        map<string,Command*>::iterator i;

        if ( e.eoo() )
            ;
        /* check for properly registered command objects.  Note that all the commands below should be
           migrated over to the command object format.
           */
        else if ( (i = commands->find(e.fieldName())) != commands->end() ) {
            valid = true;
            string errmsg;
            Command *c = i->second;
            AuthenticationInfo *ai = authInfo.get();
			assert( ai );
            uassert("unauthorized", ai->isAuthorized(database->name.c_str()) || !c->requiresAuth());

            bool admin = c->adminOnly();
            if ( admin && !fromRepl && strncmp(ns, "admin", 5) != 0 ) {
                ok = false;
                errmsg = "access denied";
                cout << "command denied: " << jsobj.toString() << endl;
            }
            else if ( isMaster() ||
                      c->slaveOk() ||
                      ( c->slaveOverrideOk() && ( queryOptions & Option_SlaveOk ) ) ||
                      fromRepl ) 
            {
                if ( jsobj.getBoolField( "help" ) ) {
                    stringstream help;
                    help << "help for: " << e.fieldName() << " ";
                    c->help( help );
                    anObjBuilder.append( "help" , help.str() );                    
                } else {
                    if( admin )
                        log( 2 ) << "command: " << jsobj << endl;
                    ok = c->run(ns, jsobj, errmsg, anObjBuilder, fromRepl);
                    if ( ok && c->logTheOp() && !fromRepl )
                        logOp("c", ns, jsobj);
                }
            }
            else {
                ok = false;
                errmsg = "not master";
            }
            if ( !ok )
                anObjBuilder.append("errmsg", errmsg);
        }
        else if ( e.type() == String ) {
            AuthenticationInfo *ai = authInfo.get();
			assert( ai );
            uassert("unauthorized", ai->isAuthorized(database->name.c_str()));

            /* { count: "collectionname"[, query: <query>] } */
            string us(ns, p-ns);

            /* we allow clean and validate on slaves */
            if ( strcmp( e.fieldName(), "clean") == 0 ) {
                valid = true;
                string dropNs = us + '.' + e.valuestr();
                NamespaceDetails *d = nsdetails(dropNs.c_str());
                if ( !cmdLine.quiet )
                    log() << "CMD: clean " << dropNs << endl;
                if ( d ) {
                    ok = true;
                    anObjBuilder.append("ns", dropNs.c_str());
                    clean(dropNs.c_str(), d);
                }
                else {
                    anObjBuilder.append("errmsg", "ns not found");
                }
            }
            else if ( strcmp( e.fieldName(), "validate") == 0 ) {
                valid = true;
                string toValidateNs = us + '.' + e.valuestr();
                NamespaceDetails *d = nsdetails(toValidateNs.c_str());
                if ( !cmdLine.quiet )
                    log() << "CMD: validate " << toValidateNs << endl;
                if ( d ) {
                    ok = true;
                    anObjBuilder.append("ns", toValidateNs.c_str());
                    string s = validateNS(toValidateNs.c_str(), d, &jsobj);
                    anObjBuilder.append("result", s.c_str());
                }
                else {
                    anObjBuilder.append("errmsg", "ns not found");
                }
            }
        }

        if ( !valid ){
            anObjBuilder.append("errmsg", "no such cmd");
            anObjBuilder.append("bad cmd" , _cmdobj );
        }
        anObjBuilder.append("ok", ok?1.0:0.0);
        BSONObj x = anObjBuilder.done();
        b.append((void*) x.objdata(), x.objsize());
        return true;
    }

} // namespace mongo
