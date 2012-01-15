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
#include "namespace-inl.h"
#include "commands.h"
#include "cmdline.h"
#include "btree.h"
#include "curop-inl.h"
#include "../util/background.h"
#include "../util/logfile.h"
#include "../util/alignedbuilder.h"
#include "../util/paths.h"
#include "../scripting/engine.h"
#include "../util/timer.h"

#include <boost/filesystem/convenience.hpp>
#include <boost/filesystem/operations.hpp>

namespace mongo {

    class CleanCmd : public Command {
    public:
        CleanCmd() : Command( "clean" ) {}

        virtual bool slaveOk() const { return true; }
        virtual LockType locktype() const { return WRITE; }

        virtual void help(stringstream& h) const { h << "internal"; }

        bool run(const string& dbname, BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl ) {
            string dropns = dbname + "." + cmdObj.firstElement().valuestrsafe();

            if ( !cmdLine.quiet )
                tlog() << "CMD: clean " << dropns << endl;

            NamespaceDetails *d = nsdetails(dropns.c_str());

            if ( ! d ) {
                errmsg = "ns not found";
                return 0;
            }

            for ( int i = 0; i < Buckets; i++ )
                d->deletedList[i].Null();

            result.append("ns", dropns.c_str());
            return 1;
        }

    } cleanCmd;

    namespace dur {
        boost::filesystem::path getJournalDir();
    }
 
    class JournalLatencyTestCmd : public Command {
    public:
        JournalLatencyTestCmd() : Command( "journalLatencyTest" ) {}

        virtual bool slaveOk() const { return true; }
        virtual LockType locktype() const { return NONE; }
        virtual bool adminOnly() const { return true; }
        virtual void help(stringstream& h) const { h << "test how long to write and fsync to a test file in the journal/ directory"; }

        bool run(const string& dbname, BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl ) {
            boost::filesystem::path p = dur::getJournalDir();
            p /= "journalLatencyTest";
        
            // remove file if already present
            try { 
                boost::filesystem::remove(p);
            }
            catch(...) { }

            BSONObjBuilder bb[2];
            for( int pass = 0; pass < 2; pass++ ) {
                LogFile f(p.string());
                AlignedBuilder b(1024 * 1024);
                {
                    Timer t;
                    for( int i = 0 ; i < 100; i++ ) { 
                        f.synchronousAppend(b.buf(), 8192);
                    }
                    bb[pass].append("8KB", t.millis() / 100.0);
                }
                {
                    const int N = 50;
                    Timer t2;
                    long long x = 0;
                    for( int i = 0 ; i < N; i++ ) { 
                        Timer t;
                        f.synchronousAppend(b.buf(), 8192);
                        x += t.micros();
                        sleepmillis(4);
                    }
                    long long y = t2.micros() - 4*N*1000;
                    // not really trusting the timer granularity on all platforms so whichever is higher of x and y
                    bb[pass].append("8KBWithPauses", max(x,y) / (N*1000.0));
                }
                {
                    Timer t;
                    for( int i = 0 ; i < 20; i++ ) { 
                        f.synchronousAppend(b.buf(), 1024 * 1024);
                    }
                    bb[pass].append("1MB", t.millis() / 20.0);
                }
                // second time around, we are prealloced.
            }
            result.append("timeMillis", bb[0].obj());
            result.append("timeMillisWithPrealloc", bb[1].obj());

            try { 
                remove(p);
            }
            catch(...) { }

            try {
                result.append("onSamePartition", onSamePartition(dur::getJournalDir().string(), dbpath));
            }
            catch(...) { }

            return 1;
        }
    } journalLatencyTestCmd;

    class ValidateCmd : public Command {
    public:
        ValidateCmd() : Command( "validate" ) {}

        virtual bool slaveOk() const {
            return true;
        }

        virtual void help(stringstream& h) const { h << "Validate contents of a namespace by scanning its data structures for correctness.  Slow.\n"
                                                        "Add full:true option to do a more thorough check"; }

        virtual LockType locktype() const { return READ; }
        //{ validate: "collectionnamewithoutthedbpart" [, scandata: <bool>] [, full: <bool> } */

        bool run(const string& dbname , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl ) {
            string ns = dbname + "." + cmdObj.firstElement().valuestrsafe();
            NamespaceDetails * d = nsdetails( ns.c_str() );
            if ( !cmdLine.quiet )
                tlog() << "CMD: validate " << ns << endl;

            if ( ! d ) {
                errmsg = "ns not found";
                return 0;
            }

            result.append( "ns", ns );
            validateNS( ns.c_str() , d, cmdObj, result);
            return 1;
        }

    private:
        void validateNS(const char *ns, NamespaceDetails *d, const BSONObj& cmdObj, BSONObjBuilder& result) {
            const bool full = cmdObj["full"].trueValue();
            const bool scanData = full || cmdObj["scandata"].trueValue();

            bool valid = true;
            BSONArrayBuilder errors; // explanation(s) for why valid = false
            if ( d->capped ){
                result.append("capped", d->capped);
                result.append("max", d->max);
            }

            result.append("firstExtent", str::stream() << d->firstExtent.toString() << " ns:" << d->firstExtent.ext()->nsDiagnostic.toString());
            result.append( "lastExtent", str::stream() <<  d->lastExtent.toString() << " ns:" <<  d->lastExtent.ext()->nsDiagnostic.toString());
            
            BSONArrayBuilder extentData;

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
                    if ( full )
                        extentData << e->dump();
                    
                    killCurrentOp.checkForInterrupt();
                }
                result.append("extentCount", ne);
            }
            catch (...) {
                valid=false;
                errors << "extent asserted";
            }

            if ( full )
                result.appendArray( "extents" , extentData.arr() );

            
            result.appendNumber("datasize", d->stats.datasize);
            result.appendNumber("nrecords", d->stats.nrecords);
            result.appendNumber("lastExtentSize", d->lastExtentSize);
            result.appendNumber("padding", d->paddingFactor);
            

            try {

                try {
                    result.append("firstExtentDetails", d->firstExtent.ext()->dump());

                    valid = valid && d->firstExtent.ext()->validates() && 
                        d->firstExtent.ext()->xprev.isNull();
                }
                catch (...) {
                    errors << "exception firstextent";
                    valid = false;
                }

                set<DiskLoc> recs;
                if( scanData ) {
                    shared_ptr<Cursor> c = theDataFileMgr.findAll(ns);
                    int n = 0;
                    int nInvalid = 0;
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

                        if (full){
                            BSONObj obj(r);
                            if (!obj.isValid() || !obj.valid()){ // both fast and deep checks
                                valid = false;
                                if (nInvalid == 0) // only log once;
                                    errors << "invalid bson object detected (see logs for more info)";

                                nInvalid++;
                                if (strcmp("_id", obj.firstElementFieldName()) == 0){
                                    try {
                                        obj.firstElement().validate(); // throws on error
                                        log() << "Invalid bson detected in " << ns << " with _id: " << obj.firstElement().toString(false) << endl;
                                    }
                                    catch(...){
                                        log() << "Invalid bson detected in " << ns << " with corrupt _id" << endl;
                                    }
                                }
                                else {
                                    log() << "Invalid bson detected in " << ns << " and couldn't find _id" << endl;
                                }
                            }
                        }

                        c->advance();
                    }
                    if ( d->capped && !d->capLooped() ) {
                        result.append("cappedOutOfOrder", outOfOrder);
                        if ( outOfOrder > 1 ) {
                            valid = false;
                            errors << "too many out of order records";
                        }
                    }
                    result.append("objectsFound", n);

                    if (full) {
                        result.append("invalidObjects", nInvalid);
                    }

                    result.appendNumber("bytesWithHeaders", len);
                    result.appendNumber("bytesWithoutHeaders", nlen);
                }

                BSONArrayBuilder deletedListArray;
                for ( int i = 0; i < Buckets; i++ ) {
                    deletedListArray << d->deletedList[i].isNull();
                }

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
                                    string err (str::stream() << "bad deleted loc: " << loc.toString() << " bucket:" << i << " k:" << k);
                                    errors << err;

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
                    }
                    catch (...) {
                        errors << ("exception in deleted chain for bucket " + BSONObjBuilder::numStr(i));
                        valid = false;
                    }
                }
                result.appendNumber("deletedCount", ndel);
                result.appendNumber("deletedSize", delSize);

                if ( incorrect ) {
                    errors << (BSONObjBuilder::numStr(incorrect) + " records from datafile are in deleted list");
                    valid = false;
                }

                int idxn = 0;
                try  {
                    result.append("nIndexes", d->nIndexes);
                    BSONObjBuilder indexes; // not using subObjStart to be exception safe
                    NamespaceDetails::IndexIterator i = d->ii();
                    while( i.more() ) {
                        IndexDetails& id = i.next();
                        long long keys = id.idxInterface().fullValidate(id.head, id.keyPattern());
                        indexes.appendNumber(id.indexNamespace(), keys);
                    }
                    result.append("keysPerIndex", indexes.done());
                }
                catch (...) {
                    errors << ("exception during index validate idxn " + BSONObjBuilder::numStr(idxn));
                    valid=false;
                }

            }
            catch (AssertionException) {
                errors << "exception during validate";
                valid = false;
            }

            result.appendBool("valid", valid);
            result.append("errors", errors.arr());

            if ( !full ){
                result.append("warning", "Some checks omitted for speed. use {full:true} option to do more thorough scan.");
            }
            
            if ( !valid ) {
                result.append("advice", "ns corrupt, requires repair");
            }

        }
    } validateCmd;

    bool lockedForWriting = false; // read from db/instance.cpp
    static bool unlockRequested = false;
    static mongo::mutex fsyncLockMutex("fsyncLock");
    static boost::condition fsyncLockCondition;
    static OID fsyncLockID; // identifies the current lock job

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
        static const char* url() { return  "http://www.mongodb.org/display/DOCS/fsync+Command"; }
        class LockDBJob : public BackgroundJob {
        protected:
            virtual string name() const { return "lockdbjob"; }
            void run() {
                Client::initThread("fsyncjob");
                Client& c = cc();
                {
                    scoped_lock lk(fsyncLockMutex);
                    while (lockedForWriting){ // there is a small window for two LockDBJob's to be active. This prevents it.
                        fsyncLockCondition.wait(lk.boost());
                    }
                    lockedForWriting = true;
                    fsyncLockID.init();
                }
                readlock lk("");
                MemoryMappedFile::flushAll(true);
                log() << "db is now locked for snapshotting, no writes allowed. db.fsyncUnlock() to unlock" << endl;
                log() << "    For more info see " << FSyncCommand::url() << endl;
                _ready = true;
                {
                    scoped_lock lk(fsyncLockMutex);
                    while( !unlockRequested ) {
                        fsyncLockCondition.wait(lk.boost());
                    }
                    unlockRequested = false;
                    lockedForWriting = false;
                    fsyncLockCondition.notify_all();
                }
                c.shutdown();
            }
        public:
            bool& _ready;
            LockDBJob(bool& ready) : BackgroundJob( true /* delete self */ ), _ready(ready) {
                _ready = false;
            }
        };
    public:
        FSyncCommand() : Command( "fsync" ) {}
        virtual LockType locktype() const { return WRITE; }
        virtual bool slaveOk() const { return true; }
        virtual bool adminOnly() const { return true; }
        /*virtual bool localHostOnlyIfNoAuth(const BSONObj& cmdObj) {
            string x = cmdObj["exec"].valuestrsafe();
            return !x.empty();
        }*/
        virtual void help(stringstream& h) const { h << url(); }
        virtual bool run(const string& dbname, BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            bool sync = !cmdObj["async"].trueValue(); // async means do an fsync, but return immediately
            bool lock = cmdObj["lock"].trueValue();
            log() << "CMD fsync:  sync:" << sync << " lock:" << lock << endl;

            if( lock ) {
                // fsync and lock variation 

                uassert(12034, "fsync: can't lock while an unlock is pending", !unlockRequested);
                uassert(12032, "fsync: sync option must be true when using lock", sync);
                /* With releaseEarly(), we must be extremely careful we don't do anything
                   where we would have assumed we were locked.  profiling is one of those things.
                   Perhaps at profile time we could check if we released early -- however,
                   we need to be careful to keep that code very fast it's a very common code path when on.
                */
                uassert(12033, "fsync: profiling must be off to enter locked mode", cc().database()->profile == 0);

                // todo future: Perhaps we could do this in the background thread.  As is now, writes may interleave between 
                //              the releaseEarly below and the acquisition of the readlock in the background thread. 
                //              However the real problem is that it seems complex to unlock here and then have a window for 
                //              writes before the bg job -- can be done correctly but harder to reason about correctness.
                //              If this command ran within a read lock in the first place, would it work, and then that 
                //              would be quite easy?
                //              Or, could we downgrade the write lock to a read lock, wait for ready, then release?
                getDur().syncDataAndTruncateJournal();

                bool ready = false;
                LockDBJob *l = new LockDBJob(ready);

                d.dbMutex.releaseEarly();
                
                // There is a narrow window for another lock request to come in
                // here before the LockDBJob grabs the readlock. LockDBJob will
                // ensure that the requests are serialized and never running
                // concurrently

                l->go();
                // don't return until background thread has acquired the read lock
                while( !ready ) {
                    sleepmillis(10);
                }
                result.append("info", "now locked against writes, use db.fsyncUnlock() to unlock");
                result.append("seeAlso", url());
            }
            else {
                // the simple fsync command case

                if (sync)
                    getDur().commitNow();
                result.append( "numFiles" , MemoryMappedFile::flushAll( sync ) );
            }
            return 1;
        }

    } fsyncCmd;

    // Note that this will only unlock the current lock.  If another thread
    // relocks before we return we still consider the unlocking successful.
    // This is imporant because if two scripts are trying to fsync-lock, each
    // one must be assured that between the fsync return and the call to unlock
    // that the database is fully locked
    void unlockFsyncAndWait(){
        scoped_lock lk(fsyncLockMutex);
        if (lockedForWriting) { // could have handled another unlock before we grabbed the lock
            OID curOp = fsyncLockID;
            unlockRequested = true;
            fsyncLockCondition.notify_all();
            while (lockedForWriting && fsyncLockID == curOp){
                fsyncLockCondition.wait( lk.boost() );
            }
        }
    }
}

