// oplog.cpp

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

    // cached copies of these...so don't rename them
    NamespaceDetails *localOplogMainDetails = 0;
    Database *localOplogDB = 0;

    void oplogCheckCloseDatabase( Database * db ){
        localOplogDB = 0;
        localOplogMainDetails = 0;
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
       logNS - e.g. "local.oplog.$main"
       bb:
         if not null, specifies a boolean to pass along to the other side as b: param.
         used for "justOne" or "upsert" flags on 'd', 'u'
       first: true
         when set, indicates this is the first thing we have logged for this database.
         thus, the slave does not need to copy down all the data when it sees this.
    */
    static void _logOp(const char *opstr, const char *ns, const char *logNS, const BSONObj& obj, BSONObj *o2, bool *bb, const OpTime &ts ) {
        if ( strncmp(ns, "local.", 6) == 0 ){
            if ( strncmp(ns, "local.slaves", 12) == 0 ){
                resetSlaveCache();
            }
            return;
        }

        DEV assertInWriteLock();
        
        Client::Context context;
        
        /* we jump through a bunch of hoops here to avoid copying the obj buffer twice --
           instead we do a single copy to the destination position in the memory mapped file.
        */

        BSONObjBuilder b;
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
        if ( strncmp( logNS, "local.", 6 ) == 0 ) { // For now, assume this is olog main
            if ( localOplogMainDetails == 0 ) {
                Client::Context ctx("local.", dbpath, 0, false);
                localOplogDB = ctx.db();
                localOplogMainDetails = nsdetails(logNS);
            }
            Client::Context ctx( "" , localOplogDB, false );
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
        
        context.getClient()->setLastOp( ts );
    }

    void logKeepalive() { 
        BSONObj obj;
        _logOp("n", "", "local.oplog.$main", obj, 0, 0, OpTime::now());
    }

    void logOp(const char *opstr, const char *ns, const BSONObj& obj, BSONObj *patt, bool *b) {
        if ( replSettings.master ) {
            _logOp(opstr, ns, "local.oplog.$main", obj, patt, b, OpTime::now());
            char cl[ 256 ];
            nsToDatabase( ns, cl );
        }
        NamespaceDetailsTransient &t = NamespaceDetailsTransient::get_w( ns );
        if ( t.cllEnabled() ) {
            try {
                _logOp(opstr, ns, t.cllNS().c_str(), obj, patt, b, OpTime::now());
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
                    ss << "cmdline oplogsize (" << n << ") different than existing (" << o << ")";
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
