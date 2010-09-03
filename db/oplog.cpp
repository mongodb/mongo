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

namespace mongo {

    void logOpForSharding( const char * opstr , const char * ns , const BSONObj& obj , BSONObj * patt );

    int __findingStartInitialTimeout = 5; // configurable for testing    

    // cached copies of these...so don't rename them, drop them, etc.!!!
    static NamespaceDetails *localOplogMainDetails = 0;
    static Database *localDB = 0;
    static NamespaceDetails *rsOplogDetails = 0;
    void oplogCheckCloseDatabase( Database * db ){
        localDB = 0;
        localOplogMainDetails = 0;
        rsOplogDetails = 0;
        resetSlaveCache();
    }

    static void _logOpUninitialized(const char *opstr, const char *ns, const char *logNS, const BSONObj& obj, BSONObj *o2, bool *bb ) {
        uassert(13288, "replSet error write op to db before replSet initialized", str::startsWith(ns, "local.") || *opstr == 'n');
    }

    /** write an op to the oplog that is already built. 
        todo : make _logOpRS() call this so we don't repeat ourself?
        */
    void _logOpObjRS(const BSONObj& op) { 
        DEV assertInWriteLock();

        const OpTime ts = op["ts"]._opTime();
        long long h = op["h"].numberLong();

        {
            const char *logns = rsoplog;
            if ( rsOplogDetails == 0 ) {
                Client::Context ctx( logns , dbpath, 0, false);
                localDB = ctx.db();
                assert( localDB );
                rsOplogDetails = nsdetails(logns);
                massert(13389, "local.oplog.rs missing. did you drop it? if so restart server", rsOplogDetails);
            }
            Client::Context ctx( logns , localDB, false );
            {
                int len = op.objsize();
                Record *r = theDataFileMgr.fast_oplog_insert(rsOplogDetails, logns, len);
                memcpy(r->data, op.objdata(), len);
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
                ctx.getClient()->setLastOp( ts.asDate() );
            }
        }
    }

    static void _logOpRS(const char *opstr, const char *ns, const char *logNS, const BSONObj& obj, BSONObj *o2, bool *bb ) {
        DEV assertInWriteLock();
        static BufBuilder bufbuilder(8*1024);
        
        if ( strncmp(ns, "local.", 6) == 0 ){
            if ( strncmp(ns, "local.slaves", 12) == 0 )
                resetSlaveCache();
            return;
        }

        const OpTime ts = OpTime::now();

        long long hNew;
        if( theReplSet ) { 
            massert(13312, "replSet error : logOp() but not primary?", theReplSet->box.getState().primary());
            hNew = (theReplSet->lastH * 131 + ts.asLL()) * 17 + theReplSet->selfId();
        }
        else {
            // must be initiation
            assert( *ns == 0 );
            hNew = 0;
        }

        /* we jump through a bunch of hoops here to avoid copying the obj buffer twice --
           instead we do a single copy to the destination position in the memory mapped file.
        */

        bufbuilder.reset();
        BSONObjBuilder b(bufbuilder);

        b.appendTimestamp("ts", ts.asDate());
        b.append("h", hNew);

        b.append("op", opstr);
        b.append("ns", ns);
        if ( bb )
            b.appendBool("b", *bb);
        if ( o2 )
            b.append("o2", *o2);
        BSONObj partial = b.done();
        int posz = partial.objsize();
        int len = posz + obj.objsize() + 1 + 2 /*o:*/;

        Record *r;
        DEV assert( logNS == 0 );
        {
            const char *logns = rsoplog;
            if ( rsOplogDetails == 0 ) {
                Client::Context ctx( logns , dbpath, 0, false);
                localDB = ctx.db();
                assert( localDB );
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
                theReplSet->lastH = hNew;
                ctx.getClient()->setLastOp( ts.asDate() );
            }
        }

        char *p = r->data;
        memcpy(p, partial.objdata(), posz);
        *((unsigned *)p) += obj.objsize() + 1 + 2;
        p += posz - 1;
        *p++ = (char) Object;
        *p++ = 'o';
        *p++ = 0;
        memcpy(p, obj.objdata(), obj.objsize());
        p += obj.objsize();
        *p = EOO;
        
        if ( logLevel >= 6 ) {
            BSONObj temp(r);
            log( 6 ) << "logOp:" << temp << endl;
        }
    }

    /* we write to local.opload.$main:
         { ts : ..., op: ..., ns: ..., o: ... }
       ts: an OpTime timestamp
       op:
        "i" insert
        "u" update
        "d" delete
        "c" db cmd
        "db" declares presence of a database (ns is set to the db name + '.')
        "n" no op
       logNS - where to log it.  0/null means "local.oplog.$main".
       bb:
         if not null, specifies a boolean to pass along to the other side as b: param.
         used for "justOne" or "upsert" flags on 'd', 'u'
       first: true
         when set, indicates this is the first thing we have logged for this database.
         thus, the slave does not need to copy down all the data when it sees this.

       note this is used for single collection logging even when --replSet is enabled.
    */
    static void _logOpOld(const char *opstr, const char *ns, const char *logNS, const BSONObj& obj, BSONObj *o2, bool *bb ) {
        DEV assertInWriteLock();
        static BufBuilder bufbuilder(8*1024);
        
        if ( strncmp(ns, "local.", 6) == 0 ){
            if ( strncmp(ns, "local.slaves", 12) == 0 ){
                resetSlaveCache();
            }
            return;
        }

        const OpTime ts = OpTime::now();
        Client::Context context;
        
        /* we jump through a bunch of hoops here to avoid copying the obj buffer twice --
           instead we do a single copy to the destination position in the memory mapped file.
        */

        bufbuilder.reset();
        BSONObjBuilder b(bufbuilder);
        b.appendTimestamp("ts", ts.asDate());
        b.append("op", opstr);
        b.append("ns", ns);
        if ( bb )
            b.appendBool("b", *bb);
        if ( o2 )
            b.append("o2", *o2);
        BSONObj partial = b.done();
        int posz = partial.objsize();
        int len = posz + obj.objsize() + 1 + 2 /*o:*/;

        Record *r;
        if( logNS == 0 ) {
            logNS = "local.oplog.$main";
            if ( localOplogMainDetails == 0 ) {
                Client::Context ctx( logNS , dbpath, 0, false);
                localDB = ctx.db();
                assert( localDB );
                localOplogMainDetails = nsdetails(logNS);
                assert( localOplogMainDetails );
            }
            Client::Context ctx( logNS , localDB, false );
            r = theDataFileMgr.fast_oplog_insert(localOplogMainDetails, logNS, len);
        } else {
            Client::Context ctx( logNS, dbpath, 0, false );
            assert( nsdetails( logNS ) );
            r = theDataFileMgr.fast_oplog_insert( nsdetails( logNS ), logNS, len);
        }

        char *p = r->data;
        memcpy(p, partial.objdata(), posz);
        *((unsigned *)p) += obj.objsize() + 1 + 2;
        p += posz - 1;
        *p++ = (char) Object;
        *p++ = 'o';
        *p++ = 0;
        memcpy(p, obj.objdata(), obj.objsize());
        p += obj.objsize();
        *p = EOO;
        
        context.getClient()->setLastOp( ts.asDate() );

        if ( logLevel >= 6 ) {
            BSONObj temp(r);
            log( 6 ) << "logging op:" << temp << endl;
        }

    }

    static void (*_logOp)(const char *opstr, const char *ns, const char *logNS, const BSONObj& obj, BSONObj *o2, bool *bb ) = _logOpOld;
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
        _logOp("n", "", 0, BSONObj(), 0, 0);
    }
    void logOpComment(const BSONObj& obj) {
        _logOp("n", "", 0, obj, 0, 0);
    }
    void logOpInitiate(const BSONObj& obj) {
        _logOpRS("n", "", 0, obj, 0, 0);
    }

    /*@ @param opstr:
          c userCreateNS
          i insert
          n no-op / keepalive
          d delete / remove
          u update
    */
    void logOp(const char *opstr, const char *ns, const BSONObj& obj, BSONObj *patt, bool *b) {
        if ( replSettings.master ) {
            _logOp(opstr, ns, 0, obj, patt, b);
            // why? :
            //char cl[ 256 ];
            //nsToDatabase( ns, cl );
        }
        
        logOpForSharding( opstr , ns , obj , patt );
    }    

    void createOplog() {
        dblock lk;

        const char * ns = "local.oplog.$main";

        bool rs = !cmdLine._replSet.empty();
        if( rs )
            ns = rsoplog;

        Client::Context ctx(ns);
        
        NamespaceDetails * nsd = nsdetails( ns );

        if ( nsd ) {
            
            if ( cmdLine.oplogSize != 0 ){
                int o = (int)(nsd->storageSize() / ( 1024 * 1024 ) );
                int n = (int)(cmdLine.oplogSize / ( 1024 * 1024 ) );
                if ( n != o ){
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
                boost::intmax_t free = freeSpace(); //-1 if call not supported.
                double fivePct = free * 0.05;
                if ( fivePct > sz )
                    sz = fivePct;
#endif
            }
        }

        log() << "******" << endl;
        log() << "creating replication oplog of size: " << (int)( sz / ( 1024 * 1024 ) ) << "MB... (use --oplogSize to change)" << endl;

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

    struct TestOpTime {
        TestOpTime() {
            OpTime t;
            for ( int i = 0; i < 10; i++ ) {
                OpTime s = OpTime::now();
                assert( s != t );
                t = s;
            }
            OpTime q = t;
            assert( q == t );
            assert( !(q != t) );
        }
    } testoptime;

    int _dummy_z;

    void pretouchN(vector<BSONObj>& v, unsigned a, unsigned b) {
        DEV assert( !dbMutex.isWriteLocked() );

        Client *c = &cc();
        if( c == 0 ) { 
            Client::initThread("pretouchN");
            c = &cc();
        }

        readlock lk("");
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

        if( dbMutex.isWriteLocked() )
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
                readlock lk(ns);
                Client::Context ctx( ns );
                if( Helpers::findById(cc(), ns, b.done(), result) )
                    _dummy_z += result.objsize(); // touch
            }
        }
        catch( DBException& ) { 
            log() << "ignoring assertion in pretouchOperation()" << endl;
        }
    }

    void applyOperation_inlock(const BSONObj& op){
        if( logLevel >= 6 ) 
            log() << "applying op: " << op << endl;
        
        assertInWriteLock();

        OpDebug debug;
        BSONObj o = op.getObjectField("o");
        const char *ns = op.getStringField("ns");
        // operation type -- see logOp() comments for types
        const char *opType = op.getStringField("op");

        if ( *opType == 'i' ) {
            const char *p = strchr(ns, '.');
            if ( p && strcmp(p, ".system.indexes") == 0 ) {
                // updates aren't allowed for indexes -- so we will do a regular insert. if index already
                // exists, that is ok.
                theDataFileMgr.insert(ns, (void*) o.objdata(), o.objsize());
            }
            else {
                // do upserts for inserts as we might get replayed more than once
                BSONElement _id;
                if( !o.getObjectID(_id) ) {
                    /* No _id.  This will be very slow. */
                    Timer t;
                    updateObjects(ns, o, o, true, false, false , debug );
                    if( t.millis() >= 2 ) {
                        RARELY OCCASIONALLY log() << "warning, repl doing slow updates (no _id field) for " << ns << endl;
                    }
                }
                else {
                    BSONObjBuilder b;
                    b.append(_id);
                    
                    /* erh 10/16/2009 - this is probably not relevant any more since its auto-created, but not worth removing */
                    RARELY ensureHaveIdIndex(ns); // otherwise updates will be slow 

                    /* todo : it may be better to do an insert here, and then catch the dup key exception and do update 
                              then.  very few upserts will not be inserts...
                              */
                    updateObjects(ns, o, b.done(), true, false, false , debug );
                }
            }
        }
        else if ( *opType == 'u' ) {
            RARELY ensureHaveIdIndex(ns); // otherwise updates will be super slow
            updateObjects(ns, o, op.getObjectField("o2"), /*upsert*/ op.getBoolField("b"), /*multi*/ false, /*logop*/ false , debug );
        }
        else if ( *opType == 'd' ) {
            if ( opType[1] == 0 )
                deleteObjects(ns, o, op.getBoolField("b"));
            else
                assert( opType[1] == 'b' ); // "db" advertisement
        }
        else if ( *opType == 'n' ) {
            // no op
        }
        else if ( *opType == 'c' ){
            BufBuilder bb;
            BSONObjBuilder ob;
            _runCommands(ns, o, bb, ob, true, 0);
        }
        else {
            stringstream ss;
            ss << "unknown opType [" << opType << "]";
            throw MsgAssertionException( 13141 , ss.str() );
        }
        
    }
    
    class ApplyOpsCmd : public Command {
    public:
        virtual bool slaveOk() const { return false; }
        virtual LockType locktype() const { return WRITE; }
        ApplyOpsCmd() : Command( "applyOps" ) {}
        virtual void help( stringstream &help ) const {
            help << "examples: { applyOps : [ ] , preCondition : [ { ns : ... , q : ... , res : ... } ] }";
        }
        virtual bool run(const string& dbname, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            
            if ( cmdObj.firstElement().type() != Array ){
                errmsg = "ops has to be an array";
                return false;
            }
            
            BSONObj ops = cmdObj.firstElement().Obj();
            
            { // check input
                BSONObjIterator i( ops );
                while ( i.more() ){
                    BSONElement e = i.next();
                    if ( e.type() == Object )
                        continue;
                    errmsg = "op not an object: ";
                    errmsg += e.fieldName();
                    return false;
                }
            }
            
            if ( cmdObj["preCondition"].type() == Array ){
                BSONObjIterator i( cmdObj["preCondition"].Obj() );
                while ( i.more() ){
                    BSONObj f = i.next().Obj();
                    
                    BSONObj realres = db.findOne( f["ns"].String() , f["q"].Obj() );
                    
                    Matcher m( f["res"].Obj() );
                    if ( ! m.matches( realres ) ){
                        result.append( "got" , realres );
                        result.append( "whatFailed" , f );
                        errmsg = "pre-condition failed";
                        return false;
                    }
                }
            }
            
            // apply
            int num = 0;
            BSONObjIterator i( ops );
            while ( i.more() ){
                BSONElement e = i.next();
                applyOperation_inlock( e.Obj() );
                num++;
            }

            result.append( "applied" , num );

            return true;
        }

        DBDirectClient db;
        
    } applyOpsCmd;

}
