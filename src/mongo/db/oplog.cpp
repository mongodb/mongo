// @file oplog.cpp

/**
*    Copyright (C) 2008 10gen Inc.
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

#include "pch.h"
#include "oplog.h"
#include "repl_block.h"
#include "repl.h"
#include "commands.h"
#include "repl/rs.h"
#include "stats/counters.h"
#include "../util/file.h"
#include "../util/startup_test.h"
#include "queryoptimizer.h"
#include "ops/update.h"
#include "ops/delete.h"
#include "mongo/db/instance.h"

namespace mongo {

    // from d_migrate.cpp
    void logOpForSharding( const char * opstr , const char * ns , const BSONObj& obj , BSONObj * patt );

    int __findingStartInitialTimeout = 5; // configurable for testing

    // cached copies of these...so don't rename them, drop them, etc.!!!
    static NamespaceDetails *localOplogMainDetails = 0;
    static Database *localDB = 0;
    static NamespaceDetails *rsOplogDetails = 0;
    void oplogCheckCloseDatabase( Database * db ) {
        verify( Lock::isW() );
        localDB = 0;
        localOplogMainDetails = 0;
        rsOplogDetails = 0;
        resetSlaveCache();
    }

    static void _logOpUninitialized(const char *opstr, const char *ns, const char *logNS, const BSONObj& obj, BSONObj *o2, bool *bb, bool fromMigrate ) {
        uassert(13288, "replSet error write op to db before replSet initialized", str::startsWith(ns, "local.") || *opstr == 'n');
    }

    /** write an op to the oplog that is already built.
        todo : make _logOpRS() call this so we don't repeat ourself?
        */
    void _logOpObjRS(const BSONObj& op) {
        Lock::DBWrite lk("local");

        const OpTime ts = op["ts"]._opTime();
        long long h = op["h"].numberLong();

        {
            const char *logns = rsoplog;
            if ( rsOplogDetails == 0 ) {
                Client::Context ctx( logns , dbpath, false);
                localDB = ctx.db();
                verify( localDB );
                rsOplogDetails = nsdetails(logns);
                massert(13389, "local.oplog.rs missing. did you drop it? if so restart server", rsOplogDetails);
            }
            Client::Context ctx( logns , localDB, false );
            {
                int len = op.objsize();
                Record *r = theDataFileMgr.fast_oplog_insert(rsOplogDetails, logns, len);
                memcpy(getDur().writingPtr(r->data(), len), op.objdata(), len);
            }
            /* todo: now() has code to handle clock skew.  but if the skew server to server is large it will get unhappy.
                     this code (or code in now() maybe) should be improved.
                     */
            if( theReplSet ) {
                if( !(theReplSet->lastOpTimeWritten<ts) ) {
                    log() << "replSet error possible failover clock skew issue? " << theReplSet->lastOpTimeWritten.toString() << ' ' << endl;
                }
                theReplSet->lastOpTimeWritten = ts;
                theReplSet->lastH = h;
                ctx.getClient()->setLastOp( ts );
            }
        }
    }

    /** given a BSON object, create a new one at dst which is the existing (partial) object
        with a new object element appended at the end with fieldname "o".

        @param partial already build object with everything except the o member.  e.g. something like:
               { ts:..., ns:..., os2:... }
        @param o a bson object to be added with fieldname "o"
        @dst   where to put the newly built combined object.  e.g. ends up as something like:
               { ts:..., ns:..., os2:..., o:... }
    */
    void append_O_Obj(char *dst, const BSONObj& partial, const BSONObj& o) {
        const int size1 = partial.objsize() - 1;  // less the EOO char
        const int oOfs = size1+3;                 // 3 = byte BSONOBJTYPE + byte 'o' + byte \0

        void *p = getDur().writingPtr(dst, oOfs+o.objsize()+1);

        memcpy(p, partial.objdata(), size1);

        // adjust overall bson object size for the o: field
        little<unsigned>::ref( p ) += o.objsize() + 1/*fieldtype byte*/ + 2/*"o" fieldname*/;

        char *b = static_cast<char *>(p);
        b += size1;
        *b++ = (char) Object;
        *b++ = 'o'; // { o : ... }
        *b++ = 0;   // null terminate "o" fieldname
        memcpy(b, o.objdata(), o.objsize());
        b += o.objsize();
        *b = EOO;
    }

    // global is safe as we are in write lock. we put the static outside the function to avoid the implicit mutex 
    // the compiler would use if inside the function.  the reason this is static is to avoid a malloc/free for this
    // on every logop call.
    static BufBuilder logopbufbuilder(8*1024);
    static void _logOpRS(const char *opstr, const char *ns, const char *logNS, const BSONObj& obj, BSONObj *o2, bool *bb, bool fromMigrate ) {
        Lock::DBWrite lk1("local");

        if ( strncmp(ns, "local.", 6) == 0 ) {
            if ( strncmp(ns, "local.slaves", 12) == 0 )
                resetSlaveCache();
            return;
        }

        mutex::scoped_lock lk2(OpTime::m);

        const OpTime ts = OpTime::now(lk2);
        long long hashNew;
        if( theReplSet ) {
            massert(13312, "replSet error : logOp() but not primary?", theReplSet->box.getState().primary());
            hashNew = (theReplSet->lastH * 131 + ts.asLL()) * 17 + theReplSet->selfId();
        }
        else {
            // must be initiation
            verify( *ns == 0 );
            hashNew = 0;
        }

        /* we jump through a bunch of hoops here to avoid copying the obj buffer twice --
           instead we do a single copy to the destination position in the memory mapped file.
        */

        logopbufbuilder.reset();
        BSONObjBuilder b(logopbufbuilder);
        b.appendTimestamp("ts", ts.asDate());
        b.append("h", hashNew);
        b.append("op", opstr);
        b.append("ns", ns);
        if (fromMigrate) 
            b.appendBool("fromMigrate", true);
        if ( bb )
            b.appendBool("b", *bb);
        if ( o2 )
            b.append("o2", *o2);
        BSONObj partial = b.done();
        int posz = partial.objsize();
        int len = posz + obj.objsize() + 1 + 2 /*o:*/;

        Record *r;
        DEV verify( logNS == 0 );
        {
            const char *logns = rsoplog;
            if ( rsOplogDetails == 0 ) {
                Client::Context ctx( logns , dbpath, false);
                localDB = ctx.db();
                verify( localDB );
                rsOplogDetails = nsdetails(logns);
                massert(13347, "local.oplog.rs missing. did you drop it? if so restart server", rsOplogDetails);
            }
            Client::Context ctx( logns , localDB, false );
            r = theDataFileMgr.fast_oplog_insert(rsOplogDetails, logns, len);
            /* todo: now() has code to handle clock skew.  but if the skew server to server is large it will get unhappy.
                     this code (or code in now() maybe) should be improved.
                     */
            if( theReplSet ) {
                if( !(theReplSet->lastOpTimeWritten<ts) ) {
                    log() << "replSet ERROR possible failover clock skew issue? " << theReplSet->lastOpTimeWritten << ' ' << ts << rsLog;
                    log() << "replSet " << theReplSet->isPrimary() << rsLog;
                }
                theReplSet->lastOpTimeWritten = ts;
                theReplSet->lastH = hashNew;
                ctx.getClient()->setLastOp( ts );
            }
        }

        append_O_Obj(r->data(), partial, obj);

        if ( logLevel >= 6 ) {
            BSONObj temp(r);
            log( 6 ) << "logOp:" << temp << endl;
        }
    }

    /* we write to local.oplog.$main:
         { ts : ..., op: ..., ns: ..., o: ... }
       ts: an OpTime timestamp
       op:
        "i" insert
        "u" update
        "d" delete
        "c" db cmd
        "db" declares presence of a database (ns is set to the db name + '.')
        "n" no op
       logNS: where to log it.  0/null means "local.oplog.$main".
       bb:
         if not null, specifies a boolean to pass along to the other side as b: param.
         used for "justOne" or "upsert" flags on 'd', 'u'
       first: true
         when set, indicates this is the first thing we have logged for this database.
         thus, the slave does not need to copy down all the data when it sees this.

       note this is used for single collection logging even when --replSet is enabled.
    */
    static void _logOpOld(const char *opstr, const char *ns, const char *logNS, const BSONObj& obj, BSONObj *o2, bool *bb, bool fromMigrate ) {
        Lock::DBWrite lk("local");
        static BufBuilder bufbuilder(8*1024); // todo there is likely a mutex on this constructor

        if ( strncmp(ns, "local.", 6) == 0 ) {
            if ( strncmp(ns, "local.slaves", 12) == 0 ) {
                resetSlaveCache();
            }
            return;
        }

        mutex::scoped_lock lk2(OpTime::m);

        const OpTime ts = OpTime::now(lk2);
        Client::Context context("",0,false);

        /* we jump through a bunch of hoops here to avoid copying the obj buffer twice --
           instead we do a single copy to the destination position in the memory mapped file.
        */

        bufbuilder.reset();
        BSONObjBuilder b(bufbuilder);
        b.appendTimestamp("ts", ts.asDate());
        b.append("op", opstr);
        b.append("ns", ns);
        if (fromMigrate) 
            b.appendBool("fromMigrate", true);
        if ( bb )
            b.appendBool("b", *bb);
        if ( o2 )
            b.append("o2", *o2);
        BSONObj partial = b.done(); // partial is everything except the o:... part.

        int po_sz = partial.objsize();
        int len = po_sz + obj.objsize() + 1 + 2 /*o:*/;

        Record *r;
        if( logNS == 0 ) {
            logNS = "local.oplog.$main";
            if ( localOplogMainDetails == 0 ) {
                Client::Context ctx( logNS , dbpath, false);
                localDB = ctx.db();
                verify( localDB );
                localOplogMainDetails = nsdetails(logNS);
                verify( localOplogMainDetails );
            }
            Client::Context ctx( logNS , localDB, false );
            r = theDataFileMgr.fast_oplog_insert(localOplogMainDetails, logNS, len);
        }
        else {
            Client::Context ctx( logNS, dbpath, false );
            verify( nsdetails( logNS ) );
            // first we allocate the space, then we fill it below.
            r = theDataFileMgr.fast_oplog_insert( nsdetails( logNS ), logNS, len);
        }

        append_O_Obj(r->data(), partial, obj);

        context.getClient()->setLastOp( ts );

        if ( logLevel >= 6 ) {
            BSONObj temp(r);
            log( 6 ) << "logging op:" << temp << endl;
        }
    }

    static void (*_logOp)(const char *opstr, const char *ns, const char *logNS, const BSONObj& obj, BSONObj *o2, bool *bb, bool fromMigrate ) = _logOpOld;
    void newReplUp() {
        replSettings.master = true;
        _logOp = _logOpRS;
    }
    void newRepl() {
        replSettings.master = true;
        _logOp = _logOpUninitialized;
    }
    void oldRepl() { _logOp = _logOpOld; }

    void logKeepalive() {
        _logOp("n", "", 0, BSONObj(), 0, 0, false);
    }
    void logOpComment(const BSONObj& obj) {
        _logOp("n", "", 0, obj, 0, 0, false);
    }
    void logOpInitiate(const BSONObj& obj) {
        _logOpRS("n", "", 0, obj, 0, 0, false);
    }

    /*@ @param opstr:
          c userCreateNS
          i insert
          n no-op / keepalive
          d delete / remove
          u update
    */
    void logOp(const char *opstr, const char *ns, const BSONObj& obj, BSONObj *patt, bool *b, bool fromMigrate) {
        if ( replSettings.master ) {
            _logOp(opstr, ns, 0, obj, patt, b, fromMigrate);
        }

        logOpForSharding( opstr , ns , obj , patt );
    }

    void createOplog() {
        Lock::GlobalWrite lk;

        const char * ns = "local.oplog.$main";

        bool rs = !cmdLine._replSet.empty();
        if( rs )
            ns = rsoplog;

        Client::Context ctx(ns);

        NamespaceDetails * nsd = nsdetails( ns );

        if ( nsd ) {

            if ( cmdLine.oplogSize != 0 ) {
                int o = (int)(nsd->storageSize() / ( 1024 * 1024 ) );
                int n = (int)(cmdLine.oplogSize / ( 1024 * 1024 ) );
                if ( n != o ) {
                    stringstream ss;
                    ss << "cmdline oplogsize (" << n << ") different than existing (" << o << ") see: http://dochub.mongodb.org/core/increase-oplog";
                    log() << ss.str() << endl;
                    throw UserException( 13257 , ss.str() );
                }
            }

            if( rs ) return;

            DBDirectClient c;
            BSONObj lastOp = c.findOne( ns, Query().sort(reverseNaturalObj) );
            if ( !lastOp.isEmpty() ) {
                OpTime::setLast( lastOp[ "ts" ].date() );
            }
            return;
        }

        /* create an oplog collection, if it doesn't yet exist. */
        BSONObjBuilder b;
        double sz;
        if ( cmdLine.oplogSize != 0 )
            sz = (double)cmdLine.oplogSize;
        else {
            /* not specified. pick a default size */
            sz = 50.0 * 1000 * 1000;
            if ( sizeof(int *) >= 8 ) {
#if defined(__APPLE__)
                // typically these are desktops (dev machines), so keep it smallish
                sz = (256-64) * 1000 * 1000;
#else
                sz = 990.0 * 1000 * 1000;
                boost::intmax_t free = File::freeSpace(dbpath); //-1 if call not supported.
                double fivePct = free * 0.05;
                if ( fivePct > sz )
                    sz = fivePct;
#endif
            }
        }

        log() << "******" << endl;
        log() << "creating replication oplog of size: " << (int)( sz / ( 1024 * 1024 ) ) << "MB..." << endl;

        b.append("size", sz);
        b.appendBool("capped", 1);
        b.appendBool("autoIndexId", false);

        string err;
        BSONObj o = b.done();
        userCreateNS(ns, o, err, false);
        if( !rs )
            logOp( "n", "", BSONObj() );

        /* sync here so we don't get any surprising lag later when we try to sync */
        MemoryMappedFile::flushAll(true);
        log() << "******" << endl;
    }

    // -------------------------------------

    FindingStartCursor::FindingStartCursor( const QueryPlan & qp ) :
    _qp( qp ),
    _findingStart( true ),
    _findingStartMode()
    { init(); }
    
    void FindingStartCursor::next() {
        if ( !_findingStartCursor || !_findingStartCursor->ok() ) {
            _findingStart = false;
            _c = _qp.newCursor(); // on error, start from beginning
            destroyClientCursor();
            return;
        }
        switch( _findingStartMode ) {
            // Initial mode: scan backwards from end of collection
            case Initial: {
                if ( !_matcher->matchesCurrent( _findingStartCursor->c() ) ) {
                    _findingStart = false; // found first record out of query range, so scan normally
                    _c = _qp.newCursor( _findingStartCursor->currLoc() );
                    destroyClientCursor();
                    return;
                }
                _findingStartCursor->advance();
                RARELY {
                    if ( _findingStartTimer.seconds() >= __findingStartInitialTimeout ) {
                        // If we've scanned enough, switch to find extent mode.
                        createClientCursor( extentFirstLoc( _findingStartCursor->currLoc() ) );
                        _findingStartMode = FindExtent;
                        return;
                    }
                }
                return;
            }
            // FindExtent mode: moving backwards through extents, check first
            // document of each extent.
            case FindExtent: {
                if ( !_matcher->matchesCurrent( _findingStartCursor->c() ) ) {
                    _findingStartMode = InExtent;
                    return;
                }
                DiskLoc prev = prevExtentFirstLoc( _findingStartCursor->currLoc() );
                if ( prev.isNull() ) { // hit beginning, so start scanning from here
                    createClientCursor();
                    _findingStartMode = InExtent;
                    return;
                }
                // There might be a more efficient implementation than creating new cursor & client cursor each time,
                // not worrying about that for now
                createClientCursor( prev );
                return;
            }
            // InExtent mode: once an extent is chosen, find starting doc in the extent.
            case InExtent: {
                if ( _matcher->matchesCurrent( _findingStartCursor->c() ) ) {
                    _findingStart = false; // found first record in query range, so scan normally
                    _c = _qp.newCursor( _findingStartCursor->currLoc() );
                    destroyClientCursor();
                    return;
                }
                _findingStartCursor->advance();
                return;
            }
            default: {
                massert( 14038, "invalid _findingStartMode", false );
            }
        }
    }
    
    DiskLoc FindingStartCursor::extentFirstLoc( const DiskLoc &rec ) {
        Extent *e = rec.rec()->myExtent( rec );
        if ( !_qp.nsd()->capLooped() || ( e->myLoc != _qp.nsd()->capExtent ) )
            return e->firstRecord;
        // Likely we are on the fresh side of capExtent, so return first fresh record.
        // If we are on the stale side of capExtent, then the collection is small and it
        // doesn't matter if we start the extent scan with capFirstNewRecord.
        return _qp.nsd()->capFirstNewRecord;
    }
    
    void wassertExtentNonempty( const Extent *e ) {
        // TODO ensure this requirement is clearly enforced, or fix.
        wassert( !e->firstRecord.isNull() );
    }
    
    DiskLoc FindingStartCursor::prevExtentFirstLoc( const DiskLoc &rec ) {
        Extent *e = rec.rec()->myExtent( rec );
        if ( _qp.nsd()->capLooped() ) {
            if ( e->xprev.isNull() ) {
                e = _qp.nsd()->lastExtent.ext();
            }
            else {
                e = e->xprev.ext();
            }
            if ( e->myLoc != _qp.nsd()->capExtent ) {
                wassertExtentNonempty( e );
                return e->firstRecord;
            }
        }
        else {
            if ( !e->xprev.isNull() ) {
                e = e->xprev.ext();
                wassertExtentNonempty( e );
                return e->firstRecord;
            }
        }
        return DiskLoc(); // reached beginning of collection
    }
    
    void FindingStartCursor::createClientCursor( const DiskLoc &startLoc ) {
        shared_ptr<Cursor> c = _qp.newCursor( startLoc );
        _findingStartCursor.reset( new ClientCursor(QueryOption_NoCursorTimeout, c, _qp.ns()) );
    }

    bool FindingStartCursor::firstDocMatchesOrEmpty() const {
        shared_ptr<Cursor> c = _qp.newCursor();
        return !c->ok() || _matcher->matchesCurrent( c.get() );
    }
    
    void FindingStartCursor::init() {
        BSONElement tsElt = _qp.originalQuery()[ "ts" ];
        massert( 13044, "no ts field in query", !tsElt.eoo() );
        BSONObjBuilder b;
        b.append( tsElt );
        BSONObj tsQuery = b.obj();
        _matcher.reset(new CoveredIndexMatcher(tsQuery, _qp.indexKey()));
        if ( firstDocMatchesOrEmpty() ) {
            _c = _qp.newCursor();
            _findingStart = false;
            return;
        }
        // Use a ClientCursor here so we can release db mutex while scanning
        // oplog (can take quite a while with large oplogs).
        shared_ptr<Cursor> c = _qp.newReverseCursor();
        _findingStartCursor.reset( new ClientCursor(QueryOption_NoCursorTimeout, c, _qp.ns(), BSONObj()) );
        _findingStartTimer.reset();
        _findingStartMode = Initial;
    }
    
    shared_ptr<Cursor> FindingStartCursor::getCursor( const char *ns, const BSONObj &query, const BSONObj &order ) {
        NamespaceDetails *d = nsdetails(ns);
        if ( !d ) {
            return shared_ptr<Cursor>( new BasicCursor( DiskLoc() ) );
        }
        FieldRangeSetPair frsp( ns, query );
        QueryPlan oplogPlan( d, -1, frsp, 0, query, order );
        FindingStartCursor finder( oplogPlan );
        ElapsedTracker yieldCondition( 256, 20 );
        while( !finder.done() ) {
            if ( yieldCondition.intervalHasElapsed() ) {
                if ( finder.prepareToYield() ) {
                    ClientCursor::staticYield( -1, ns, 0 );
                    finder.recoverFromYield();
                }
            }
            finder.next();
        }
        shared_ptr<Cursor> ret = finder.cursor();
        shared_ptr<CoveredIndexMatcher> matcher( new CoveredIndexMatcher( query, BSONObj() ) );
        ret->setMatcher( matcher );
        return ret;
    }
    
    // -------------------------------------

    struct TestOpTime : public StartupTest {
        void run() {
            OpTime t;
            for ( int i = 0; i < 10; i++ ) {
                OpTime s = OpTime::_now();
                verify( s != t );
                t = s;
            }
            OpTime q = t;
            verify( q == t );
            verify( !(q != t) );
        }
    } testoptime;

    int _dummy_z;

    void pretouchN(vector<BSONObj>& v, unsigned a, unsigned b) {
        DEV verify( ! Lock::isW() );

        Client *c = currentClient.get();
        if( c == 0 ) {
            Client::initThread("pretouchN");
            c = &cc();
        }

        Lock::GlobalRead lk;
        for( unsigned i = a; i <= b; i++ ) {
            const BSONObj& op = v[i];
            const char *which = "o";
            const char *opType = op.getStringField("op");
            if ( *opType == 'i' )
                ;
            else if( *opType == 'u' )
                which = "o2";
            else
                continue;
            /* todo : other operations */

            try {
                BSONObj o = op.getObjectField(which);
                BSONElement _id;
                if( o.getObjectID(_id) ) {
                    const char *ns = op.getStringField("ns");
                    BSONObjBuilder b;
                    b.append(_id);
                    BSONObj result;
                    Client::Context ctx( ns );
                    if( Helpers::findById(cc(), ns, b.done(), result) )
                        _dummy_z += result.objsize(); // touch
                }
            }
            catch( DBException& e ) {
                log() << "ignoring assertion in pretouchN() " << a << ' ' << b << ' ' << i << ' ' << e.toString() << endl;
            }
        }
    }

    void pretouchOperation(const BSONObj& op) {

        if( Lock::somethingWriteLocked() )
            return; // no point pretouching if write locked. not sure if this will ever fire, but just in case.

        const char *which = "o";
        const char *opType = op.getStringField("op");
        if ( *opType == 'i' )
            ;
        else if( *opType == 'u' )
            which = "o2";
        else
            return;
        /* todo : other operations */

        try {
            BSONObj o = op.getObjectField(which);
            BSONElement _id;
            if( o.getObjectID(_id) ) {
                const char *ns = op.getStringField("ns");
                BSONObjBuilder b;
                b.append(_id);
                BSONObj result;
                Client::ReadContext ctx( ns );
                if( Helpers::findById(cc(), ns, b.done(), result) )
                    _dummy_z += result.objsize(); // touch
            }
        }
        catch( DBException& ) {
            log() << "ignoring assertion in pretouchOperation()" << endl;
        }
    }

    BSONObj Sync::getMissingDoc(const BSONObj& o) {
        OplogReader missingObjReader;
        const char *ns = o.getStringField("ns");

        // capped collections
        NamespaceDetails *nsd = nsdetails(ns);
        if ( nsd && nsd->isCapped() ) {
            log() << "replication missing doc, but this is okay for a capped collection (" << ns << ")" << endl;
            return BSONObj();
        }

        uassert(15916, str::stream() << "Can no longer connect to initial sync source: " << hn, missingObjReader.connect(hn));

        // might be more than just _id in the update criteria
        BSONObj query = BSONObjBuilder().append(o.getObjectField("o2")["_id"]).obj();
        BSONObj missingObj;
        try {
            missingObj = missingObjReader.findOne(ns, query);
        } catch(DBException& e) {
            log() << "replication assertion fetching missing object: " << e.what() << endl;
            throw;
        }

        return missingObj;
    }

    bool Sync::shouldRetry(const BSONObj& o) {
        // should already have write lock
        const char *ns = o.getStringField("ns");
        Client::Context ctx(ns);

        // we don't have the object yet, which is possible on initial sync.  get it.
        log() << "replication info adding missing object" << endl; // rare enough we can log

        BSONObj missingObj = getMissingDoc(o);

        if( missingObj.isEmpty() ) {
            log() << "replication missing object not found on source. presumably deleted later in oplog" << endl;
            log() << "replication o2: " << o.getObjectField("o2").toString() << endl;
            log() << "replication o firstfield: " << o.getObjectField("o").firstElementFieldName() << endl;

            return false;
        }
        else {
            DiskLoc d = theDataFileMgr.insert(ns, (void*) missingObj.objdata(), missingObj.objsize());
            uassert(15917, "Got bad disk location when attempting to insert", !d.isNull());

            return true;
        }
    }

    /** @param fromRepl false if from ApplyOpsCmd
        @return true if was and update should have happened and the document DNE.  see replset initial sync code.
     */
    bool applyOperation_inlock(const BSONObj& op , bool fromRepl ) {
        LOG(6) << "applying op: " << op << endl;
        bool failedUpdate = false;

        OpCounters * opCounters = fromRepl ? &replOpCounters : &globalOpCounters;

        const char *names[] = { "o", "ns", "op", "b" };
        BSONElement fields[4];
        op.getFields(4, names, fields);

        BSONObj o;
        if( fields[0].isABSONObj() )
            o = fields[0].embeddedObject();
            
        const char *ns = fields[1].valuestrsafe();

        Lock::assertWriteLocked(ns);

        NamespaceDetails *nsd = nsdetails(ns);

        // operation type -- see logOp() comments for types
        const char *opType = fields[2].valuestrsafe();

        if ( *opType == 'i' ) {
            opCounters->gotInsert();

            const char *p = strchr(ns, '.');
            if ( p && strcmp(p, ".system.indexes") == 0 ) {
                // updates aren't allowed for indexes -- so we will do a regular insert. if index already
                // exists, that is ok.
                theDataFileMgr.insert(ns, (void*) o.objdata(), o.objsize());
            }
            else {
                // do upserts for inserts as we might get replayed more than once
                OpDebug debug;
                BSONElement _id;
                if( !o.getObjectID(_id) ) {
                    /* No _id.  This will be very slow. */
                    Timer t;
                    updateObjects(ns, o, o, true, false, false, debug, false,
                                  QueryPlanSelectionPolicy::idElseNatural() );
                    if( t.millis() >= 2 ) {
                        RARELY OCCASIONALLY log() << "warning, repl doing slow updates (no _id field) for " << ns << endl;
                    }
                }
                else {
                    /* erh 10/16/2009 - this is probably not relevant any more since its auto-created, but not worth removing */
                    RARELY if (nsd && !nsd->isCapped()) { ensureHaveIdIndex(ns); } // otherwise updates will be slow

                    /* todo : it may be better to do an insert here, and then catch the dup key exception and do update
                              then.  very few upserts will not be inserts...
                              */
                    BSONObjBuilder b;
                    b.append(_id);
                    updateObjects(ns, o, b.done(), true, false, false , debug, false,
                                  QueryPlanSelectionPolicy::idElseNatural() );
                }
            }
        }
        else if ( *opType == 'u' ) {
            opCounters->gotUpdate();
            // dm do we create this for a capped collection?
            //  - if not, updates would be slow
            //    - but if were by id would be slow on primary too so maybe ok
            //    - if on primary was by another key and there are other indexes, this could be very bad w/out an index
            //  - if do create, odd to have on secondary but not primary.  also can cause secondary to block for
            //    quite a while on creation.
            RARELY if (nsd && !nsd->isCapped()) { ensureHaveIdIndex(ns); } // otherwise updates will be super slow
            OpDebug debug;
            BSONObj updateCriteria = op.getObjectField("o2");
            bool upsert = fields[3].booleanSafe();
            UpdateResult ur = updateObjects(ns, o, updateCriteria, upsert, /*multi*/ false,
                                            /*logop*/ false , debug, /*fromMigrate*/ false,
                                            QueryPlanSelectionPolicy::idElseNatural() );
            if( ur.num == 0 ) { 
                if( ur.mod ) {
                    if( updateCriteria.nFields() == 1 ) {
                        // was a simple { _id : ... } update criteria
                        failedUpdate = true; 
                        // todo: probably should assert in these failedUpdate cases if not in initialSync
                    }
                    // need to check to see if it isn't present so we can set failedUpdate correctly.
                    // note that adds some overhead for this extra check in some cases, such as an updateCriteria
                    // of the form
                    //   { _id:..., { x : {$size:...} }
                    // thus this is not ideal.
                    else {
                        if (nsd == NULL ||
                            (nsd->findIdIndex() >= 0 && Helpers::findById(nsd, updateCriteria).isNull()) ||
                            // capped collections won't have an _id index
                            (nsd->findIdIndex() < 0 && Helpers::findOne(ns, updateCriteria, false).isNull())) {
                            failedUpdate = true;
                        }

                        // Otherwise, it's present; zero objects were updated because of additional specifiers
                        // in the query for idempotence
                    }
                }
                else { 
                    // this could happen benignly on an oplog duplicate replay of an upsert
                    // (because we are idempotent), 
                    // if an regular non-mod update fails the item is (presumably) missing.
                    if( !upsert ) {
                        failedUpdate = true;
                    }
                }
            }
        }
        else if ( *opType == 'd' ) {
            opCounters->gotDelete();
            if ( opType[1] == 0 )
                deleteObjects(ns, o, /*justOne*/ fields[3].booleanSafe());
            else
                verify( opType[1] == 'b' ); // "db" advertisement
        }
        else if ( *opType == 'c' ) {
            opCounters->gotCommand();
            BufBuilder bb;
            BSONObjBuilder ob;
            _runCommands(ns, o, bb, ob, true, 0);
        }
        else if ( *opType == 'n' ) {
            // no op
        }
        else {
            throw MsgAssertionException( 14825 , ErrorMsg("error in applyOperation : unknown opType ", *opType) );
        }
        return failedUpdate;
    }

    class ApplyOpsCmd : public Command {
    public:
        virtual bool slaveOk() const { return false; }
        virtual LockType locktype() const { return WRITE; }
        virtual bool lockGlobally() const { return true; } // SERVER-4328 todo : is global ok or does this take a long time? i believe multiple ns used so locking individually requires more analysis
        ApplyOpsCmd() : Command( "applyOps" ) {}
        virtual void help( stringstream &help ) const {
            help << "internal (sharding)\n{ applyOps : [ ] , preCondition : [ { ns : ... , q : ... , res : ... } ] }";
        }
        virtual bool run(const string& dbname, BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {

            if ( cmdObj.firstElement().type() != Array ) {
                errmsg = "ops has to be an array";
                return false;
            }

            BSONObj ops = cmdObj.firstElement().Obj();

            {
                // check input
                BSONObjIterator i( ops );
                while ( i.more() ) {
                    BSONElement e = i.next();
                    if ( e.type() == Object )
                        continue;
                    errmsg = "op not an object: ";
                    errmsg += e.fieldName();
                    return false;
                }
            }

            if ( cmdObj["preCondition"].type() == Array ) {
                BSONObjIterator i( cmdObj["preCondition"].Obj() );
                while ( i.more() ) {
                    BSONObj f = i.next().Obj();

                    BSONObj realres = db.findOne( f["ns"].String() , f["q"].Obj() );

                    Matcher m( f["res"].Obj() );
                    if ( ! m.matches( realres ) ) {
                        result.append( "got" , realres );
                        result.append( "whatFailed" , f );
                        errmsg = "pre-condition failed";
                        return false;
                    }
                }
            }

            // apply
            int num = 0;
            int errors = 0;
            
            BSONObjIterator i( ops );
            BSONArrayBuilder ab;
            
            while ( i.more() ) {
                BSONElement e = i.next();
                const BSONObj& temp = e.Obj();
                
                Client::Context ctx( temp["ns"].String() ); // this handles security
                bool failed = applyOperation_inlock( temp , false );
                ab.append(!failed);
                if ( failed )
                    errors++;

                num++;
            }

            result.append( "applied" , num );
            result.append( "results" , ab.arr() );

            if ( ! fromRepl ) {
                // We want this applied atomically on slaves
                // so we re-wrap without the pre-condition for speed

                string tempNS = str::stream() << dbname << ".$cmd";

                logOp( "c" , tempNS.c_str() , cmdObj.firstElement().wrap() );
            }

            return errors == 0;
        }

        DBDirectClient db;

    } applyOpsCmd;

}
