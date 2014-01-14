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

#include "mongo/db/repl/oplog.h"

#include <vector>

#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_manager_global.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/dbhash.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/index_builder.h"
#include "mongo/db/instance.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/ops/update.h"
#include "mongo/db/ops/update_lifecycle_impl.h"
#include "mongo/db/ops/delete.h"
#include "mongo/db/repl/bgsync.h"
#include "mongo/db/repl/replication_server_status.h"
#include "mongo/db/repl/rs.h"
#include "mongo/db/repl/write_concern.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/storage_options.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/s/d_logic.h"
#include "mongo/scripting/engine.h"
#include "mongo/util/elapsed_tracker.h"
#include "mongo/util/file.h"
#include "mongo/util/startup_test.h"

namespace mongo {

    // cached copies of these...so don't rename them, drop them, etc.!!!
    static Database* localDB = NULL;
    static Collection* localOplogMainCollection = 0;
    static Collection* localOplogRSCollection = 0;

    void oplogCheckCloseDatabase( Database* db ) {
        verify( Lock::isW() );
        localDB = NULL;
        localOplogMainCollection = NULL;
        localOplogRSCollection = NULL;
        resetSlaveCache();
    }

    // so we can fail the same way
    void checkOplogInsert( StatusWith<DiskLoc> result ) {
        massert( 17322,
                 str::stream() << "write to oplog failed: " << result.getStatus().toString(),
                 result.isOK() );
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
            if ( localOplogRSCollection == 0 ) {
                Client::Context ctx(rsoplog, storageGlobalParams.dbpath);
                localDB = ctx.db();
                verify( localDB );
                localOplogRSCollection = localDB->getCollection( rsoplog );
                massert(13389,
                        "local.oplog.rs missing. did you drop it? if so restart server",
                        localOplogRSCollection);
            }
            Client::Context ctx(rsoplog, localDB);
            checkOplogInsert( localOplogRSCollection->insertDocument( op, false ) );

            /* todo: now() has code to handle clock skew.  but if the skew server to server is large it will get unhappy.
                     this code (or code in now() maybe) should be improved.
                     */
            if( theReplSet ) {
                if( !(theReplSet->lastOpTimeWritten<ts) ) {
                    log() << "replication oplog stream went back in time. previous timestamp: "
                          << theReplSet->lastOpTimeWritten << " newest timestamp: " << ts
                          << ". attempting to sync directly from primary." << endl;
                    std::string errmsg;
                    BSONObjBuilder result;
                    if (!theReplSet->forceSyncFrom(theReplSet->box.getPrimary()->fullName(),
                                                   errmsg, result)) {
                        log() << "Can't sync from primary: " << errmsg << endl;
                    }
                }
                theReplSet->lastOpTimeWritten = ts;
                theReplSet->lastH = h;
                ctx.getClient()->setLastOp( ts );

                replset::BackgroundSync::notify();
            }
        }

        OpTime::setLast( ts );
    }

    /**
     * This allows us to stream the oplog entry directly into data region
     * main goal is to avoid copying the o portion
     * which can be very large
     * TODO: can have this build the entire doc
     */
    class OplogDocWriter : public DocWriter {
    public:
        OplogDocWriter( const BSONObj& frame, const BSONObj& oField )
            : _frame( frame ), _oField( oField ) {
        }

        ~OplogDocWriter(){}

        void writeDocument( char* start ) const {
            char* buf = start;

            memcpy( buf, _frame.objdata(), _frame.objsize() - 1 ); // don't copy final EOO

            reinterpret_cast<int*>( buf )[0] = documentSize();

            buf += ( _frame.objsize() - 1 );
            buf[0] = (char)Object;
            buf[1] = 'o';
            buf[2] = 0;
            memcpy( buf+3, _oField.objdata(), _oField.objsize() );
            buf += 3 + _oField.objsize();
            buf[0] = EOO;

            verify( static_cast<size_t>( ( buf + 1 ) - start ) == documentSize() ); // DEV?
        }

        size_t documentSize() const {
            return _frame.objsize() + _oField.objsize() + 1 /* type */ + 2 /* "o" */;
        }

    private:
        BSONObj _frame;
        BSONObj _oField;
    };

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

        DEV verify( logNS == 0 ); // check this was never a master/slave master

        if ( localOplogRSCollection == 0 ) {
            Client::Context ctx(rsoplog, storageGlobalParams.dbpath);
            localDB = ctx.db();
            verify( localDB );
            localOplogRSCollection = localDB->getCollection( rsoplog );
            massert(13347, "local.oplog.rs missing. did you drop it? if so restart server", localOplogRSCollection);
        }

        Client::Context ctx(rsoplog, localDB);
        OplogDocWriter writer( partial, obj );
        checkOplogInsert( localOplogRSCollection->insertDocument( &writer, false ) );

        /* todo: now() has code to handle clock skew.  but if the skew server to server is large it will get unhappy.
           this code (or code in now() maybe) should be improved.
        */
        if( theReplSet ) {
            if( !(theReplSet->lastOpTimeWritten<ts) ) {
                log() << "replication oplog stream went back in time. previous timestamp: "
                      << theReplSet->lastOpTimeWritten << " newest timestamp: " << ts
                      << ". attempting to sync directly from primary." << endl;
                std::string errmsg;
                BSONObjBuilder result;
                if (!theReplSet->forceSyncFrom(theReplSet->box.getPrimary()->fullName(),
                                               errmsg, result)) {
                    log() << "Can't sync from primary: " << errmsg << endl;
                }
            }
            theReplSet->lastOpTimeWritten = ts;
            theReplSet->lastH = hashNew;
            ctx.getClient()->setLastOp( ts );
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

        if( logNS == 0 ) {
            logNS = "local.oplog.$main";
        }

        if ( localOplogMainCollection == 0 ) {
            Client::Context ctx(logNS, storageGlobalParams.dbpath);
            localDB = ctx.db();
            verify( localDB );
            localOplogMainCollection = localDB->getCollection(logNS);
            verify( localOplogMainCollection );
        }

        Client::Context ctx(logNS , localDB);
        OplogDocWriter writer( partial, obj );
        checkOplogInsert( localOplogMainCollection->insertDocument( &writer, false ) );

        context.getClient()->setLastOp( ts );
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
    void logOp(const char* opstr,
               const char* ns,
               const BSONObj& obj,
               BSONObj* patt,
               bool* b,
               bool fromMigrate,
               const BSONObj* fullObj) {
        if ( replSettings.master ) {
            _logOp(opstr, ns, 0, obj, patt, b, fromMigrate);
        }

        logOpForSharding(opstr, ns, obj, patt, fullObj, fromMigrate);
        logOpForDbHash(opstr, ns, obj, patt, fullObj, fromMigrate);
        getGlobalAuthorizationManager()->logOp(opstr, ns, obj, patt, b);

        if ( strstr( ns, ".system.js" ) ) {
            Scope::storedFuncMod(); // this is terrible
        }

    }

    void createOplog() {
        Lock::GlobalWrite lk;

        const char * ns = "local.oplog.$main";

        bool rs = !replSettings.replSet.empty();
        if( rs )
            ns = rsoplog;

        Client::Context ctx(ns);
        Collection* collection = ctx.db()->getCollection( ns );

        if ( collection ) {

            if (replSettings.oplogSize != 0) {
                int o = (int)(collection->storageSize() / ( 1024 * 1024 ) );
                int n = (int)(replSettings.oplogSize / (1024 * 1024));
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
        if (replSettings.oplogSize != 0)
            sz = (double)replSettings.oplogSize;
        else {
            /* not specified. pick a default size */
            sz = 50.0 * 1024 * 1024;
            if ( sizeof(int *) >= 8 ) {
#if defined(__APPLE__)
                // typically these are desktops (dev machines), so keep it smallish
                sz = (256-64) * 1024 * 1024;
#else
                sz = 990.0 * 1024 * 1024;
                boost::intmax_t free =
                    File::freeSpace(storageGlobalParams.dbpath); //-1 if call not supported.
                double fivePct = free * 0.05;
                if ( fivePct > sz )
                    sz = fivePct;
                // we use 5% of free space up to 50GB (1TB free)
                double upperBound = 50.0 * 1024 * 1024 * 1024;
                if (fivePct > upperBound)
                    sz = upperBound;
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

    /** @param fromRepl false if from ApplyOpsCmd
        @return true if was and update should have happened and the document DNE.  see replset initial sync code.
     */
    bool applyOperation_inlock(const BSONObj& op, bool fromRepl, bool convertUpdateToUpsert) {
        LOG(3) << "applying op: " << op << endl;
        bool failedUpdate = false;

        OpCounters * opCounters = fromRepl ? &replOpCounters : &globalOpCounters;

        const char *names[] = { "o", "ns", "op", "b", "o2" };
        BSONElement fields[5];
        op.getFields(5, names, fields);
        BSONElement& fieldO = fields[0];
        BSONElement& fieldNs = fields[1];
        BSONElement& fieldOp = fields[2];
        BSONElement& fieldB = fields[3];
        BSONElement& fieldO2 = fields[4];

        BSONObj o;
        if( fieldO.isABSONObj() )
            o = fieldO.embeddedObject();

        const char *ns = fieldNs.valuestrsafe();

        BSONObj o2;
        if (fieldO2.isABSONObj())
            o2 = fieldO2.Obj();

        bool valueB = fieldB.booleanSafe();

        Lock::assertWriteLocked(ns);

        Collection* collection = cc().database()->getCollection( ns );
        IndexCatalog* indexCatalog = collection == NULL ? NULL : collection->getIndexCatalog();

        // operation type -- see logOp() comments for types
        const char *opType = fieldOp.valuestrsafe();

        if ( *opType == 'i' ) {
            opCounters->gotInsert();

            const char *p = strchr(ns, '.');
            if ( p && nsToCollectionSubstring( p ) == "system.indexes" ) {
                if (o["background"].trueValue()) {
                    IndexBuilder* builder = new IndexBuilder(o);
                    // This spawns a new thread and returns immediately.
                    builder->go();
                }
                else {
                    Client::Context* ctx = cc().getContext();
                    verify( ctx );
                    IndexBuilder builder(o);
                    uassertStatusOK( builder.build( *ctx ) );
                }
            }
            else {
                // do upserts for inserts as we might get replayed more than once
                OpDebug debug;
                BSONElement _id;
                if( !o.getObjectID(_id) ) {
                    /* No _id.  This will be very slow. */
                    Timer t;

                    const NamespaceString requestNs(ns);
                    UpdateRequest request(requestNs);

                    request.setQuery(o);
                    request.setUpdates(o);
                    request.setUpsert();
                    request.setFromReplication();
                    UpdateLifecycleImpl updateLifecycle(true, requestNs);
                    request.setLifecycle(&updateLifecycle);

                    update(request, &debug);

                    if( t.millis() >= 2 ) {
                        RARELY OCCASIONALLY log() << "warning, repl doing slow updates (no _id field) for " << ns << endl;
                    }
                }
                else {
                    // probably don't need this since all replicated colls have _id indexes now
                    // but keep it just in case
                    RARELY if ( indexCatalog && !collection->isCapped() ) {
                        indexCatalog->ensureHaveIdIndex();
                    }

                    /* todo : it may be better to do an insert here, and then catch the dup key exception and do update
                              then.  very few upserts will not be inserts...
                              */
                    BSONObjBuilder b;
                    b.append(_id);

                    const NamespaceString requestNs(ns);
                    UpdateRequest request(requestNs);

                    request.setQuery(b.done());
                    request.setUpdates(o);
                    request.setUpsert();
                    request.setFromReplication();
                    UpdateLifecycleImpl updateLifecycle(true, requestNs);
                    request.setLifecycle(&updateLifecycle);

                    update(request, &debug);
                }
            }
        }
        else if ( *opType == 'u' ) {
            opCounters->gotUpdate();

            // probably don't need this since all replicated colls have _id indexes now
            // but keep it just in case
            RARELY if ( indexCatalog && !collection->isCapped() ) {
                indexCatalog->ensureHaveIdIndex();
            }

            OpDebug debug;
            BSONObj updateCriteria = o2;
            const bool upsert = valueB || convertUpdateToUpsert;

            const NamespaceString requestNs(ns);
            UpdateRequest request(requestNs);

            request.setQuery(updateCriteria);
            request.setUpdates(o);
            request.setUpsert(upsert);
            request.setFromReplication();
            UpdateLifecycleImpl updateLifecycle(true, requestNs);
            request.setLifecycle(&updateLifecycle);

            UpdateResult ur = update(request, &debug);

            if( ur.numMatched == 0 ) {
                if( ur.modifiers ) {
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
                        if (collection == NULL ||
                            (indexCatalog->haveIdIndex() && Helpers::findById(collection, updateCriteria).isNull()) ||
                            // capped collections won't have an _id index
                            (!indexCatalog->haveIdIndex() && Helpers::findOne(ns, updateCriteria, false).isNull())) {
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
                deleteObjects(ns, o, /*justOne*/ valueB);
            else
                verify( opType[1] == 'b' ); // "db" advertisement
        }
        else if ( *opType == 'c' ) {
            BufBuilder bb;
            BSONObjBuilder ob;
            _runCommands(ns, o, bb, ob, true, 0);
            // _runCommands takes care of adjusting opcounters for command counting.
        }
        else if ( *opType == 'n' ) {
            // no op
        }
        else {
            throw MsgAssertionException( 14825 , ErrorMsg("error in applyOperation : unknown opType ", *opType) );
        }
        getGlobalAuthorizationManager()->logOp(
                opType,
                ns,
                o,
                fieldO2.isABSONObj() ? &o2 : NULL,
                !fieldB.eoo() ? &valueB : NULL );
        return failedUpdate;
    }
}
