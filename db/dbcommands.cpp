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

namespace mongo {

    extern bool quiet;
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
        CmdShutdown() : Command("shutdown") {}
        bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            if( noauth ) {
                // if running without auth, you must be on localhost
                AuthenticationInfo *ai = authInfo.get();
                if( ai == 0 || !ai->isLocalHost ) {
                    errmsg = "unauthorized [2]";
                    return false;
                }
            }
            log() << "terminating, shutdown command received" << endl;
            dbexit(EXIT_SUCCESS);
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

    class CmdDropDatabase : public Command {
    public:
        virtual bool logTheOp() {
            return true;
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
            if ( !quiet )
                log() << "CMD: opLogging set to " << opLogging << endl;
            return true;
        }
    } cmdoplogging;

    bool deleteIndexes( NamespaceDetails *d, const char *ns, const char *name, string &errmsg, BSONObjBuilder &anObjBuilder ) {
        
        d->aboutToDeleteAnIndex();
        
        /* there may be pointers pointing at keys in the btree(s).  kill them. */
        ClientCursor::invalidate(ns);
        
        // delete a specific index or all?
        if ( *name == '*' && name[1] == 0 ) {
            log() << "  d->nIndexes was " << d->nIndexes << '\n';
            anObjBuilder.append("nIndexesWas", (double)d->nIndexes);
            anObjBuilder.append("msg", "all indexes deleted for collection");
            if( d->nIndexes ) { 
                for ( int i = 0; i < d->nIndexes; i++ )
                    d->indexes[i].kill();
                d->nIndexes = 0;
            }
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
                d->indexes[x].kill();
                
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
            if ( !quiet )
                log() << "CMD: drop " << nsToDrop << endl;
            if ( d == 0 ) {
                errmsg = "ns not found";
                return false;
            }
            if ( d->nIndexes != 0 ) {
                assert( deleteIndexes(d, nsToDrop.c_str(), "*", errmsg, result) );
                assert( d->nIndexes == 0 );
            }
            result.append("ns", nsToDrop.c_str());
            ClientCursor::invalidate(nsToDrop.c_str());
            dropNS(nsToDrop);
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
            return false;
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
            if ( n < 0 ) {
                ok = false;
                nn = 0;
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
            return true;
        }
        virtual bool slaveOk() {
            return false;
        }
        virtual bool adminOnly() {
            return false;
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
        CmdDeleteIndexes() : Command("deleteIndexes") { }
        bool run(const char *ns, BSONObj& jsobj, string& errmsg, BSONObjBuilder& anObjBuilder, bool /*fromRepl*/) {
            /* note: temp implementation.  space not reclaimed! */
            BSONElement e = jsobj.findElement(name.c_str());
            string toDeleteNs = database->name + '.' + e.valuestr();
            NamespaceDetails *d = nsdetails(toDeleteNs.c_str());
            if ( !quiet )
                log() << "CMD: deleteIndexes " << toDeleteNs << endl;
            if ( d ) {
                BSONElement f = jsobj.findElement("index");
                if ( f.type() == String ) {
                    return deleteIndexes( d, toDeleteNs.c_str(), f.valuestr(), errmsg, anObjBuilder );
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
            for ( vector< string >::iterator i = dbNames.begin(); i != dbNames.end(); ++i ) {
                BSONObjBuilder b;
                b.append( "name", i->c_str() );
                b.append( "sizeOnDisk", (double) dbSize( i->c_str() ) );
                dbInfos.push_back( b.obj() );

                seen.insert( i->c_str() );
            }
            
            for ( map<string,Database*>::iterator i = databases.begin(); i != databases.end(); i++ ){
                string name = i->first;
                name = name.substr( 0 , name.find( ":" ) );

                if ( seen.count( name ) )
                    continue;
                
                dbInfos.push_back( BSON( "name" << name << "sizeOnDisk" << double( 1 ) ) );
            }

            result.append( "databases", dbInfos );
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
    
    bool indexWorks( const BSONObj &idxPattern, const BSONObj &sampleKey, int direction, int firstSignificantField ) {
        BSONObjIterator p( idxPattern );
        BSONObjIterator k( sampleKey );
        int i = 0;
        while( 1 ) {
            BSONElement pe = p.next();
            BSONElement ke = k.next();
            if ( pe.eoo() && ke.eoo() )
                return true;
            if ( pe.eoo() || ke.eoo() )
                return false;
            if ( strcmp( pe.fieldName(), ke.fieldName() ) != 0 )
                return false;
            if ( ( i == firstSignificantField ) && !( ( direction > 0 ) == ( pe.number() > 0 ) ) )
                return false;
            ++i;
        }
        return false;
    }

    // NOTE min, max, and keyPattern will be updated to be consistent with the selected index.
    const IndexDetails *indexDetailsForRange( const char *ns, string &errmsg, BSONObj &min, BSONObj &max, BSONObj &keyPattern ) {
        if ( ns[ 0 ] == '\0' || min.isEmpty() || max.isEmpty() ) {
            errmsg = "invalid command syntax (note: min and max are required)";
            return 0;
        }

        setClient( ns );
        const IndexDetails *id = 0;
        NamespaceDetails *d = nsdetails( ns );
        if ( !d ) {
            errmsg = "ns not found";
            return 0;
        }
        
        if ( keyPattern.isEmpty() ) {
            BSONObjIterator i( min );
            BSONObjIterator a( max );
            int direction = 0;
            int firstSignificantField = 0;
            while( 1 ) {
                BSONElement ie = i.next();
                BSONElement ae = a.next();
                if ( ie.eoo() && ae.eoo() )
                    break;
                if ( ie.eoo() || ae.eoo() || strcmp( ie.fieldName(), ae.fieldName() ) != 0 ) {
                    errmsg = "min and max keys do not share pattern";
                    return 0;
                }
                int cmp = ie.woCompare( ae );
                if ( cmp < 0 )
                    direction = 1;
                if ( cmp > 0 )
                    direction = -1;
                if ( direction != 0 )
                    break;
                ++firstSignificantField;
            }
            for (int i = 0; i < d->nIndexes; i++ ) {
                IndexDetails& ii = d->indexes[i];
                if ( indexWorks( ii.keyPattern(), min, direction, firstSignificantField ) ) {
                    id = &ii;
                    keyPattern = ii.keyPattern();
                    break;
                }
            }
            
        } else {            
            for (int i = 0; i < d->nIndexes; i++ ) {
                IndexDetails& ii = d->indexes[i];
                if( ii.keyPattern().woCompare(keyPattern) == 0 ) {
                    id = &ii;
                    break;
                }
            }
        }

        if ( !id ) {
            errmsg = "no index found for specified keyPattern";
            return 0;
        }
        
        min = min.extractFieldsUnDotted( keyPattern );
        max = max.extractFieldsUnDotted( keyPattern );

        return id;
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
            
            const IndexDetails *id = indexDetailsForRange( ns, errmsg, min, max, keyPattern );
            if ( id == 0 )
                return false;

            Timer t;
            int num = 0;
            for( BtreeCursor c( *id, min, max, 1 ); c.ok(); c.advance(), ++num );
            num /= 2;
            BtreeCursor c( *id, min, max, 1 );
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
            help << " example: { medianKey:\"blog.posts\", keyPattern:{x:1}, min:{x:10}, max:{x:55} }\n"
                "NOTE: This command may take awhile to run";
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
                const IndexDetails *id = indexDetailsForRange( ns, errmsg, min, max, keyPattern );
                if ( id == 0 )
                    return false;
                c.reset( new BtreeCursor( *id, min, max, 1 ) );
            }
            
            Timer t;
            long long size = 0;
            while( c->ok() ) {
                size += c->current().objsize();
                c->advance();
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
            return true;
        }
    } cmdDatasize;

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
                if ( !quiet )
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
                if ( !quiet )
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

        if ( !valid )
            anObjBuilder.append("errmsg", "no such cmd");
        anObjBuilder.append("ok", ok?1.0:0.0);
        BSONObj x = anObjBuilder.done();
        b.append((void*) x.objdata(), x.objsize());
        return true;
    }

} // namespace mongo
