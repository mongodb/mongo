// dbcommands_admin.cpp

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

/**
   this file has dbcommands that are for dba type administration
   mostly around dbs and collections
   NOT system stuff
*/


#include "pch.h"
#include "jsobj.h"
#include "pdfile.h"
#include "namespace.h"
#include "commands.h"
#include "cmdline.h"
#include "btree.h"
#include "curop.h"
#include "../util/background.h"
#include "../scripting/engine.h"

namespace mongo {

    class CleanCmd : public Command {
    public:
        CleanCmd() : Command( "clean" ){}

        virtual bool slaveOk() const { return true; }
        virtual LockType locktype() const { return WRITE; } 
        
        virtual void help(stringstream& h) const { h << "internal"; }

        bool run(const string& dbname, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl ){
            string dropns = dbname + "." + cmdObj.firstElement().valuestrsafe();
            
            if ( !cmdLine.quiet )
                tlog() << "CMD: clean " << dropns << endl;
            
            NamespaceDetails *d = nsdetails(dropns.c_str());
            
            if ( ! d ){
                errmsg = "ns not found";
                return 0;
            }

            for ( int i = 0; i < Buckets; i++ )
                d->deletedList[i].Null();

            result.append("ns", dropns.c_str());
            return 1;
        }
        
    } cleanCmd;
    
    class ValidateCmd : public Command {
    public:
        ValidateCmd() : Command( "validate" ){}

        virtual bool slaveOk() const {
            return true;
        }
        
        virtual void help(stringstream& h) const { h << "Validate contents of a namespace by scanning its data structures for correctness.  Slow."; }

        virtual LockType locktype() const { return READ; } 
        //{ validate: "collectionnamewithoutthedbpart" [, scandata: <bool>] } */
        
        bool run(const string& dbname , BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl ){
            string ns = dbname + "." + cmdObj.firstElement().valuestrsafe();
            NamespaceDetails * d = nsdetails( ns.c_str() );
            if ( !cmdLine.quiet )
                tlog() << "CMD: validate " << ns << endl;

            if ( ! d ){
                errmsg = "ns not found";
                return 0;
            }
            
            result.append( "ns", ns );
            result.append( "result" , validateNS( ns.c_str() , d, &cmdObj ) );
            return 1;
        }
                    
        
        string validateNS(const char *ns, NamespaceDetails *d, BSONObj *cmdObj) {
            bool scanData = true;
            if( cmdObj && cmdObj->hasElement("scandata") && !cmdObj->getBoolField("scandata") )
                scanData = false;
            bool valid = true;
            stringstream ss;
            ss << "\nvalidate\n";
            //ss << "  details: " << hex << d << " ofs:" << nsindex(ns)->detailsOffset(d) << dec << endl;
            if ( d->capped )
                ss << "  capped:" << d->capped << " max:" << d->max << '\n';
            
            ss << "  firstExtent:" << d->firstExtent.toString() << " ns:" << d->firstExtent.ext()->nsDiagnostic.toString()<< '\n';
            ss << "  lastExtent:" << d->lastExtent.toString()    << " ns:" << d->lastExtent.ext()->nsDiagnostic.toString() << '\n';
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
                    killCurrentOp.checkForInterrupt();
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
                    shared_ptr<Cursor> c = theDataFileMgr.findAll(ns);
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
                    if ( d->capped && !d->capLooped() ) {
                        ss << "  capped outOfOrder:" << outOfOrder;
                        if ( outOfOrder > 1 ) {
                            valid = false;
                            ss << " ???";
                        }
                        else ss << " (OK)";
                        ss << '\n';
                    }
                    ss << "  " << n << " objects found, nobj:" << d->nrecords << '\n';
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
                                if( d->capped && !loc.isValid() && i == 1 ) { 
                                    /* the constructor for NamespaceDetails intentionally sets deletedList[1] to invalid
                                       see comments in namespace.h
                                    */
                                    break;
                                }

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
                            killCurrentOp.checkForInterrupt();
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
                    NamespaceDetails::IndexIterator i = d->ii();
                    while( i.more() ) {
                        IndexDetails& id = i.next();
                        ss << "    " << id.indexNamespace() << " keys:" <<
                            id.head.btree()->fullValidate(id.head, id.keyPattern()) << endl;
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
    } validateCmd;

    extern bool unlockRequested;
    extern unsigned lockedForWriting;
    extern mongo::mutex lockedForWritingMutex;

/*
    class UnlockCommand : public Command { 
    public:
        UnlockCommand() : Command( "unlock" ) { }
        virtual bool readOnly() { return true; }
        virtual bool slaveOk() const { return true; }
        virtual bool adminOnly() const { return true; }
        virtual bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            if( lockedForWriting ) { 
				log() << "command: unlock requested" << endl;
                errmsg = "unlock requested";
                unlockRequested = true;
            }
            else { 
                errmsg = "not locked, so cannot unlock";
                return 0;
            }
            return 1;
        }
        
    } unlockCommand;
*/
    /* see unlockFsync() for unlocking:
       db.$cmd.sys.unlock.findOne()
    */
    class FSyncCommand : public Command {
        class LockDBJob : public BackgroundJob { 
        protected:
            string name() { return "lockdbjob"; }
            void run() { 
                Client::initThread("fsyncjob");
                Client& c = cc();
                {
                    scoped_lock lk(lockedForWritingMutex);
                    lockedForWriting++;
                }
                readlock lk("");
                MemoryMappedFile::flushAll(true);
                log() << "db is now locked for snapshotting, no writes allowed. use db.$cmd.sys.unlock.findOne() to unlock" << endl;
                _ready = true;
                while( 1 ) { 
                    if( unlockRequested ) { 
                        unlockRequested = false;
                        break;
                    }
                    sleepmillis(20);
                }
                {
                    scoped_lock lk(lockedForWritingMutex);
                    lockedForWriting--;
                }
                c.shutdown();
            }
        public:
            bool& _ready;
            LockDBJob(bool& ready) : _ready(ready) {
                deleteSelf = true;
                _ready = false;
            }
        };
    public:
        FSyncCommand() : Command( "fsync" ){}
        virtual LockType locktype() const { return WRITE; } 
        virtual bool slaveOk() const { return true; }
        virtual bool adminOnly() const { return true; }
        /*virtual bool localHostOnlyIfNoAuth(const BSONObj& cmdObj) { 
            string x = cmdObj["exec"].valuestrsafe();
            return !x.empty();
        }*/
        virtual void help(stringstream& h) const { h << "http://www.mongodb.org/display/DOCS/fsync+Command"; }
        virtual bool run(const string& dbname, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            /* async means do an fsync, but return immediately */
            bool sync = ! cmdObj["async"].trueValue();
            bool lock = cmdObj["lock"].trueValue();
            log() << "CMD fsync:  sync:" << sync << " lock:" << lock << endl;

            if( lock ) { 
                uassert(12034, "fsync: can't lock while an unlock is pending", !unlockRequested);
                uassert(12032, "fsync: sync option must be true when using lock", sync);
                /* With releaseEarly(), we must be extremely careful we don't do anything 
                   where we would have assumed we were locked.  profiling is one of those things. 
                   Perhaps at profile time we could check if we released early -- however, 
                   we need to be careful to keep that code very fast it's a very common code path when on.
                */
                uassert(12033, "fsync: profiling must be off to enter locked mode", cc().database()->profile == 0);
                bool ready = false;
                LockDBJob *l = new LockDBJob(ready);
                dbMutex.releaseEarly();
                l->go();
                // don't return until background thread has acquired the write lock
                while( !ready ) { 
                    sleepmillis(10);
                }
                result.append("info", "now locked against writes, use db.$cmd.sys.unlock.findOne() to unlock");
            }
            else {
                result.append( "numFiles" , MemoryMappedFile::flushAll( sync ) );
            }
            return 1;
        }
        
    } fsyncCmd;
    


}

