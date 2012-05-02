// @file repl.cpp

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

/* Collections we use:

   local.sources         - indicates what sources we pull from as a "slave", and the last update of each
   local.oplog.$main     - our op log as "master"
   local.dbinfo.<dbname> - no longer used???
   local.pair.startup    - [deprecated] can contain a special value indicating for a pair that we have the master copy.
                           used when replacing other half of the pair which has permanently failed.
   local.pair.sync       - [deprecated] { initialsynccomplete: 1 }
*/

#include "pch.h"
#include "jsobj.h"
#include "../util/goodies.h"
#include "repl.h"
#include "../util/net/message.h"
#include "../util/background.h"
#include "../client/connpool.h"
#include "pdfile.h"
#include "db.h"
#include "commands.h"
#include "security.h"
#include "cmdline.h"
#include "repl_block.h"
#include "repl/rs.h"
#include "replutil.h"
#include "repl/connections.h"
#include "ops/update.h"
#include "pcrecpp.h"
#include "mongo/db/instance.h"

namespace mongo {

    // our config from command line etc.
    ReplSettings replSettings;

    /* if 1 sync() is running */
    volatile int syncing = 0;
    static volatile int relinquishSyncingSome = 0;

    /* "dead" means something really bad happened like replication falling completely out of sync.
       when non-null, we are dead and the string is informational
    */
    const char *replAllDead = 0;

    time_t lastForcedResync = 0;

} // namespace mongo

namespace mongo {

    /* output by the web console */
    const char *replInfo = "";
    struct ReplInfo {
        ReplInfo(const char *msg) {
            replInfo = msg;
        }
        ~ReplInfo() {
            replInfo = "?";
        }
    };

    /* operator requested resynchronization of replication (on the slave).  { resync : 1 } */
    class CmdResync : public Command {
    public:
        virtual bool slaveOk() const {
            return true;
        }
        virtual bool adminOnly() const {
            return true;
        }
        virtual bool logTheOp() { return false; }
        virtual bool lockGlobally() const { return true; }
        virtual LockType locktype() const { return WRITE; }
        void help(stringstream&h) const { h << "resync (from scratch) an out of date replica slave.\nhttp://www.mongodb.org/display/DOCS/Master+Slave"; }
        CmdResync() : Command("resync") { }
        virtual bool run(const string& , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            if( cmdLine.usingReplSets() ) {
                errmsg = "resync command not currently supported with replica sets.  See RS102 info in the mongodb documentations";
                result.append("info", "http://www.mongodb.org/display/DOCS/Resyncing+a+Very+Stale+Replica+Set+Member");
                return false;
            }

            if ( cmdObj.getBoolField( "force" ) ) {
                if ( !waitForSyncToFinish( errmsg ) )
                    return false;
                replAllDead = "resync forced";
            }
            if ( !replAllDead ) {
                errmsg = "not dead, no need to resync";
                return false;
            }
            if ( !waitForSyncToFinish( errmsg ) )
                return false;

            ReplSource::forceResyncDead( "client" );
            result.append( "info", "triggered resync for all sources" );
            return true;
        }
        bool waitForSyncToFinish( string &errmsg ) const {
            // Wait for slave thread to finish syncing, so sources will be be
            // reloaded with new saved state on next pass.
            Timer t;
            while ( 1 ) {
                if ( syncing == 0 || t.millis() > 30000 )
                    break;
                {
                    Lock::TempRelease t;
                    relinquishSyncingSome = 1;
                    sleepmillis(1);
                }
            }
            if ( syncing ) {
                errmsg = "timeout waiting for sync() to finish";
                return false;
            }
            return true;
        }
    } cmdResync;

    bool anyReplEnabled() {
        return replSettings.slave || replSettings.master || theReplSet;
    }

    bool replAuthenticate(DBClientBase *conn);

    void appendReplicationInfo( BSONObjBuilder& result , bool authed , int level ) {

        if ( replSet ) {
            if( theReplSet == 0 ) {
                result.append("ismaster", false);
                result.append("secondary", false);
                result.append("info", ReplSet::startupStatusMsg.get());
                result.append( "isreplicaset" , true );
                return;
            }

            theReplSet->fillIsMaster(result);
            return;
        }

        if ( replAllDead ) {
            result.append("ismaster", 0);
            string s = string("dead: ") + replAllDead;
            result.append("info", s);
        }
        else {
            result.appendBool("ismaster", _isMaster() );
        }

        if ( level && replSet ) {
            result.append( "info" , "is replica set" );
        }
        else if ( level ) {
            BSONObjBuilder sources( result.subarrayStart( "sources" ) );

            int n = 0;
            list<BSONObj> src;
            {
                Client::ReadContext ctx( "local.sources", dbpath, authed );
                shared_ptr<Cursor> c = findTableScan("local.sources", BSONObj());
                while ( c->ok() ) {
                    src.push_back(c->current());
                    c->advance();
                }
            }

            for( list<BSONObj>::const_iterator i = src.begin(); i != src.end(); i++ ) {
                BSONObj s = *i;
                BSONObjBuilder bb;
                bb.append( s["host"] );
                string sourcename = s["source"].valuestr();
                if ( sourcename != "main" )
                    bb.append( s["source"] );
                {
                    BSONElement e = s["syncedTo"];
                    BSONObjBuilder t( bb.subobjStart( "syncedTo" ) );
                    t.appendDate( "time" , e.timestampTime() );
                    t.append( "inc" , e.timestampInc() );
                    t.done();
                }

                if ( level > 1 ) {
                    wassert( !Lock::isLocked() );
                    // note: there is no so-style timeout on this connection; perhaps we should have one.
                    ScopedDbConnection conn( s["host"].valuestr() );
                    DBClientConnection *cliConn = dynamic_cast< DBClientConnection* >( &conn.conn() );
                    if ( cliConn && replAuthenticate( cliConn ) ) {
                        BSONObj first = conn->findOne( (string)"local.oplog.$" + sourcename , Query().sort( BSON( "$natural" << 1 ) ) );
                        BSONObj last = conn->findOne( (string)"local.oplog.$" + sourcename , Query().sort( BSON( "$natural" << -1 ) ) );
                        bb.appendDate( "masterFirst" , first["ts"].timestampTime() );
                        bb.appendDate( "masterLast" , last["ts"].timestampTime() );
                        double lag = (double) (last["ts"].timestampTime() - s["syncedTo"].timestampTime());
                        bb.append( "lagSeconds" , lag / 1000 );
                    }
                    conn.done();
                }

                sources.append( BSONObjBuilder::numStr( n++ ) , bb.obj() );
            }

            sources.done();
        }
    }

    class CmdIsMaster : public Command {
    public:
        virtual bool requiresAuth() { return false; }
        virtual bool slaveOk() const {
            return true;
        }
        virtual void help( stringstream &help ) const {
            help << "Check if this server is primary for a replica pair/set; also if it is --master or --slave in simple master/slave setups.\n";
            help << "{ isMaster : 1 }";
        }
        virtual LockType locktype() const { return NONE; }
        CmdIsMaster() : Command("isMaster", true, "ismaster") { }
        virtual bool run(const string& , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool /*fromRepl*/) {
            /* currently request to arbiter is (somewhat arbitrarily) an ismaster request that is not
               authenticated.
               we allow unauthenticated ismaster but we aren't as verbose informationally if
               one is not authenticated for admin db to be safe.
            */
            bool authed = cc().getAuthenticationInfo()->isAuthorizedReads("admin");
            appendReplicationInfo( result , authed );

            result.appendNumber("maxBsonObjectSize", BSONObjMaxUserSize);
            result.appendDate("localTime", jsTime());
            return true;
        }
    } cmdismaster;

    ReplSource::ReplSource() {
        nClonedThisPass = 0;
    }

    ReplSource::ReplSource(BSONObj o) : nClonedThisPass(0) {
        only = o.getStringField("only");
        hostName = o.getStringField("host");
        _sourceName = o.getStringField("source");
        uassert( 10118 ,  "'host' field not set in sources collection object", !hostName.empty() );
        uassert( 10119 ,  "only source='main' allowed for now with replication", sourceName() == "main" );
        BSONElement e = o.getField("syncedTo");
        if ( !e.eoo() ) {
            uassert( 10120 ,  "bad sources 'syncedTo' field value", e.type() == Date || e.type() == Timestamp );
            OpTime tmp( e.date() );
            syncedTo = tmp;
        }

        BSONObj dbsObj = o.getObjectField("dbsNextPass");
        if ( !dbsObj.isEmpty() ) {
            BSONObjIterator i(dbsObj);
            while ( 1 ) {
                BSONElement e = i.next();
                if ( e.eoo() )
                    break;
                addDbNextPass.insert( e.fieldName() );
            }
        }

        dbsObj = o.getObjectField("incompleteCloneDbs");
        if ( !dbsObj.isEmpty() ) {
            BSONObjIterator i(dbsObj);
            while ( 1 ) {
                BSONElement e = i.next();
                if ( e.eoo() )
                    break;
                incompleteCloneDbs.insert( e.fieldName() );
            }
        }
    }

    /* Turn our C++ Source object into a BSONObj */
    BSONObj ReplSource::jsobj() {
        BSONObjBuilder b;
        b.append("host", hostName);
        b.append("source", sourceName());
        if ( !only.empty() )
            b.append("only", only);
        if ( !syncedTo.isNull() )
            b.appendTimestamp("syncedTo", syncedTo.asDate());

        BSONObjBuilder dbsNextPassBuilder;
        int n = 0;
        for ( set<string>::iterator i = addDbNextPass.begin(); i != addDbNextPass.end(); i++ ) {
            n++;
            dbsNextPassBuilder.appendBool(*i, 1);
        }
        if ( n )
            b.append("dbsNextPass", dbsNextPassBuilder.done());

        BSONObjBuilder incompleteCloneDbsBuilder;
        n = 0;
        for ( set<string>::iterator i = incompleteCloneDbs.begin(); i != incompleteCloneDbs.end(); i++ ) {
            n++;
            incompleteCloneDbsBuilder.appendBool(*i, 1);
        }
        if ( n )
            b.append("incompleteCloneDbs", incompleteCloneDbsBuilder.done());

        return b.obj();
    }

    void ReplSource::save() {
        BSONObjBuilder b;
        verify( !hostName.empty() );
        b.append("host", hostName);
        // todo: finish allowing multiple source configs.
        // this line doesn't work right when source is null, if that is allowed as it is now:
        //b.append("source", _sourceName);
        BSONObj pattern = b.done();

        BSONObj o = jsobj();
        log( 1 ) << "Saving repl source: " << o << endl;

        {
            OpDebug debug;
            Client::Context ctx("local.sources");
            UpdateResult res = updateObjects("local.sources", o, pattern, true/*upsert for pair feature*/, false,false,debug);
            verify( ! res.mod );
            verify( res.num == 1 );
        }
    }

    static void addSourceToList(ReplSource::SourceVector &v, ReplSource& s, ReplSource::SourceVector &old) {
        if ( !s.syncedTo.isNull() ) { // Don't reuse old ReplSource if there was a forced resync.
            for ( ReplSource::SourceVector::iterator i = old.begin(); i != old.end();  ) {
                if ( s == **i ) {
                    v.push_back(*i);
                    old.erase(i);
                    return;
                }
                i++;
            }
        }

        v.push_back( shared_ptr< ReplSource >( new ReplSource( s ) ) );
    }

    /* we reuse our existing objects so that we can keep our existing connection
       and cursor in effect.
    */
    void ReplSource::loadAll(SourceVector &v) {
        Client::Context ctx("local.sources");
        SourceVector old = v;
        v.clear();

        if ( !cmdLine.source.empty() ) {
            // --source <host> specified.
            // check that no items are in sources other than that
            // add if missing
            shared_ptr<Cursor> c = findTableScan("local.sources", BSONObj());
            int n = 0;
            while ( c->ok() ) {
                n++;
                ReplSource tmp(c->current());
                if ( tmp.hostName != cmdLine.source ) {
                    log() << "repl: --source " << cmdLine.source << " != " << tmp.hostName << " from local.sources collection" << endl;
                    log() << "repl: for instructions on changing this slave's source, see:" << endl;
                    log() << "http://dochub.mongodb.org/core/masterslave" << endl;
                    log() << "repl: terminating mongod after 30 seconds" << endl;
                    sleepsecs(30);
                    dbexit( EXIT_REPLICATION_ERROR );
                }
                if ( tmp.only != cmdLine.only ) {
                    log() << "--only " << cmdLine.only << " != " << tmp.only << " from local.sources collection" << endl;
                    log() << "terminating after 30 seconds" << endl;
                    sleepsecs(30);
                    dbexit( EXIT_REPLICATION_ERROR );
                }
                c->advance();
            }
            uassert( 10002 ,  "local.sources collection corrupt?", n<2 );
            if ( n == 0 ) {
                // source missing.  add.
                ReplSource s;
                s.hostName = cmdLine.source;
                s.only = cmdLine.only;
                s.save();
            }
        }
        else {
            try {
                massert( 10384 , "--only requires use of --source", cmdLine.only.empty());
            }
            catch ( ... ) {
                dbexit( EXIT_BADOPTIONS );
            }
        }

        shared_ptr<Cursor> c = findTableScan("local.sources", BSONObj());
        while ( c->ok() ) {
            ReplSource tmp(c->current());
            if ( tmp.syncedTo.isNull() ) {
                DBDirectClient c;
                if ( c.exists( "local.oplog.$main" ) ) {
                    BSONObj op = c.findOne( "local.oplog.$main", QUERY( "op" << NE << "n" ).sort( BSON( "$natural" << -1 ) ) );
                    if ( !op.isEmpty() ) {
                        tmp.syncedTo = op[ "ts" ].date();
                    }
                }
            }
            addSourceToList(v, tmp, old);
            c->advance();
        }
    }

    BSONObj opTimeQuery = fromjson("{\"getoptime\":1}");

    bool ReplSource::throttledForceResyncDead( const char *requester ) {
        if ( time( 0 ) - lastForcedResync > 600 ) {
            forceResyncDead( requester );
            lastForcedResync = time( 0 );
            return true;
        }
        return false;
    }

    void ReplSource::forceResyncDead( const char *requester ) {
        if ( !replAllDead )
            return;
        SourceVector sources;
        ReplSource::loadAll(sources);
        for( SourceVector::iterator i = sources.begin(); i != sources.end(); ++i ) {
            log() << requester << " forcing resync from "  << (*i)->hostName << endl;
            (*i)->forceResync( requester );
        }
        replAllDead = 0;
    }

    void ReplSource::forceResync( const char *requester ) {
        BSONObj info;
        {
            dbtemprelease t;
            if (!oplogReader.connect(hostName)) {
                msgassertedNoTrace( 14051 , "unable to connect to resync");
            }
            /* todo use getDatabaseNames() method here */
            bool ok = oplogReader.conn()->runCommand( "admin", BSON( "listDatabases" << 1 ), info );
            massert( 10385 ,  "Unable to get database list", ok );
        }
        BSONObjIterator i( info.getField( "databases" ).embeddedObject() );
        while( i.moreWithEOO() ) {
            BSONElement e = i.next();
            if ( e.eoo() )
                break;
            string name = e.embeddedObject().getField( "name" ).valuestr();
            if ( !e.embeddedObject().getBoolField( "empty" ) ) {
                if ( name != "local" ) {
                    if ( only.empty() || only == name ) {
                        resyncDrop( name.c_str(), requester );
                    }
                }
            }
        }
        syncedTo = OpTime();
        addDbNextPass.clear();
        save();
    }

    string ReplSource::resyncDrop( const char *db, const char *requester ) {
        log() << "resync: dropping database " << db << endl;
        Client::Context ctx(db);
        dropDatabase(db);
        return db;
    }

    /* grab initial copy of a database from the master */
    void ReplSource::resync(string db) {
        string dummyNs = resyncDrop( db.c_str(), "internal" );
        Client::Context ctx( dummyNs );
        {
            log() << "resync: cloning database " << db << " to get an initial copy" << endl;
            ReplInfo r("resync: cloning a database");
            string errmsg;
            int errCode = 0;
            bool ok = cloneFrom(hostName.c_str(), errmsg, cc().database()->name, false, /*slaveOk*/ true, /*replauth*/ true, /*snapshot*/false, /*mayYield*/true, /*mayBeInterrupted*/false, &errCode);
            if ( !ok ) {
                if ( errCode == DatabaseDifferCaseCode ) {
                    resyncDrop( db.c_str(), "internal" );
                    log() << "resync: database " << db << " not valid on the master due to a name conflict, dropping." << endl;
                    return;
                }
                else {
                    problem() << "resync of " << db << " from " << hostName << " failed " << errmsg << endl;
                    throw SyncException();
                }
            }
        }

        log() << "resync: done with initial clone for db: " << db << endl;

        return;
    }
    
    DatabaseIgnorer ___databaseIgnorer;
    
    void DatabaseIgnorer::doIgnoreUntilAfter( const string &db, const OpTime &futureOplogTime ) {
        if ( futureOplogTime > _ignores[ db ] ) {
            _ignores[ db ] = futureOplogTime;   
        }
    }

    bool DatabaseIgnorer::ignoreAt( const string &db, const OpTime &currentOplogTime ) {
        if ( _ignores[ db ].isNull() ) {
            return false;
        }
        if ( _ignores[ db ] >= currentOplogTime ) {
            return true;
        } else {
            // The ignore state has expired, so clear it.
            _ignores.erase( db );
            return false;
        }
    }

    bool ReplSource::handleDuplicateDbName( const BSONObj &op, const char *ns, const char *db ) {
        if ( dbHolder()._isLoaded( ns, dbpath ) ) {
            // Database is already present.
            return true;   
        }
        BSONElement ts = op.getField( "ts" );
        if ( ( ts.type() == Date || ts.type() == Timestamp ) && ___databaseIgnorer.ignoreAt( db, ts.date() ) ) {
            // Database is ignored due to a previous indication that it is
            // missing from master after optime "ts".
            return false;   
        }
        if ( Database::duplicateUncasedName( false, db, dbpath ).empty() ) {
            // No duplicate database names are present.
            return true;
        }
        
        OpTime lastTime;
        bool dbOk = false;
        {
            dbtemprelease release;
        
            // We always log an operation after executing it (never before), so
            // a database list will always be valid as of an oplog entry generated
            // before it was retrieved.
            
            BSONObj last = oplogReader.findOne( this->ns().c_str(), Query().sort( BSON( "$natural" << -1 ) ) );
            if ( !last.isEmpty() ) {
	            BSONElement ts = last.getField( "ts" );
	            massert( 14032, "Invalid 'ts' in remote log", ts.type() == Date || ts.type() == Timestamp );
	            lastTime = OpTime( ts.date() );
            }

            BSONObj info;
            bool ok = oplogReader.conn()->runCommand( "admin", BSON( "listDatabases" << 1 ), info );
            massert( 14033, "Unable to get database list", ok );
            BSONObjIterator i( info.getField( "databases" ).embeddedObject() );
            while( i.more() ) {
                BSONElement e = i.next();
            
                const char * name = e.embeddedObject().getField( "name" ).valuestr();
                if ( strcasecmp( name, db ) != 0 )
                    continue;
                
                if ( strcmp( name, db ) == 0 ) {
                    // The db exists on master, still need to check that no conflicts exist there.
                    dbOk = true;
                    continue;
                }
                
                // The master has a db name that conflicts with the requested name.
                dbOk = false;
                break;
            }
        }
        
        if ( !dbOk ) {
            ___databaseIgnorer.doIgnoreUntilAfter( db, lastTime );
            incompleteCloneDbs.erase(db);
            addDbNextPass.erase(db);
            return false;   
        }
        
        // Check for duplicates again, since we released the lock above.
        set< string > duplicates;
        Database::duplicateUncasedName( false, db, dbpath, &duplicates );
        
        // The database is present on the master and no conflicting databases
        // are present on the master.  Drop any local conflicts.
        for( set< string >::const_iterator i = duplicates.begin(); i != duplicates.end(); ++i ) {
            ___databaseIgnorer.doIgnoreUntilAfter( *i, lastTime );
            incompleteCloneDbs.erase(*i);
            addDbNextPass.erase(*i);
            Client::Context ctx(*i);
            dropDatabase(*i);
        }
        
        massert( 14034, "Duplicate database names present after attempting to delete duplicates",
                Database::duplicateUncasedName( false, db, dbpath ).empty() );
        return true;
    }

    void ReplSource::applyOperation(const BSONObj& op) {
        try {
            bool failedUpdate = applyOperation_inlock( op );
            if (failedUpdate) {
                Sync sync(hostName);
                if (sync.shouldRetry(op)) {
                    uassert(15914, "Failure retrying initial sync update", !applyOperation_inlock(op));
                }
            }
        }
        catch ( UserException& e ) {
            log() << "sync: caught user assertion " << e << " while applying op: " << op << endl;;
        }
        catch ( DBException& e ) {
            log() << "sync: caught db exception " << e << " while applying op: " << op << endl;;
        }

    }

    /* local.$oplog.main is of the form:
         { ts: ..., op: <optype>, ns: ..., o: <obj> , o2: <extraobj>, b: <boolflag> }
         ...
       see logOp() comments.

       @param alreadyLocked caller already put us in write lock if true
    */
    void ReplSource::sync_pullOpLog_applyOperation(BSONObj& op, bool alreadyLocked) {
        if( logLevel >= 6 ) // op.tostring is expensive so doing this check explicitly
            log(6) << "processing op: " << op << endl;

        if( op.getStringField("op")[0] == 'n' )
            return;

        char clientName[MaxDatabaseNameLen];
        const char *ns = op.getStringField("ns");
        nsToDatabase(ns, clientName);

        if ( *ns == '.' ) {
            problem() << "skipping bad op in oplog: " << op.toString() << endl;
            return;
        }
        else if ( *ns == 0 ) {
            /*if( op.getStringField("op")[0] != 'n' )*/ {
                problem() << "halting replication, bad op in oplog:\n  " << op.toString() << endl;
                replAllDead = "bad object in oplog";
                throw SyncException();
            }
            //ns = "local.system.x";
            //nsToDatabase(ns, clientName);
        }

        if ( !only.empty() && only != clientName )
            return;

        if( cmdLine.pretouch && !alreadyLocked/*doesn't make sense if in write lock already*/ ) {
            if( cmdLine.pretouch > 1 ) {
                /* note: this is bad - should be put in ReplSource.  but this is first test... */
                static int countdown;
                verify( countdown >= 0 );
                if( countdown > 0 ) {
                    countdown--; // was pretouched on a prev pass
                }
                else {
                    const int m = 4;
                    if( tp.get() == 0 ) {
                        int nthr = min(8, cmdLine.pretouch);
                        nthr = max(nthr, 1);
                        tp.reset( new ThreadPool(nthr) );
                    }
                    vector<BSONObj> v;
                    oplogReader.peek(v, cmdLine.pretouch);
                    unsigned a = 0;
                    while( 1 ) {
                        if( a >= v.size() ) break;
                        unsigned b = a + m - 1; // v[a..b]
                        if( b >= v.size() ) b = v.size() - 1;
                        tp->schedule(pretouchN, v, a, b);
                        DEV cout << "pretouch task: " << a << ".." << b << endl;
                        a += m;
                    }
                    // we do one too...
                    pretouchOperation(op);
                    tp->join();
                    countdown = v.size();
                }
            }
            else {
                pretouchOperation(op);
            }
        }

        scoped_ptr<Lock::GlobalWrite> lk( alreadyLocked ? 0 : new Lock::GlobalWrite() );

        if ( replAllDead ) {
            // hmmm why is this check here and not at top of this function? does it get set between top and here?
            log() << "replAllDead, throwing SyncException: " << replAllDead << endl;
            throw SyncException();
        }

        if ( !handleDuplicateDbName( op, ns, clientName ) ) {
            return;   
        }
                
        Client::Context ctx( ns );
        ctx.getClient()->curop()->reset();

        bool empty = ctx.db()->isEmpty();
        bool incompleteClone = incompleteCloneDbs.count( clientName ) != 0;

        if( logLevel >= 6 )
            log(6) << "ns: " << ns << ", justCreated: " << ctx.justCreated() << ", empty: " << empty << ", incompleteClone: " << incompleteClone << endl;

        // always apply admin command command
        // this is a bit hacky -- the semantics of replication/commands aren't well specified
        if ( strcmp( clientName, "admin" ) == 0 && *op.getStringField( "op" ) == 'c' ) {
            applyOperation( op );
            return;
        }

        if ( ctx.justCreated() || empty || incompleteClone ) {
            // we must add to incomplete list now that setClient has been called
            incompleteCloneDbs.insert( clientName );
            if ( nClonedThisPass ) {
                /* we only clone one database per pass, even if a lot need done.  This helps us
                 avoid overflowing the master's transaction log by doing too much work before going
                 back to read more transactions. (Imagine a scenario of slave startup where we try to
                 clone 100 databases in one pass.)
                 */
                addDbNextPass.insert( clientName );
            }
            else {
                if ( incompleteClone ) {
                    log() << "An earlier initial clone of '" << clientName << "' did not complete, now resyncing." << endl;
                }
                save();
                Client::Context ctx(ns);
                nClonedThisPass++;
                resync(ctx.db()->name);
                addDbNextPass.erase(clientName);
                incompleteCloneDbs.erase( clientName );
            }
            save();
        }
        else {
            applyOperation( op );
            addDbNextPass.erase( clientName );
        }
    }

    void ReplSource::syncToTailOfRemoteLog() {
        string _ns = ns();
        BSONObjBuilder b;
        if ( !only.empty() ) {
            b.appendRegex("ns", string("^") + pcrecpp::RE::QuoteMeta( only ));
        }
        BSONObj last = oplogReader.findOne( _ns.c_str(), Query( b.done() ).sort( BSON( "$natural" << -1 ) ) );
        if ( !last.isEmpty() ) {
            BSONElement ts = last.getField( "ts" );
            massert( 10386 ,  "non Date ts found: " + last.toString(), ts.type() == Date || ts.type() == Timestamp );
            syncedTo = OpTime( ts.date() );
        }
    }

    extern unsigned replApplyBatchSize;

    /* slave: pull some data from the master's oplog
       note: not yet in db mutex at this point.
       @return -1 error
               0 ok, don't sleep
               1 ok, sleep
    */
    int ReplSource::sync_pullOpLog(int& nApplied) {
        int okResultCode = 1;
        string ns = string("local.oplog.$") + sourceName();
        log(2) << "repl: sync_pullOpLog " << ns << " syncedTo:" << syncedTo.toStringLong() << '\n';

        bool tailing = true;
        oplogReader.tailCheck();

        bool initial = syncedTo.isNull();

        if ( !oplogReader.haveCursor() || initial ) {
            if ( initial ) {
                // Important to grab last oplog timestamp before listing databases.
                syncToTailOfRemoteLog();
                BSONObj info;
                bool ok = oplogReader.conn()->runCommand( "admin", BSON( "listDatabases" << 1 ), info );
                massert( 10389 ,  "Unable to get database list", ok );
                BSONObjIterator i( info.getField( "databases" ).embeddedObject() );
                while( i.moreWithEOO() ) {
                    BSONElement e = i.next();
                    if ( e.eoo() )
                        break;
                    string name = e.embeddedObject().getField( "name" ).valuestr();
                    if ( !e.embeddedObject().getBoolField( "empty" ) ) {
                        if ( name != "local" ) {
                            if ( only.empty() || only == name ) {
                                log( 2 ) << "adding to 'addDbNextPass': " << name << endl;
                                addDbNextPass.insert( name );
                            }
                        }
                    }
                }
                // obviously global isn't ideal, but non-repl set is old so 
                // keeping it simple
                Lock::GlobalWrite lk;
                save();
            }

            BSONObjBuilder q;
            q.appendDate("$gte", syncedTo.asDate());
            BSONObjBuilder query;
            query.append("ts", q.done());
            if ( !only.empty() ) {
                // note we may here skip a LOT of data table scanning, a lot of work for the master.
                // maybe append "\\." here?
                query.appendRegex("ns", string("^") + pcrecpp::RE::QuoteMeta( only ));
            }
            BSONObj queryObj = query.done();
            // e.g. queryObj = { ts: { $gte: syncedTo } }

            oplogReader.tailingQuery(ns.c_str(), queryObj);
            tailing = false;
        }
        else {
            log(2) << "repl: tailing=true\n";
        }

        if( !oplogReader.haveCursor() ) {
            problem() << "repl: dbclient::query returns null (conn closed?)" << endl;
            oplogReader.resetConnection();
            return -1;
        }

        // show any deferred database creates from a previous pass
        {
            set<string>::iterator i = addDbNextPass.begin();
            if ( i != addDbNextPass.end() ) {
                BSONObjBuilder b;
                b.append("ns", *i + '.');
                b.append("op", "db");
                BSONObj op = b.done();
                sync_pullOpLog_applyOperation(op, false);
            }
        }

        if ( !oplogReader.more() ) {
            if ( tailing ) {
                log(2) << "repl: tailing & no new activity\n";
                if( oplogReader.awaitCapable() )
                    okResultCode = 0; // don't sleep

            }
            else {
                log() << "repl:   " << ns << " oplog is empty\n";
            }
            {
                Lock::GlobalWrite lk;
                save();
            }
            return okResultCode;
        }

        OpTime nextOpTime;
        {
            BSONObj op = oplogReader.next();
            BSONElement ts = op.getField("ts");
            if ( ts.type() != Date && ts.type() != Timestamp ) {
                string err = op.getStringField("$err");
                if ( !err.empty() ) {
                    // 13051 is "tailable cursor requested on non capped collection"
                    if (op.getIntField("code") == 13051) {
                        problem() << "trying to slave off of a non-master" << '\n';
                        massert( 13344 ,  "trying to slave off of a non-master", false );
                    }
                    else {
                        problem() << "repl: $err reading remote oplog: " + err << '\n';
                        massert( 10390 ,  "got $err reading remote oplog", false );
                    }
                }
                else {
                    problem() << "repl: bad object read from remote oplog: " << op.toString() << '\n';
                    massert( 10391 , "repl: bad object read from remote oplog", false);
                }
            }

            nextOpTime = OpTime( ts.date() );
            log(2) << "repl: first op time received: " << nextOpTime.toString() << '\n';
            if ( initial ) {
                log(1) << "repl:   initial run\n";
            }
            if( tailing ) {
                if( !( syncedTo < nextOpTime ) ) {
                    log() << "repl ASSERTION failed : syncedTo < nextOpTime" << endl;
                    log() << "repl syncTo:     " << syncedTo.toStringLong() << endl;
                    log() << "repl nextOpTime: " << nextOpTime.toStringLong() << endl;
                    verify(false);
                }
                oplogReader.putBack( op ); // op will be processed in the loop below
                nextOpTime = OpTime(); // will reread the op below
            }
            else if ( nextOpTime != syncedTo ) { // didn't get what we queried for - error
                Nullstream& l = log();
                l << "repl:   nextOpTime " << nextOpTime.toStringLong() << ' ';
                if ( nextOpTime < syncedTo )
                    l << "<??";
                else
                    l << ">";

                l << " syncedTo " << syncedTo.toStringLong() << '\n';
                log() << "repl:   time diff: " << (nextOpTime.getSecs() - syncedTo.getSecs()) << "sec\n";
                log() << "repl:   tailing: " << tailing << '\n';
                log() << "repl:   data too stale, halting replication" << endl;
                replInfo = replAllDead = "data too stale halted replication";
                verify( syncedTo < nextOpTime );
                throw SyncException();
            }
            else {
                /* t == syncedTo, so the first op was applied previously or it is the first op of initial query and need not be applied. */
            }
        }

        // apply operations
        {
            int n = 0;
            time_t saveLast = time(0);
            while ( 1 ) {

                bool moreInitialSyncsPending = !addDbNextPass.empty() && n; // we need "&& n" to assure we actually process at least one op to get a sync point recorded in the first place.

                if ( moreInitialSyncsPending || !oplogReader.more() ) {
                    Lock::GlobalWrite lk;

                    // NOTE aaron 2011-03-29 This block may be unnecessary, but I'm leaving it in place to avoid changing timing behavior.
                    {
                        dbtemprelease t;
                        if ( !moreInitialSyncsPending && oplogReader.more() ) {
                            continue;
                        }
                        // otherwise, break out of loop so we can set to completed or clone more dbs
                    }
                    
                    if( oplogReader.awaitCapable() && tailing )
                        okResultCode = 0; // don't sleep
                    syncedTo = nextOpTime;
                    save(); // note how far we are synced up to now
                    log() << "repl:   applied " << n << " operations" << endl;
                    nApplied = n;
                    log() << "repl:  end sync_pullOpLog syncedTo: " << syncedTo.toStringLong() << endl;
                    break;
                }
                else {
                }

                OCCASIONALLY if( n > 0 && ( n > 100000 || time(0) - saveLast > 60 ) ) {
                    // periodically note our progress, in case we are doing a lot of work and crash
                    Lock::GlobalWrite lk;
                    syncedTo = nextOpTime;
                    // can't update local log ts since there are pending operations from our peer
                    save();
                    log() << "repl:   checkpoint applied " << n << " operations" << endl;
                    log() << "repl:   syncedTo: " << syncedTo.toStringLong() << endl;
                    saveLast = time(0);
                    n = 0;
                }

                BSONObj op = oplogReader.next();

                unsigned b = replApplyBatchSize;
                bool justOne = b == 1;
                scoped_ptr<Lock::GlobalWrite> lk( justOne ? 0 : new Lock::GlobalWrite() );
                while( 1 ) {

                    BSONElement ts = op.getField("ts");
                    if( !( ts.type() == Date || ts.type() == Timestamp ) ) {
                        log() << "sync error: problem querying remote oplog record" << endl;
                        log() << "op: " << op.toString() << endl;
                        log() << "halting replication" << endl;
                        replInfo = replAllDead = "sync error: no ts found querying remote oplog record";
                        throw SyncException();
                    }
                    OpTime last = nextOpTime;
                    nextOpTime = OpTime( ts.date() );
                    if ( !( last < nextOpTime ) ) {
                        log() << "sync error: last applied optime at slave >= nextOpTime from master" << endl;
                        log() << " last:       " << last.toStringLong() << endl;
                        log() << " nextOpTime: " << nextOpTime.toStringLong() << endl;
                        log() << " halting replication" << endl;
                        replInfo = replAllDead = "sync error last >= nextOpTime";
                        uassert( 10123 , "replication error last applied optime at slave >= nextOpTime from master", false);
                    }
                    if ( replSettings.slavedelay && ( unsigned( time( 0 ) ) < nextOpTime.getSecs() + replSettings.slavedelay ) ) {
                        verify( justOne );
                        oplogReader.putBack( op );
                        _sleepAdviceTime = nextOpTime.getSecs() + replSettings.slavedelay + 1;
                        Lock::GlobalWrite lk;
                        if ( n > 0 ) {
                            syncedTo = last;
                            save();
                        }
                        log() << "repl:   applied " << n << " operations" << endl;
                        log() << "repl:   syncedTo: " << syncedTo.toStringLong() << endl;
                        log() << "waiting until: " << _sleepAdviceTime << " to continue" << endl;
                        return okResultCode;
                    }

                    sync_pullOpLog_applyOperation(op, !justOne);
                    n++;

                    if( --b == 0 )
                        break;
                    // if to here, we are doing mulpile applications in a singel write lock acquisition
                    if( !oplogReader.moreInCurrentBatch() ) {
                        // break if no more in batch so we release lock while reading from the master
                        break;
                    }
                    op = oplogReader.next();

                    getDur().commitIfNeeded();
                }
            }
        }

        return okResultCode;
    }

    BSONObj userReplQuery = fromjson("{\"user\":\"repl\"}");

    bool replAuthenticate(DBClientBase *conn) {
        if( noauth ) {
            return true;
        }
        if( ! cc().isAdmin() ) {
            log() << "replauthenticate: requires admin permissions, failing\n";
            return false;
        }

        string u;
        string p;
        if (internalSecurity.pwd.length() > 0) {
            u = internalSecurity.user;
            p = internalSecurity.pwd;
        }
        else {
            BSONObj user;
            {
                Lock::GlobalWrite lk;
                Client::Context ctxt("local.");
                if( !Helpers::findOne("local.system.users", userReplQuery, user) ||
                        // try the first user in local
                        !Helpers::getSingleton("local.system.users", user) ) {
                    log() << "replauthenticate: no user in local.system.users to use for authentication\n";
                    return false;
                }
            }
            u = user.getStringField("user");
            p = user.getStringField("pwd");
            massert( 10392 , "bad user object? [1]", !u.empty());
            massert( 10393 , "bad user object? [2]", !p.empty());
        }

        string err;
        if( !conn->auth("local", u.c_str(), p.c_str(), err, false) ) {
            log() << "replauthenticate: can't authenticate to master server, user:" << u << endl;
            return false;
        }
        return true;
    }

    bool replHandshake(DBClientConnection *conn) {
        string myname = getHostName();

        BSONObj me;
        {
            
            Lock::GlobalWrite l;
            // local.me is an identifier for a server for getLastError w:2+
            if ( ! Helpers::getSingleton( "local.me" , me ) ||
                 ! me.hasField("host") ||
                 me["host"].String() != myname ) {

                // clean out local.me
                Helpers::emptyCollection("local.me");

                // repopulate
                BSONObjBuilder b;
                b.appendOID( "_id" , 0 , true );
                b.append( "host", myname );
                me = b.obj();
                Helpers::putSingleton( "local.me" , me );
            }
        }

        BSONObjBuilder cmd;
        cmd.appendAs( me["_id"] , "handshake" );
        if (theReplSet) {
            cmd.append("member", theReplSet->selfId());
        }

        BSONObj res;
        bool ok = conn->runCommand( "admin" , cmd.obj() , res );
        // ignoring for now on purpose for older versions
        log(ok) << "replHandshake res not: " << ok << " res: " << res << endl;
        return true;
    }

    OplogReader::OplogReader( bool doHandshake ) : 
        _doHandshake( doHandshake ) { 
        
        _tailingQueryOptions = QueryOption_SlaveOk;
        _tailingQueryOptions |= QueryOption_CursorTailable | QueryOption_OplogReplay;
        
        /* TODO: slaveOk maybe shouldn't use? */
        _tailingQueryOptions |= QueryOption_AwaitData;
    }

    bool OplogReader::commonConnect(const string& hostName) {
        if( conn() == 0 ) {
            _conn = shared_ptr<DBClientConnection>(new DBClientConnection( false, 0, 60 /* tcp timeout */));
            string errmsg;
            ReplInfo r("trying to connect to sync source");
            if ( !_conn->connect(hostName.c_str(), errmsg) ||
                 (!noauth && !replAuthenticate(_conn.get())) ) {
                resetConnection();
                log() << "repl: " << errmsg << endl;
                return false;
            }
        }
        return true;
    }
    
    bool OplogReader::connect(string hostName) {
        if (conn() != 0) {
            return true;
        }

        if ( ! commonConnect(hostName) ) {
            return false;
        }
        
        
        if ( _doHandshake && ! replHandshake(_conn.get() ) ) {
            return false;
        }

        return true;
    }

    bool OplogReader::connect(const BSONObj& rid, const int from, const string& to) {
        if (conn() != 0) {
            return true;
        }
        if (commonConnect(to)) {
            log() << "handshake between " << from << " and " << to << endl;
            return passthroughHandshake(rid, from);
        }
        return false;
    }

    bool OplogReader::passthroughHandshake(const BSONObj& rid, const int f) {
        BSONObjBuilder cmd;
        cmd.appendAs( rid["_id"], "handshake" );
        cmd.append( "member" , f );

        BSONObj res;
        return conn()->runCommand( "admin" , cmd.obj() , res );
    }

    void OplogReader::tailingQuery(const char *ns, const BSONObj& query, const BSONObj* fields ) {
        verify( !haveCursor() );
        LOG(2) << "repl: " << ns << ".find(" << query.toString() << ')' << endl;
        cursor.reset( _conn->query( ns, query, 0, 0, fields, _tailingQueryOptions ).release() );
    }
    
    void OplogReader::tailingQueryGTE(const char *ns, OpTime t, const BSONObj* fields ) {
        BSONObjBuilder q;
        q.appendDate("$gte", t.asDate());
        BSONObjBuilder query;
        query.append("ts", q.done());
        tailingQuery(ns, query.done(), fields);
    }


    /* note: not yet in mutex at this point.
       returns >= 0 if ok.  return -1 if you want to reconnect.
       return value of zero indicates no sleep necessary before next call
    */
    int ReplSource::sync(int& nApplied) {
        _sleepAdviceTime = 0;
        ReplInfo r("sync");
        if ( !cmdLine.quiet ) {
            Nullstream& l = log();
            l << "repl: syncing from ";
            if( sourceName() != "main" ) {
                l << "source:" << sourceName() << ' ';
            }
            l << "host:" << hostName << endl;
        }
        nClonedThisPass = 0;

        // FIXME Handle cases where this db isn't on default port, or default port is spec'd in hostName.
        if ( (string("localhost") == hostName || string("127.0.0.1") == hostName) && cmdLine.port == CmdLine::DefaultDBPort ) {
            log() << "repl:   can't sync from self (localhost). sources configuration may be wrong." << endl;
            sleepsecs(5);
            return -1;
        }

        if ( !oplogReader.connect(hostName) ) {
            log(4) << "repl:  can't connect to sync source" << endl;
            return -1;
        }

        /*
            // get current mtime at the server.
            BSONObj o = conn->findOne("admin.$cmd", opTimeQuery);
            BSONElement e = o.getField("optime");
            if( e.eoo() ) {
                log() << "repl:   failed to get cur optime from master" << endl;
                log() << "        " << o.toString() << endl;
                return false;
            }
            uassert( 10124 ,  e.type() == Date );
            OpTime serverCurTime;
            serverCurTime.asDate() = e.date();
        */
        return sync_pullOpLog(nApplied);
    }

    /* --------------------------------------------------------------*/

    /*
    TODO:
    _ source has autoptr to the cursor
    _ reuse that cursor when we can
    */

    /* returns: # of seconds to sleep before next pass
                0 = no sleep recommended
                1 = special sentinel indicating adaptive sleep recommended
    */
    int _replMain(ReplSource::SourceVector& sources, int& nApplied) {
        {
            ReplInfo r("replMain load sources");
            Lock::GlobalWrite lk;
            ReplSource::loadAll(sources);
            replSettings.fastsync = false; // only need this param for initial reset
        }

        if ( sources.empty() ) {
            /* replication is not configured yet (for --slave) in local.sources.  Poll for config it
            every 20 seconds.
            */
            log() << "no source given, add a master to local.sources to start replication" << endl;
            return 20;
        }

        int sleepAdvice = 1;
        for ( ReplSource::SourceVector::iterator i = sources.begin(); i != sources.end(); i++ ) {
            ReplSource *s = i->get();
            int res = -1;
            try {
                res = s->sync(nApplied);
                bool moreToSync = s->haveMoreDbsToSync();
                if( res < 0 ) {
                    sleepAdvice = 3;
                }
                else if( moreToSync ) {
                    sleepAdvice = 0;
                }
                else if ( s->sleepAdvice() ) {
                    sleepAdvice = s->sleepAdvice();
                }
                else
                    sleepAdvice = res;
            }
            catch ( const SyncException& ) {
                log() << "caught SyncException" << endl;
                return 10;
            }
            catch ( AssertionException& e ) {
                if ( e.severe() ) {
                    log() << "replMain AssertionException " << e.what() << endl;
                    return 60;
                }
                else {
                    log() << "repl: AssertionException " << e.what() << '\n';
                }
                replInfo = "replMain caught AssertionException";
            }
            catch ( const DBException& e ) {
                log() << "repl: DBException " << e.what() << endl;
                replInfo = "replMain caught DBException";
            }
            catch ( const std::exception &e ) {
                log() << "repl: std::exception " << e.what() << endl;
                replInfo = "replMain caught std::exception";
            }
            catch ( ... ) {
                log() << "unexpected exception during replication.  replication will halt" << endl;
                replAllDead = "caught unexpected exception during replication";
            }
            if ( res < 0 )
                s->oplogReader.resetConnection();
        }
        return sleepAdvice;
    }

    void replMain() {
        ReplSource::SourceVector sources;
        while ( 1 ) {
            int s = 0;
            {
                Lock::GlobalWrite lk;
                if ( replAllDead ) {
                    // throttledForceResyncDead can throw
                    if ( !replSettings.autoresync || !ReplSource::throttledForceResyncDead( "auto" ) ) {
                        log() << "all sources dead: " << replAllDead << ", sleeping for 5 seconds" << endl;
                        break;
                    }
                }
                verify( syncing == 0 ); // i.e., there is only one sync thread running. we will want to change/fix this.
                syncing++;
            }
            try {
                int nApplied = 0;
                s = _replMain(sources, nApplied);
                if( s == 1 ) {
                    if( nApplied == 0 ) s = 2;
                    else if( nApplied > 100 ) {
                        // sleep very little - just enought that we aren't truly hammering master
                        sleepmillis(75);
                        s = 0;
                    }
                }
            }
            catch (...) {
                out() << "caught exception in _replMain" << endl;
                s = 4;
            }
            {
                Lock::GlobalWrite lk;
                verify( syncing == 1 );
                syncing--;
            }

            if( relinquishSyncingSome )  {
                relinquishSyncingSome = 0;
                s = 1; // sleep before going back in to syncing=1
            }

            if ( s ) {
                stringstream ss;
                ss << "repl: sleep " << s << " sec before next pass";
                string msg = ss.str();
                if ( ! cmdLine.quiet )
                    log() << msg << endl;
                ReplInfo r(msg.c_str());
                sleepsecs(s);
            }
        }
    }

    static void replMasterThread() {
        sleepsecs(4);
        Client::initThread("replmaster");
        int toSleep = 10;
        while( 1 ) {

            sleepsecs( toSleep );
            /* write a keep-alive like entry to the log.  this will make things like
               printReplicationStatus() and printSlaveReplicationStatus() stay up-to-date
               even when things are idle.
            */
            {
                writelocktry lk(1);
                if ( lk.got() ) {
                    toSleep = 10;

                    replLocalAuth();

                    try {
                        logKeepalive();
                    }
                    catch(...) {
                        log() << "caught exception in replMasterThread()" << endl;
                    }
                }
                else {
                    log(5) << "couldn't logKeepalive" << endl;
                    toSleep = 1;
                }
            }
        }
    }

    void replSlaveThread() {
        sleepsecs(1);
        Client::initThread("replslave");
        cc().iAmSyncThread();

        {
            Lock::GlobalWrite lk;
            replLocalAuth();
        }

        while ( 1 ) {
            try {
                replMain();
                sleepsecs(5);
            }
            catch ( AssertionException& ) {
                ReplInfo r("Assertion in replSlaveThread(): sleeping 5 minutes before retry");
                problem() << "Assertion in replSlaveThread(): sleeping 5 minutes before retry" << endl;
                sleepsecs(300);
            }
            catch ( DBException& e ) {
                problem() << "exception in replSlaveThread(): " << e.what()
                          << ", sleeping 5 minutes before retry" << endl;
                sleepsecs(300);
            }
            catch ( ... ) {
                problem() << "error in replSlaveThread(): sleeping 5 minutes before retry" << endl;
                sleepsecs(300);
            }
        }
    }

    void tempThread() {
        while ( 1 ) {
            out() << d.dbMutex.info().isLocked() << endl;
            sleepmillis(100);
        }
    }

    void newRepl();
    void oldRepl();
    void startReplSets(ReplSetCmdline*);
    void startReplication() {
        /* if we are going to be a replica set, we aren't doing other forms of replication. */
        if( !cmdLine._replSet.empty() ) {
            if( replSettings.slave || replSettings.master ) {
                log() << "***" << endl;
                log() << "ERROR: can't use --slave or --master replication options with --replSet" << endl;
                log() << "***" << endl;
            }
            newRepl();

            replSet = true;
            ReplSetCmdline *replSetCmdline = new ReplSetCmdline(cmdLine._replSet);
            boost::thread t( boost::bind( &startReplSets, replSetCmdline) );

            return;
        }

        oldRepl();

        /* this was just to see if anything locks for longer than it should -- we need to be careful
           not to be locked when trying to connect() or query() the other side.
           */
        //boost::thread tempt(tempThread);

        if( !replSettings.slave && !replSettings.master )
            return;

        {
            Lock::GlobalWrite lk;
            replLocalAuth();
        }

        if ( replSettings.slave ) {
            verify( replSettings.slave == SimpleSlave );
            log(1) << "slave=true" << endl;
            boost::thread repl_thread(replSlaveThread);
        }

        if ( replSettings.master ) {
            log(1) << "master=true" << endl;
            replSettings.master = true;
            createOplog();
            boost::thread t(replMasterThread);
        }

        while( replSettings.fastsync ) // don't allow writes until we've set up from log
            sleepmillis( 50 );
    }

    void testPretouch() {
        int nthr = min(8, 8);
        nthr = max(nthr, 1);
        int m = 8 / nthr;
        ThreadPool tp(nthr);
        vector<BSONObj> v;

        BSONObj x = BSON( "ns" << "test.foo" << "o" << BSON( "_id" << 1 ) << "op" << "i" );

        v.push_back(x);
        v.push_back(x);
        v.push_back(x);

        unsigned a = 0;
        while( 1 ) {
            if( a >= v.size() ) break;
            unsigned b = a + m - 1; // v[a..b]
            if( b >= v.size() ) b = v.size() - 1;
            tp.schedule(pretouchN, v, a, b);
            DEV cout << "pretouch task: " << a << ".." << b << endl;
            a += m;
        }
        tp.join();
    }

    class ReplApplyBatchSizeValidator : public ParameterValidator {
    public:
        ReplApplyBatchSizeValidator() : ParameterValidator( "replApplyBatchSize" ) {}

        virtual bool isValid( BSONElement e , string& errmsg ) const {
            int b = e.numberInt();
            if( b < 1 || b > 1024 ) {
                errmsg = "replApplyBatchSize has to be >= 1 and < 1024";
                return false;
            }

            if ( replSettings.slavedelay != 0 && b > 1 ) {
                errmsg = "can't use a batch size > 1 with slavedelay";
                return false;
            }
            if ( ! replSettings.slave ) {
                errmsg = "can't set replApplyBatchSize on a non-slave machine";
                return false;
            }

            return true;
        }
    } replApplyBatchSizeValidator;

} // namespace mongo
