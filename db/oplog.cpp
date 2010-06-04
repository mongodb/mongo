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

namespace mongo {

    int __findingStartInitialTimeout = 5; // configurable for testing    

    // cached copies of these...so don't rename them, drop them, etc.!!!
    static NamespaceDetails *localOplogMainDetails = 0;
    static Database *localDB = 0;
    static NamespaceDetails *rsOplogDetails = 0;
    void oplogCheckCloseDatabase( Database * db ){
        localDB = 0;
        localOplogMainDetails = 0;
        rsOplogDetails = 0;
    }

    static void _logOpUninitialized(const char *opstr, const char *ns, const char *logNS, const BSONObj& obj, BSONObj *o2, bool *bb ) {
        log() << "replSet logop not done" << endl;
        uassert(13288, "replSet error write op to db before replSet initialized", str::startsWith(ns, "local.") || *opstr == 'n');
    }

    static void _logOpRS(const char *opstr, const char *ns, const char *logNS, const BSONObj& obj, BSONObj *o2, bool *bb ) {
        DEV assertInWriteLock();
        DEV assert( theReplSet );
        static BufBuilder bufbuilder(8*1024);
        
        if ( strncmp(ns, "local.", 6) == 0 ){
            if ( strncmp(ns, "local.slaves", 12) == 0 )
                resetSlaveCache();
            return;
        }

        /* we jump through a bunch of hoops here to avoid copying the obj buffer twice --
           instead we do a single copy to the destination position in the memory mapped file.
        */

        bufbuilder.reset();
        BSONObjBuilder b(bufbuilder);
        DEV assert( theReplSet->isPrimary() );
        DEV assert( rsOpTime.initiated() );
        const ReplTime ts = rsOpTime.inc();
        b.append("t", (long long) ts);
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
            const char *logns = "local.oplog.rs";
            if ( rsOplogDetails == 0 ) {
                Client::Context ctx("local.", dbpath, 0, false);
                localDB = ctx.db();
                rsOplogDetails = nsdetails(logns);
            }
            Client::Context ctx( "" , localDB, false );
            r = theDataFileMgr.fast_oplog_insert(localOplogMainDetails, logns, len);
            ctx.getClient()->setLastOp( ts );
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
                Client::Context ctx("local.", dbpath, 0, false);
                localDB = ctx.db();
                localOplogMainDetails = nsdetails(logNS);
            }
            Client::Context ctx( "" , localDB, false );
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
        
        if ( logLevel >= 6 ) {
            BSONObj temp(r);
            log( 6 ) << "logging op:" << temp << endl;
        }
        
        context.getClient()->setLastOp( ts.asDate() );
    }

    static void (*_logOp)(const char *opstr, const char *ns, const char *logNS, const BSONObj& obj, BSONObj *o2, bool *bb ) = _logOpOld;
    void newReplUp() { _logOp = _logOpRS; }
    void newRepl() { _logOp = _logOpUninitialized; }
    void oldRepl() { _logOp = _logOpOld; }

    void logKeepalive() { 
        _logOp("n", "", 0, BSONObj(), 0, 0);
    }
    void logOpComment(const BSONObj& obj) {
        _logOp("n", "", 0, obj, 0, 0);
    }

    void logOp(const char *opstr, const char *ns, const BSONObj& obj, BSONObj *patt, bool *b) {
        if ( replSettings.master ) {
            _logOp(opstr, ns, 0, obj, patt, b);
            // why? :
            //char cl[ 256 ];
            //nsToDatabase( ns, cl );
        }
        NamespaceDetailsTransient &t = NamespaceDetailsTransient::get_w( ns );
        if ( t.cllEnabled() ) {
            try {
                _logOpOld(opstr, ns, t.cllNS().c_str(), obj, patt, b);
            } catch ( const DBException & ) {
                t.cllInvalidate();
            }
        }
    }    

    void createOplog() {
        dblock lk;

        const char * ns = "local.oplog.$main";
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

            DBDirectClient c;
            BSONObj lastOp = c.findOne( ns, Query().sort( BSON( "$natural" << -1 ) ) );
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

        log() << "******\n";
        log() << "creating replication oplog of size: " << (int)( sz / ( 1024 * 1024 ) ) << "MB (use --oplogSize to change)\n";
        log() << "******" << endl;

        b.append("size", sz);
        b.appendBool("capped", 1);
        b.appendBool("autoIndexId", false);

        string err;
        BSONObj o = b.done();
        userCreateNS(ns, o, err, false);
        logOp( "n", "dummy", BSONObj() );
    }

    class CmdLogCollection : public Command {
    public:
        virtual bool slaveOk() const {
            return false;
        }
        virtual LockType locktype() const { return WRITE; }
        CmdLogCollection() : Command( "logCollection" ) {}
        virtual void help( stringstream &help ) const {
            help << "examples: { logCollection: <collection ns>, start: 1 }, "
                 << "{ logCollection: <collection ns>, validateComplete: 1 }";
        }
        virtual bool run(const string& dbname, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            string logCollection = cmdObj.getStringField( "logCollection" );
            if ( logCollection.empty() ) {
                errmsg = "missing logCollection spec";
                return false;
            }
            bool start = !cmdObj.getField( "start" ).eoo();
            bool validateComplete = !cmdObj.getField( "validateComplete" ).eoo();
            if ( start ? validateComplete : !validateComplete ) {
                errmsg = "Must specify exactly one of start:1 or validateComplete:1";
                return false;
            }
            int logSizeMb = cmdObj.getIntField( "logSizeMb" );
            NamespaceDetailsTransient &t = NamespaceDetailsTransient::get_w( logCollection.c_str() );
            if ( start ) {
                if ( t.cllNS().empty() ) {
                    if ( logSizeMb == INT_MIN ) {
                        t.cllStart();
                    } else {
                        t.cllStart( logSizeMb );
                    }
                } else {
                    errmsg = "Log already started for ns: " + logCollection;
                    return false;
                }
            } else {
                if ( t.cllNS().empty() ) {
                    errmsg = "No log to validateComplete for ns: " + logCollection;
                    return false;
                } else {
                    if ( !t.cllValidateComplete() ) {
                        errmsg = "Oplog failure, insufficient space allocated";
                        return false;
                    }
                }
            }
            log() << "started logCollection with cmd obj: " << cmdObj << endl;
            return true;
        }
    } cmdlogcollection;

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

}
