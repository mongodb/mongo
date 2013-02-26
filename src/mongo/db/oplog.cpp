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

#include "mongo/pch.h"

#include "mongo/db/oplog.h"

#include <vector>

#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/commands.h"
#include "mongo/db/index_update.h"
#include "mongo/db/instance.h"
#include "mongo/db/namespacestring.h"
#include "mongo/db/ops/update.h"
#include "mongo/db/ops/delete.h"
#include "mongo/db/queryoptimizer.h"
#include "mongo/db/repl.h"
#include "mongo/db/repl_block.h"
#include "mongo/db/repl/bgsync.h"
#include "mongo/db/repl/rs.h"
#include "mongo/db/stats/counters.h"
#include "mongo/util/elapsed_tracker.h"
#include "mongo/util/file.h"
#include "mongo/util/startup_test.h"

namespace mongo {

    // from d_migrate.cpp
    void logOpForSharding( const char * opstr , const char * ns , const BSONObj& obj , BSONObj * patt );

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
                Client::Context ctx(logns , dbpath);
                localDB = ctx.db();
                verify( localDB );
                rsOplogDetails = nsdetails(logns);
                massert(13389, "local.oplog.rs missing. did you drop it? if so restart server", rsOplogDetails);
            }
            Client::Context ctx(logns , localDB);
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

        OpTime::setLast( ts );
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
        *(static_cast<unsigned*>(p)) += o.objsize() + 1/*fieldtype byte*/ + 2/*"o" fieldname*/;

        char *b = static_cast<char *>(p);
        b += size1;
        *b++ = (char) Object;
        *b++ = 'o'; // { o : ... }
        *b++ = 0;   // null terminate "o" fieldname
        memcpy(b, o.objdata(), o.objsize());
        b += o.objsize();
        *b = EOO;
    }

    /* we write to local.oplog.rs:
         { ts : ..., h: ..., v: ..., op: ..., etc }
       ts: an OpTime timestamp
       h: hash
       v: version
       op:
        "i" insert
        "u" update
        "d" delete
        "c" db cmd
        "db" declares presence of a database (ns is set to the db name + '.')
        "n" no op

       bb param:
         if not null, specifies a boolean to pass along to the other side as b: param.
         used for "justOne" or "upsert" flags on 'd', 'u'

    */

    // global is safe as we are in write lock. we put the static outside the function to avoid the implicit mutex 
    // the compiler would use if inside the function.  the reason this is static is to avoid a malloc/free for this
    // on every logop call.
    static BufBuilder logopbufbuilder(8*1024);
    static const int OPLOG_VERSION = 2;
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
        b.append("v", OPLOG_VERSION);
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
                Client::Context ctx(logns , dbpath);
                localDB = ctx.db();
                verify( localDB );
                rsOplogDetails = nsdetails(logns);
                massert(13347, "local.oplog.rs missing. did you drop it? if so restart server", rsOplogDetails);
            }
            Client::Context ctx(logns , localDB);
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
            LOG( 6 ) << "logOp:" << BSONObj::make(r) << endl;
        }
    }

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
        Client::Context context("", 0);

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
                Client::Context ctx(logNS , dbpath);
                localDB = ctx.db();
                verify( localDB );
                localOplogMainDetails = nsdetails(logNS);
                verify( localOplogMainDetails );
            }
            Client::Context ctx(logNS , localDB);
            r = theDataFileMgr.fast_oplog_insert(localOplogMainDetails, logNS, len);
        }
        else {
            Client::Context ctx(logNS, dbpath);
            verify( nsdetails( logNS ) );
            // first we allocate the space, then we fill it below.
            r = theDataFileMgr.fast_oplog_insert( nsdetails( logNS ), logNS, len);
        }

        append_O_Obj(r->data(), partial, obj);

        context.getClient()->setLastOp( ts );

        LOG( 6 ) << "logging op:" << BSONObj::make(r) << endl;
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
            sz = 50.0 * 1024 * 1024;
            if ( sizeof(int *) >= 8 ) {
#if defined(__APPLE__)
                // typically these are desktops (dev machines), so keep it smallish
                sz = (256-64) * 1024 * 1024;
#else
                sz = 990.0 * 1024 * 1024;
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

    void Sync::setHostname(const string& hostname) {
        hn = hostname;
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

            LOG(1) << "replication inserted missing doc: " << missingObj.toString() << endl;
            return true;
        }
    }

    /** @param fromRepl false if from ApplyOpsCmd
        @return true if was and update should have happened and the document DNE.  see replset initial sync code.
     */
    bool applyOperation_inlock(const BSONObj& op, bool fromRepl, bool convertUpdateToUpsert) {
        LOG(3) << "applying op: " << op << endl;
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

            if (NamespaceString(ns).coll == "system.indexes") {
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
                    updateObjectsForReplication(ns, o, o, true, false, false, debug, false,
                                                QueryPlanSelectionPolicy::idElseNatural() );
                    if( t.millis() >= 2 ) {
                        RARELY OCCASIONALLY log() << "warning, repl doing slow updates (no _id field) for " << ns << endl;
                    }
                }
                else {
                    // probably don't need this since all replicated colls have _id indexes now
                    // but keep it just in case
                    RARELY if ( nsd && !nsd->isCapped() ) { ensureHaveIdIndex(ns, false); }

                    /* todo : it may be better to do an insert here, and then catch the dup key exception and do update
                              then.  very few upserts will not be inserts...
                              */
                    BSONObjBuilder b;
                    b.append(_id);
                    updateObjectsForReplication(ns, o, b.done(), true, false, false , debug, false,
                                                QueryPlanSelectionPolicy::idElseNatural() );
                }
            }
        }
        else if ( *opType == 'u' ) {
            opCounters->gotUpdate();

            // probably don't need this since all replicated colls have _id indexes now
            // but keep it just in case
            RARELY if ( nsd && !nsd->isCapped() ) { ensureHaveIdIndex(ns, false); }

            OpDebug debug;
            BSONObj updateCriteria = op.getObjectField("o2");
            bool upsert = fields[3].booleanSafe() || convertUpdateToUpsert;
            UpdateResult ur =
                updateObjectsForReplication(ns,
                                            o,
                                            updateCriteria,
                                            upsert,
                                            /*multi*/ false,
                                            /*logop*/ false,
                                            debug,
                                            /*fromMigrate*/ false,
                                            QueryPlanSelectionPolicy::idElseNatural() );

            if( ur.num == 0 ) {
                if( ur.mod ) {
                    if( updateCriteria.nFields() == 1 ) {
                        // was a simple { _id : ... } update criteria
                        failedUpdate = true;
                        log() << "replication failed to apply update: " << op.toString() << endl;
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
                            log() << "replication couldn't find doc: " << op.toString() << endl;
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
                        log() << "replication update of non-mod failed: " << op.toString() << endl;
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
}
