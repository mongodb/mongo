// repl.cpp

/* TODO

   PAIRING
    _ on a syncexception, don't allow going back to master state?

*/

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
   local.dbinfo.<dbname>
   local.pair.startup    - can contain a special value indicating for a pair that we have the master copy.
                           used when replacing other half of the pair which has permanently failed.
   local.pair.sync       - { initialsynccomplete: 1 }
*/

#include "stdafx.h"
#include "jsobj.h"
#include "../util/goodies.h"
#include "repl.h"
#include "../util/message.h"
#include "../client/dbclient.h"
#include "pdfile.h"
#include "query.h"
#include "db.h"
#include "commands.h"
#include "security.h"
#include "cmdline.h"

namespace mongo {

    void ensureHaveIdIndex(const char *ns);

    /* if 1 sync() is running */
    int syncing = 0;

    /* if true replace our peer in a replication pair -- don't worry about if his
       local.oplog.$main is empty.
    */
    bool replacePeer = false;

    /* "dead" means something really bad happened like replication falling completely out of sync.
       when non-null, we are dead and the string is informational
    */
    const char *replAllDead = 0;

    extern bool autoresync;
    time_t lastForcedResync = 0;
    
    IdTracker &idTracker = *( new IdTracker() );
    
} // namespace mongo

#include "replset.h"

namespace mongo {

    PairSync *pairSync = new PairSync();
    bool getInitialSyncCompleted() {
        return pairSync->initialSyncCompleted();
    }

    /* --- ReplPair -------------------------------- */

    ReplPair *replPair = 0;

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

    void ReplPair::setMaster(int n, const char *_comment ) {
        if ( n == State_Master && !getInitialSyncCompleted() )
            return;
        info = _comment;
        if ( n != state && !cmdLine.quiet )
            log() << "pair: setting master=" << n << " was " << state << '\n';
        state = n;
    }

    /* peer unreachable, try our arbiter */
    void ReplPair::arbitrate() {
        ReplInfo r("arbitrate");

        if ( arbHost == "-" ) {
            // no arbiter. we are up, let's assume partner is down and network is not partitioned.
            setMasterLocked(State_Master, "remote unreachable");
            return;
        }

        auto_ptr<DBClientConnection> conn( newClientConnection() );
        string errmsg;
        if ( !conn->connect(arbHost.c_str(), errmsg) ) {
            log() << "repl:   cantconn arbiter " << errmsg << endl;
            setMasterLocked(State_CantArb, "can't connect to arb");
            return;
        }

        negotiate( conn.get(), "arbiter" );
    }

    /* --------------------------------------------- */

    class CmdReplacePeer : public Command {
    public:
        virtual bool slaveOk() {
            return true;
        }
        virtual bool adminOnly() {
            return true;
        }
        virtual bool logTheOp() {
            return false;
        }
        CmdReplacePeer() : Command("replacepeer") { }
        virtual bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            if ( replPair == 0 ) {
                errmsg = "not paired";
                return false;
            }
            if ( !getInitialSyncCompleted() ) {
                errmsg = "not caught up cannot replace peer";
                return false;
            }
            if ( syncing < 0 ) {
                errmsg = "replacepeer already invoked";
                return false;
            }
            Timer t;
            while ( 1 ) {
                if ( syncing == 0 || t.millis() > 20000 )
                    break;
                {
                    dbtemprelease t;
                    sleepmillis(10);
                }
            }
            if ( syncing ) {
                assert( syncing > 0 );
                errmsg = "timeout waiting for sync() to finish";
                return false;
            }
            {
                ReplSource::SourceVector sources;
                ReplSource::loadAll(sources);
                if ( sources.size() != 1 ) {
                    errmsg = "local.sources.count() != 1, cannot replace peer";
                    return false;
                }
            }
            {
                Helpers::emptyCollection("local.sources");
                BSONObj o = fromjson("{\"replacepeer\":1}");
                Helpers::putSingleton("local.pair.startup", o);
            }
            syncing = -1;
            replAllDead = "replacepeer invoked -- adjust local.sources hostname then restart this db process";
            result.append("info", "adjust local.sources hostname; db restart now required");
            return true;
        }
    } cmdReplacePeer;

    class CmdForceDead : public Command {
    public:
        virtual bool slaveOk() {
            return true;
        }
        virtual bool adminOnly() {
            return true;
        }
        virtual bool logTheOp() {
            return false;   
        }
        CmdForceDead() : Command("forcedead") { }
        virtual bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            replAllDead = "forced by command";
            return true;
        }
    } cmdForceDead;
    
    /* operator requested resynchronization of replication (on the slave).  { resync : 1 } */
    class CmdResync : public Command {
    public:
        virtual bool slaveOk() {
            return true;
        }
        virtual bool adminOnly() {
            return true;
        }
        virtual bool logTheOp() {
            return false;
        }
        CmdResync() : Command("resync") { }
        virtual bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
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
                if ( syncing == 0 || t.millis() > 20000 )
                    break;
                {
                    dbtemprelease t;
                    sleepmillis(10);
                }
            }
            if ( syncing ) {
                errmsg = "timeout waiting for sync() to finish";
                return false;
            }
            return true;
        }
    } cmdResync;
    
    class CmdIsMaster : public Command {
    public:
        virtual bool requiresAuth() { return false; }
        virtual bool slaveOk() {
            return true;
        }
        CmdIsMaster() : Command("ismaster") { }
        virtual bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool /*fromRepl*/) {
			/* currently request to arbiter is (somewhat arbitrarily) an ismaster request that is not 
			   authenticated.
			   we allow unauthenticated ismaster but we aren't as verbose informationally if 
			   one is not authenticated for admin db to be safe.
			*/
            AuthenticationInfo *ai = currentClient.get()->ai;
			bool authed = ai->isAuthorized("admin");

            if ( replAllDead ) {
                result.append("ismaster", 0.0);
				if( authed ) { 
					if ( replPair )
						result.append("remote", replPair->remote);
					result.append("info", replAllDead);
				}
            }
            else if ( replPair ) {
                result.append("ismaster", replPair->state);
				if( authed ) {
					result.append("remote", replPair->remote);
					if ( !replPair->info.empty() )
						result.append("info", replPair->info);
				}
			}
            else {
                result.append("ismaster", slave ? 0 : 1);
				result.append("msg", "not paired");
            }
            
            return true;
        }
    } cmdismaster;

    class CmdIsInitialSyncComplete : public Command {
    public:
        virtual bool requiresAuth() { return false; }
        virtual bool slaveOk() {
            return true;
        }
        CmdIsInitialSyncComplete() : Command( "isinitialsynccomplete" ) {}
        virtual bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool /*fromRepl*/) {
            result.appendBool( "initialsynccomplete", getInitialSyncCompleted() );
            return true;
        }
    } cmdisinitialsynccomplete;
    
    /* negotiate who is master

       -1=not set (probably means we just booted)
        0=was slave
        1=was master

       remote,local -> new remote,local
       !1,1  -> 0,1
       1,!1  -> 1,0
       -1,-1 -> dominant->1, nondom->0
       0,0   -> dominant->1, nondom->0
       1,1   -> dominant->1, nondom->0

       { negotiatemaster:1, i_was:<state>, your_name:<hostname> }
       returns:
       { ok:1, you_are:..., i_am:... }
    */
    class CmdNegotiateMaster : public Command {
    public:
        CmdNegotiateMaster() : Command("negotiatemaster") { }
        virtual bool slaveOk() {
            return true;
        }
        virtual bool adminOnly() {
            return true;
        }

        virtual bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool) {
            if ( replPair == 0 ) {
                massert( "Another mongod instance believes incorrectly that this node is its peer", !cmdObj.getBoolField( "fromArbiter" ) );
                // assume that we are an arbiter and should forward the request
                string host = cmdObj.getStringField("your_name");
                int port = cmdObj.getIntField( "your_port" );
                if ( port == INT_MIN ) {
                    errmsg = "no port specified";
                    problem() << errmsg << endl;
                    return false;
                }
                stringstream ss;
                ss << host << ":" << port;
                string remote = ss.str();
                BSONObj ret;
                {
                    dbtemprelease t;
                    auto_ptr<DBClientConnection> conn( new DBClientConnection() );
                    if ( !conn->connect( remote.c_str(), errmsg ) ) {
                        result.append( "you_are", ReplPair::State_Master );
                        return true;
                    }
                    BSONObjBuilder forwardCommand;
                    forwardCommand.appendElements( cmdObj );
                    forwardCommand.appendBool( "fromArbiter", true );
                    ret = conn->findOne( "admin.$cmd", forwardCommand.done() );
                }
                BSONObjIterator i( ret );
                while( i.moreWithEOO() ) {
                    BSONElement e = i.next();
                    if ( e.eoo() )
                        break;
                    if ( e.fieldName() != string( "ok" ) )
                        result.append( e );
                }
                return ( ret.getIntField("ok") == 1 );
            }

            int was = cmdObj.getIntField("i_was");
            string myname = cmdObj.getStringField("your_name");
            if ( myname.empty() || was < -3 ) {
                errmsg = "your_name/i_was not specified";
                return false;
            }

            int N = ReplPair::State_Negotiating;
            int M = ReplPair::State_Master;
            int S = ReplPair::State_Slave;

            if ( !replPair->dominant( myname ) ) {
                result.append( "you_are", N );
                result.append( "i_am", replPair->state );
                return true;
            }

            int me, you;
            if ( !getInitialSyncCompleted() || ( replPair->state != M && was == M ) ) {
                me=S;
                you=M;
            }
            else {
                me=M;
                you=S;
            }
            replPair->setMaster( me, "CmdNegotiateMaster::run()" );

            result.append("you_are", you);
            result.append("i_am", me);

            return true;
        }
    } cmdnegotiatemaster;
    
    int ReplPair::negotiate(DBClientConnection *conn, string method) {
        BSONObjBuilder b;
        b.append("negotiatemaster",1);
        b.append("i_was", state);
        b.append("your_name", remoteHost);
        b.append("your_port", remotePort);
        BSONObj cmd = b.done();
        BSONObj res = conn->findOne("admin.$cmd", cmd);
        if ( res.getIntField("ok") != 1 ) {
            string message = method + " negotiate failed";
            problem() << message << ": " << res.toString() << '\n';
            setMasterLocked(State_Confused, message.c_str());
            return State_Confused;
        }
        int x = res.getIntField("you_are");
        int remote = res.getIntField("i_am");
        // State_Negotiating means the remote node is not dominant and cannot
        // choose who is master.
        if ( x != State_Slave && x != State_Master && x != State_Negotiating ) {
            problem() << method << " negotiate: bad you_are value " << res.toString() << endl;
        } else if ( x != State_Negotiating ) {
            string message = method + " negotiation";
            setMasterLocked(x, message.c_str());
        }
        return remote;
    }

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

    /* --------------------------------------------------------------*/

    ReplSource::ReplSource() {
        replacing = false;
        nClonedThisPass = 0;
        paired = false;
    }

    ReplSource::ReplSource(BSONObj o) : nClonedThisPass(0) {
        replacing = false;
        paired = false;
        only = o.getStringField("only");
        hostName = o.getStringField("host");
        _sourceName = o.getStringField("source");
        uassert( "'host' field not set in sources collection object", !hostName.empty() );
        uassert( "only source='main' allowed for now with replication", sourceName() == "main" );
        BSONElement e = o.getField("syncedTo");
        if ( !e.eoo() ) {
            uassert( "bad sources 'syncedTo' field value", e.type() == Date || e.type() == Timestamp );
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

        lastSavedLocalTs_ = OpTime( o.getField( "localLogTs" ).date() );
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

        b.appendTimestamp("localLogTs", lastSavedLocalTs_.asDate());
        
        BSONObjBuilder dbsNextPassBuilder;
        int n = 0;
        for ( set<string>::iterator i = addDbNextPass.begin(); i != addDbNextPass.end(); i++ ) {
            n++;
            dbsNextPassBuilder.appendBool(i->c_str(), 1);
        }
        if ( n )
            b.append("dbsNextPass", dbsNextPassBuilder.done());

        BSONObjBuilder incompleteCloneDbsBuilder;
        n = 0;
        for ( set<string>::iterator i = incompleteCloneDbs.begin(); i != incompleteCloneDbs.end(); i++ ) {
            n++;
            incompleteCloneDbsBuilder.appendBool(i->c_str(), 1);
        }
        if ( n )
            b.append("incompleteCloneDbs", incompleteCloneDbsBuilder.done());

        return b.obj();
    }

    void ReplSource::save() {
        BSONObjBuilder b;
        assert( !hostName.empty() );
        b.append("host", hostName);
        // todo: finish allowing multiple source configs.
        // this line doesn't work right when source is null, if that is allowed as it is now:
        //b.append("source", _sourceName);
        BSONObj pattern = b.done();

        BSONObj o = jsobj();
        log( 1 ) << "Saving repl source: " << o << endl;

        stringstream ss;
        setClient("local.sources");
        UpdateResult res = updateObjects("local.sources", o, pattern, true/*upsert for pair feature*/, false,ss,false);
        assert( ! res.mod );
        assert( res.num == 1 );
        cc().clearns();

        if ( replacing ) {
            /* if we were in "replace" mode, we now have synced up with the replacement,
               so turn that off.
               */
            replacing = false;
            wassert( replacePeer );
            replacePeer = false;
            Helpers::emptyCollection("local.pair.startup");
        }
    }

    static void addSourceToList(ReplSource::SourceVector &v, ReplSource& s, const BSONObj &spec, ReplSource::SourceVector &old) {
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
        SourceVector old = v;
        v.clear();

        bool gotPairWith = false;

        if ( !cmdLine.source.empty() ) {
            setClient("local.sources");
            // --source <host> specified.
            // check that no items are in sources other than that
            // add if missing
            auto_ptr<Cursor> c = findTableScan("local.sources", BSONObj());
            int n = 0;
            while ( c->ok() ) {
                n++;
                ReplSource tmp(c->current());
                if ( tmp.hostName != cmdLine.source ) {
                    log() << "E10000 --source " << cmdLine.source << " != " << tmp.hostName << " from local.sources collection" << endl;
                    log() << "terminating after 30 seconds" << endl;
                    sleepsecs(30);
                    dbexit( EXIT_REPLICATION_ERROR );
                }
                if ( tmp.only != cmdLine.only ) {
                    log() << "E10001 --only " << cmdLine.only << " != " << tmp.only << " from local.sources collection" << endl;
                    log() << "terminating after 30 seconds" << endl;
                    sleepsecs(30);
                    dbexit( EXIT_REPLICATION_ERROR );
                }
                c->advance();
            }
            uassert( "E10002 local.sources collection corrupt?", n<2 );
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
                massert("--only requires use of --source", cmdLine.only.empty());
            } catch ( ... ) {
                dbexit( EXIT_BADOPTIONS );
            }
        }
        
        if ( replPair ) {
            const string &remote = replPair->remote;
            setClient( "local.sources" );
            // --pairwith host specified.
            // check that no items are in sources other than that
            // add if missing
            auto_ptr<Cursor> c = findTableScan("local.sources", BSONObj());
            int n = 0;
            while ( c->ok() ) {
                n++;
                ReplSource tmp(c->current());
                if ( tmp.hostName != remote ) {
                    log() << "E10003 pairwith " << remote << " != " << tmp.hostName << " from local.sources collection" << endl;
                    log() << "terminating after 30 seconds" << endl;
                    sleepsecs(30);
                    dbexit( EXIT_REPLICATION_ERROR );
                }
                c->advance();
            }
            uassert( "E10002 local.sources collection corrupt?", n<2 );
            if ( n == 0 ) {
                // source missing.  add.
                ReplSource s;
                s.hostName = remote;
                s.save();
            }
        }

        setClient("local.sources");
        auto_ptr<Cursor> c = findTableScan("local.sources", BSONObj());
        while ( c->ok() ) {
            ReplSource tmp(c->current());
            if ( replPair && tmp.hostName == replPair->remote && tmp.sourceName() == "main" ) {
                gotPairWith = true;
                tmp.paired = true;
                if ( replacePeer ) {
                    // peer was replaced -- start back at the beginning.
                    tmp.syncedTo = OpTime();
                    tmp.replacing = true;
                }
            }
            addSourceToList(v, tmp, c->current(), old);
            c->advance();
        }
        cc().clearns();

        if ( !gotPairWith && replPair ) {
            /* add the --pairwith server */
            shared_ptr< ReplSource > s( new ReplSource() );
            s->paired = true;
            s->hostName = replPair->remote;
            s->replacing = replacePeer;
            v.push_back(s);
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
            (*i)->forceResync( requester );
        }
        replAllDead = 0;        
    }
    
    void ReplSource::forceResync( const char *requester ) {
        BSONObj info;
        {
            dbtemprelease t;
            connect();
            bool ok = conn->runCommand( "admin", BSON( "listDatabases" << 1 ), info );
            massert( "Unable to get database list", ok );
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
        string dummyns = string( db ) + ".";
        setClient(dummyns.c_str());        
        assert( cc().database()->name == db );
        dropDatabase(dummyns.c_str());
        return dummyns;
    }
    
    /* grab initial copy of a database from the master */
    bool ReplSource::resync(string db) {
        string dummyNs = resyncDrop( db.c_str(), "internal" );
        setClient( dummyNs.c_str() );
        {
            log() << "resync: cloning database " << db << endl;
            ReplInfo r("resync: cloning a database");
            string errmsg;
            bool ok = cloneFrom(hostName.c_str(), errmsg, cc().database()->name, false, /*slaveok*/ true, /*replauth*/ true, /*snapshot*/false);
            if ( !ok ) {
                problem() << "resync of " << db << " from " << hostName << " failed " << errmsg << endl;
                throw SyncException();
            }
        }

        log() << "resync: done " << db << endl;

        return true;
    }

    void ReplSource::applyOperation(const BSONObj& op) {
      log( 6 ) << "applying op: " << op << endl;
        stringstream ss;
        BSONObj o = op.getObjectField("o");
        const char *ns = op.getStringField("ns");
        // operation type -- see logOp() comments for types
        const char *opType = op.getStringField("op");
        try {
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
                        updateObjects(ns, o, o, true, false, ss, false );
                        if( t.millis() >= 2 ) {
                            RARELY OCCASIONALLY log() << "warning, repl doing slow updates (no _id field) for " << ns << endl;
                        }
                    }
                    else {
                        BSONObjBuilder b;
						b.append(_id);
                        
                        /* erh 10/16/2009 - this is probably not relevant any more since its auto-created, but not worth removing */
                        RARELY ensureHaveIdIndex(ns); // otherwise updates will be slow 

                        updateObjects(ns, o, b.done(), true, false, ss, false );
                    }
                }
            }
            else if ( *opType == 'u' ) {
                RARELY ensureHaveIdIndex(ns); // otherwise updates will be super slow
                updateObjects(ns, o, op.getObjectField("o2"), op.getBoolField("b"), false, ss, false );
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
            else {
                BufBuilder bb;
                BSONObjBuilder ob;
                assert( *opType == 'c' );
                _runCommands(ns, o, ss, bb, ob, true, 0);
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
    */
    void ReplSource::sync_pullOpLog_applyOperation(BSONObj& op, OpTime *localLogTail) {
        log( 6 ) << "processing op: " << op << endl;
        // skip no-op
        if ( op.getStringField( "op" )[ 0 ] == 'n' )
            return;
        
        char clientName[MaxClientLen];
        const char *ns = op.getStringField("ns");
        nsToClient(ns, clientName);

        if ( *ns == '.' ) {
            problem() << "skipping bad op in oplog: " << op.toString() << endl;
            return;
        }
        else if ( *ns == 0 ) {
            problem() << "halting replication, bad op in oplog:\n  " << op.toString() << endl;
            replAllDead = "bad object in oplog";
            throw SyncException();
        }

        if ( !only.empty() && only != clientName )
            return;

        dblock lk;

        if ( localLogTail && replPair && replPair->state == ReplPair::State_Master ) {
            updateSetsWithLocalOps( *localLogTail, true ); // allow unlocking
            updateSetsWithLocalOps( *localLogTail, false ); // don't allow unlocking or conversion to db backed storage
        }

        if ( replAllDead ) {
            // hmmm why is this check here and not at top of this function? does it get set between top and here?
            log() << "replAllDead, throwing SyncException: " << replAllDead << endl;
            throw SyncException();
        }
        
        bool justCreated;
        try {
            justCreated = setClient(ns);
        } catch ( AssertionException& ) {
            problem() << "skipping bad(?) op in oplog, setClient() failed, ns: '" << ns << "'\n";
            addDbNextPass.erase(clientName);
            return;
        }

        bool empty = clientIsEmpty();
        bool incompleteClone = incompleteCloneDbs.count( clientName ) != 0;

        log( 6 ) << "ns: " << ns << ", justCreated: " << justCreated << ", empty: " << empty << ", incompleteClone: " << incompleteClone << endl;

	// always apply admin command command
	// this is a bit hacky -- the semantics of replication/commands aren't well specified
	if ( strcmp( clientName, "admin" ) == 0 && *op.getStringField( "op" ) == 'c' ) {
	  applyOperation( op );
      cc().clearns();
	  return;
	}
        
        if ( justCreated || empty || incompleteClone ) {
            // we must add to incomplete list now that setClient has been called
            incompleteCloneDbs.insert( clientName );
            if ( nClonedThisPass ) {
                /* we only clone one database per pass, even if a lot need done.  This helps us
                 avoid overflowing the master's transaction log by doing too much work before going
                 back to read more transactions. (Imagine a scenario of slave startup where we try to
                 clone 100 databases in one pass.)
                 */
                addDbNextPass.insert( clientName );
            } else {
                if ( incompleteClone ) {
                    log() << "An earlier initial clone of '" << clientName << "' did not complete, now resyncing." << endl;
                }
                save();
                setClient( ns );
                nClonedThisPass++;
                resync(cc().database()->name);
                addDbNextPass.erase(clientName);
                incompleteCloneDbs.erase( clientName );
            }
            save();
        } else {
            bool mod;
            if ( replPair && replPair->state == ReplPair::State_Master ) {
                BSONObj id = idForOp( op, mod );
                if ( !idTracker.haveId( ns, id ) ) {
                    applyOperation( op );    
                } else if ( idTracker.haveModId( ns, id ) ) {
                    log( 6 ) << "skipping operation matching mod id object " << op << endl;
                    BSONObj existing;
                    if ( Helpers::findOne( ns, id, existing ) )
                        logOp( "i", ns, existing );
                } else {
                    log( 6 ) << "skipping operation matching changed id object " << op << endl;
                }
            } else {
                applyOperation( op );
            }
            addDbNextPass.erase( clientName );
        }
        cc().clearns();
    }

    BSONObj ReplSource::idForOp( const BSONObj &op, bool &mod ) {
        mod = false;
        const char *opType = op.getStringField( "op" );
        BSONObj o = op.getObjectField( "o" );
        switch( opType[ 0 ] ) {
            case 'i': {
                BSONObjBuilder idBuilder;
                BSONElement id;
                if ( !o.getObjectID( id ) )
                    return BSONObj();                    
                idBuilder.append( id );
                return idBuilder.obj();
            }
            case 'u': {
                BSONObj o2 = op.getObjectField( "o2" );
                if ( strcmp( o2.firstElement().fieldName(), "_id" ) != 0 )
                    return BSONObj();
                if ( o.firstElement().fieldName()[ 0 ] == '$' )
                    mod = true;
                return o2;
            }
            case 'd': {
                if ( opType[ 1 ] != '\0' )
                    return BSONObj(); // skip "db" op type
                return o;
            }
            default:
                break;
        }        
        return BSONObj();
    }
    
    void ReplSource::updateSetsWithOp( const BSONObj &op, bool mayUnlock ) {
        if ( mayUnlock ) {
            idTracker.mayUpgradeStorage();
        }
        bool mod;
        BSONObj id = idForOp( op, mod );
        if ( !id.isEmpty() ) {
            const char *ns = op.getStringField( "ns" );
            // Since our range of local ops may not be the same as our peer's
            // range of unapplied ops, it is always necessary to rewrite objects
            // to the oplog after a mod update.
            if ( mod )
                idTracker.haveModId( ns, id, true );
            idTracker.haveId( ns, id, true );
        }        
    }
    
    void ReplSource::syncToTailOfRemoteLog() {
        string _ns = ns();
        BSONObj last = conn->findOne( _ns.c_str(), Query().sort( BSON( "$natural" << -1 ) ) );
        if ( !last.isEmpty() ) {
            BSONElement ts = last.findElement( "ts" );
            massert( "non Date ts found", ts.type() == Date || ts.type() == Timestamp );
            syncedTo = OpTime( ts.date() );
        }        
    }
    
    OpTime ReplSource::nextLastSavedLocalTs() const {
        setClient( "local.oplog.$main" );
        auto_ptr< Cursor > c = findTableScan( "local.oplog.$main", BSON( "$natural" << -1 ) );
        if ( c->ok() )
            return OpTime( c->current().getField( "ts" ).date() );        
        return OpTime();
    }
    
    void ReplSource::setLastSavedLocalTs( const OpTime &nextLocalTs ) {
        lastSavedLocalTs_ = nextLocalTs;
        log( 3 ) << "updated lastSavedLocalTs_ to: " << lastSavedLocalTs_ << endl;
    }
    
    void ReplSource::resetSlave() {
        massert( "request to kill slave replication falied",
                conn->simpleCommand( "admin", 0, "forcedead" ) );        
        syncToTailOfRemoteLog();
        {
            dblock lk;
            setLastSavedLocalTs( nextLastSavedLocalTs() );
            save();
            cursor.reset();
        }
    }
    
    bool ReplSource::updateSetsWithLocalOps( OpTime &localLogTail, bool mayUnlock ) {
        setClient( "local.oplog.$main" );
        auto_ptr< Cursor > localLog = findTableScan( "local.oplog.$main", BSON( "$natural" << -1 ) );
        OpTime newTail;
        for( ; localLog->ok(); localLog->advance() ) {
            BSONObj op = localLog->current();
            OpTime ts( localLog->current().getField( "ts" ).date() );
            if ( newTail.isNull() ) {
                newTail = ts;
            }
            if ( !( localLogTail < ts ) )
                break;
            updateSetsWithOp( op, mayUnlock );
            if ( mayUnlock ) {
                RARELY {
                    dbtemprelease t;
                }
            }
        }
        if ( !localLogTail.isNull() && !localLog->ok() ) {
            // local log filled up
            idTracker.reset();
            dbtemprelease t;
            resetSlave();
            massert( "local master log filled, forcing slave resync", false );
        }        
        if ( !newTail.isNull() )
            localLogTail = newTail;
        return true;
    }
    
    /* note: not yet in mutex at this point. */
    bool ReplSource::sync_pullOpLog(int& nApplied) {
        string ns = string("local.oplog.$") + sourceName();
        log(2) << "repl: sync_pullOpLog " << ns << " syncedTo:" << syncedTo.toStringLong() << '\n';

        bool tailing = true;
        DBClientCursor *c = cursor.get();
        if ( c && c->isDead() ) {
            log() << "repl:   old cursor isDead, initiating a new one\n";
            c = 0;
        }

        if ( replPair && replPair->state == ReplPair::State_Master ) {
            dblock lk;
            idTracker.reset();
        }
        OpTime localLogTail = lastSavedLocalTs_;

        bool initial = syncedTo.isNull();
        
        if ( c == 0 || initial ) {
            if ( initial ) {
                // Important to grab last oplog timestamp before listing databases.
                syncToTailOfRemoteLog();
                BSONObj info;
                bool ok = conn->runCommand( "admin", BSON( "listDatabases" << 1 ), info );
                massert( "Unable to get database list", ok );
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
                dblock lk;
                save();
            }
                        
            BSONObjBuilder q;
            q.appendDate("$gte", syncedTo.asDate());
            BSONObjBuilder query;
            query.append("ts", q.done());
            if ( !only.empty() ) {
                // note we may here skip a LOT of data table scanning, a lot of work for the master.
                query.appendRegex("ns", string("^") + only);
            }
            BSONObj queryObj = query.done();
            // queryObj = { ts: { $gte: syncedTo } }

            log(2) << "repl: " << ns << ".find(" << queryObj.toString() << ')' << '\n';
            cursor = conn->query( ns.c_str(), queryObj, 0, 0, 0, Option_CursorTailable | Option_SlaveOk | Option_OplogReplay );
            c = cursor.get();
            tailing = false;
        }
        else {
            log(2) << "repl: tailing=true\n";
        }

        if ( c == 0 ) {
            problem() << "repl:   dbclient::query returns null (conn closed?)" << endl;
            resetConnection();
            return false;
        }

        // show any deferred database creates from a previous pass
        {
            set<string>::iterator i = addDbNextPass.begin();
            if ( i != addDbNextPass.end() ) {
                BSONObjBuilder b;
                b.append("ns", *i + '.');
                b.append("op", "db");
                BSONObj op = b.done();
                sync_pullOpLog_applyOperation(op, 0);
            }
        }

        if ( !c->more() ) {
            if ( tailing ) {
                log(2) << "repl: tailing & no new activity\n";
            } else {
                log() << "repl:   " << ns << " oplog is empty\n";
            }
            {
                dblock lk;
                OpTime nextLastSaved = nextLastSavedLocalTs();
                {
                    dbtemprelease t;
                    if ( !c->more() ) {
                        setLastSavedLocalTs( nextLastSaved );
                    }
                }
                save();            
            }
            return true;
        }

        int n = 0;
        BSONObj op = c->next();
        BSONElement ts = op.findElement("ts");
        if ( ts.type() != Date && ts.type() != Timestamp ) {
            string err = op.getStringField("$err");
            if ( !err.empty() ) {
                problem() << "repl: $err reading remote oplog: " + err << '\n';
                massert( "got $err reading remote oplog", false );
            }
            else {
                problem() << "repl: bad object read from remote oplog: " << op.toString() << '\n';
                massert("repl: bad object read from remote oplog", false);
            }
        }
        
        if ( replPair && replPair->state == ReplPair::State_Master ) {
            
            OpTime nextOpTime( ts.date() );
            if ( !tailing && !initial && nextOpTime != syncedTo ) {
                log() << "remote slave log filled, forcing slave resync" << endl;
                resetSlave();
                return true;
            }            
            
            dblock lk;
            updateSetsWithLocalOps( localLogTail, true );
        }
        
        OpTime nextOpTime( ts.date() );
        log(2) << "repl: first op time received: " << nextOpTime.toString() << '\n';
        if ( tailing || initial ) {
            if ( initial )
                log(1) << "repl:   initial run\n";
            else
                assert( syncedTo < nextOpTime );
            sync_pullOpLog_applyOperation(op, &localLogTail);
            n++;
        }
        else if ( nextOpTime != syncedTo ) {
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
            assert( syncedTo < nextOpTime );
            throw SyncException();
        }
        else {
            /* t == syncedTo, so the first op was applied previously. */
        }

        // apply operations
        {
			time_t saveLast = time(0);
            while ( 1 ) {
                /* from a.s.:
                   I think the idea here is that we can establish a sync point between the local op log and the remote log with the following steps:

                   1) identify most recent op in local log -- call it O
                   2) ask "does nextOpTime reflect the tail of the remote op log?" (in other words, is more() false?) - If yes, all subsequent ops after nextOpTime in the remote log must have occurred after O.  If no, we can't establish a sync point.

                   Note that we can't do step (2) followed by step (1) because if we do so ops may be added to both machines between steps (2) and (1) and we can't establish a sync point.  (In particular, between (2) and (1) an op may be added to the remote log before a different op is added to the local log.  In this case, the newest remote op will have occurred after nextOpTime but before O.)

                   Now, for performance reasons we don't want to have to identify the most recent op in the local log every time we call c->more() because in performance sensitive situations more() will be true most of the time.  So we do:

                   0) more()?
                   1) find most recent op in local log
                   2) more()?
                */
                if ( !c->more() ) {
                    dblock lk;
                    OpTime nextLastSaved = nextLastSavedLocalTs(); // this may make c->more() become true
                    {
                        dbtemprelease t;
                        if ( c->more() ) {
                            continue;
                        } else {
                            setLastSavedLocalTs( nextLastSaved );
                        }
                    }
                    syncedTo = nextOpTime;
                    save(); // note how far we are synced up to now
                    log() << "repl:   applied " << n << " operations" << endl;
                    nApplied = n;
                    log() << "repl: end sync_pullOpLog syncedTo: " << syncedTo.toStringLong() << endl;
                    break;
                }

                OCCASIONALLY if( n > 100000 || time(0) - saveLast > 60 ) { 
					// periodically note our progress, in case we are doing a lot of work and crash
					dblock lk;
                    syncedTo = nextOpTime;
                    // can't update local log ts since there are pending operations from our peer
					save();
                    log() << "repl:   checkpoint applied " << n << " operations" << endl;
                    log() << "repl:   syncedTo: " << syncedTo.toStringLong() << endl;
					saveLast = time(0);
					n = 0;
				}

                BSONObj op = c->next();
                ts = op.findElement("ts");
                assert( ts.type() == Date || ts.type() == Timestamp );
                OpTime last = nextOpTime;
                OpTime tmp( ts.date() );
                nextOpTime = tmp;
                if ( !( last < nextOpTime ) ) {
                    problem() << "sync error: last " << last.toString() << " >= nextOpTime " << nextOpTime.toString() << endl;
                    uassert("bad 'ts' value in sources", false);
                }

                sync_pullOpLog_applyOperation(op, &localLogTail);
                n++;
            }
        }

        return true;
    }

	BSONObj userReplQuery = fromjson("{\"user\":\"repl\"}");

	bool replAuthenticate(DBClientConnection *conn) {
        AuthenticationInfo *ai = currentClient.get()->ai;
		if( !ai->isAuthorized("admin") ) { 
			log() << "replauthenticate: requires admin permissions, failing\n";
			return false;
		}

		BSONObj user;
		{
			dblock lk;
			DBContext ctxt("local.");
			if( !Helpers::findOne("local.system.users", userReplQuery, user) ) { 
				// try the first user is local
				if( !Helpers::getSingleton("local.system.users", user) ) {
					if( noauth ) 
						return true; // presumably we are running a --noauth setup all around.

					log() << "replauthenticate: no user in local.system.users to use for authentication\n";
					return false;
				}
			}
		}

		string u = user.getStringField("user");
		string p = user.getStringField("pwd");
		massert("bad user object? [1]", !u.empty());
		massert("bad user object? [2]", !p.empty());
		string err;
		if( !conn->auth("local", u.c_str(), p.c_str(), err, false) ) {
			log() << "replauthenticate: can't authenticate to master server, user:" << u << endl;
			return false;
		}
		return true;
	}

    bool ReplSource::connect() {
        if ( conn.get() == 0 ) {
            conn = auto_ptr<DBClientConnection>(new DBClientConnection());
            string errmsg;
            ReplInfo r("trying to connect to sync source");
            if ( !conn->connect(hostName.c_str(), errmsg) || !replAuthenticate(conn.get()) ) {
                resetConnection();
                log() << "repl: " << errmsg << endl;
                return false;
            }
        }
        return true;
    }
    
    /* note: not yet in mutex at this point.
       returns true if everything happy.  return false if you want to reconnect.
    */
    bool ReplSource::sync(int& nApplied) {
        ReplInfo r("sync");
        if ( !cmdLine.quiet )
            log() << "repl: " << sourceName() << '@' << hostName << endl;
        nClonedThisPass = 0;

        // FIXME Handle cases where this db isn't on default port, or default port is spec'd in hostName.
        if ( (string("localhost") == hostName || string("127.0.0.1") == hostName) && cmdLine.port == CmdLine::DefaultDBPort ) {
            log() << "repl:   can't sync from self (localhost). sources configuration may be wrong." << endl;
            sleepsecs(5);
            return false;
        }

        if ( !connect() ) {
            if ( replPair && paired ) {
                assert( startsWith(hostName.c_str(), replPair->remoteHost.c_str()) );
                replPair->arbitrate();
            }
            {
                ReplInfo r("can't connect to sync source");
            }
            return false;            
        }
        
        if ( paired ) {
            int remote = replPair->negotiate(conn.get(), "direct");
            int nMasters = ( remote == ReplPair::State_Master ) + ( replPair->state == ReplPair::State_Master );
            if ( getInitialSyncCompleted() && nMasters != 1 ) {
                log() << ( nMasters == 0 ? "no master" : "two masters" ) << ", deferring oplog pull" << endl;
                return true;
            }
        }

        /*
        	// get current mtime at the server.
        	BSONObj o = conn->findOne("admin.$cmd", opTimeQuery);
        	BSONElement e = o.findElement("optime");
        	if( e.eoo() ) {
        		log() << "repl:   failed to get cur optime from master" << endl;
        		log() << "        " << o.toString() << endl;
        		return false;
        	}
        	uassert( e.type() == Date );
        	OpTime serverCurTime;
        	serverCurTime.asDate() = e.date();
        */
        return sync_pullOpLog(nApplied);
    }

    /* -- Logging of operations -------------------------------------*/

// cached copies of these...so don't rename them
    NamespaceDetails *localOplogMainDetails = 0;
    Database *localOplogClient = 0;

    void logOp(const char *opstr, const char *ns, const BSONObj& obj, BSONObj *patt, bool *b) {
        if ( master ) {
            _logOp(opstr, ns, "local.oplog.$main", obj, patt, b, OpTime::now());
            char cl[ 256 ];
            nsToClient( ns, cl );
        }
        NamespaceDetailsTransient &t = NamespaceDetailsTransient::get( ns );
        if ( t.logValid() ) {
            try {
                _logOp(opstr, ns, t.logNS().c_str(), obj, patt, b, OpTime::now());
            } catch ( const DBException & ) {
                t.invalidateLog();
            }
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
       bb:
         if not null, specifies a boolean to pass along to the other side as b: param.
         used for "justOne" or "upsert" flags on 'd', 'u'
       first: true
         when set, indicates this is the first thing we have logged for this database.
         thus, the slave does not need to copy down all the data when it sees this.
    */
    void _logOp(const char *opstr, const char *ns, const char *logNS, const BSONObj& obj, BSONObj *o2, bool *bb, const OpTime &ts ) {
        if ( strncmp(ns, "local.", 6) == 0 )
            return;

        DEV assertInWriteLock();

        DBContext context;

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
                setClient("local.");
                localOplogClient = cc().database();
                localOplogMainDetails = nsdetails(logNS);
            }
            cc().setns("", localOplogClient); // database = localOplogClient;
            r = theDataFileMgr.fast_oplog_insert(localOplogMainDetails, logNS, len);
        } else {
            setClient( logNS );
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
            dblock lk;
            ReplSource::loadAll(sources);
        }

        if ( sources.empty() ) {
            /* replication is not configured yet (for --slave) in local.sources.  Poll for config it
            every 20 seconds.
            */
            return 20;
        }

        int sleepAdvice = 1;
        for ( ReplSource::SourceVector::iterator i = sources.begin(); i != sources.end(); i++ ) {
            ReplSource *s = i->get();
            bool ok = false;
            try {
                ok = s->sync(nApplied);
                bool moreToSync = s->haveMoreDbsToSync();
                if( !ok ) { 
                    sleepAdvice = 3;
                }
                else if( moreToSync ) {
                    sleepAdvice = 0;
                }
                if ( ok && !moreToSync /*&& !s->syncedTo.isNull()*/ ) {
                    pairSync->setInitialSyncCompletedLocking();
                }
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
            if ( !ok )
                s->resetConnection();
        }
        return sleepAdvice;
    }

    void replMain() {
        ReplSource::SourceVector sources;
        while ( 1 ) {
            int s = 0;
            {
                dblock lk;
                if ( replAllDead ) {
                    if ( !autoresync || !ReplSource::throttledForceResyncDead( "auto" ) )
                        break;
                }
                assert( syncing == 0 );
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
            } catch (...) {
                out() << "caught exception in _replMain" << endl;
                s = 4;
            }
            {
                dblock lk;
                assert( syncing == 1 );
                syncing--;
            }
            if ( s ) {
                stringstream ss;
                ss << "repl: sleep " << s << "sec before next pass";
                string msg = ss.str();
                if ( ! cmdLine.quiet )
                    log() << msg << endl;
                ReplInfo r(msg.c_str());
                sleepsecs(s);
            }
        }
    }

    int debug_stop_repl = 0;

    void replSlaveThread() {
        sleepsecs(1);

        {
            dblock lk;

            Client::initThread("replslave");
	    currentClient.get()->ai->authorize("admin");
        
            BSONObj obj;
            if ( Helpers::getSingleton("local.pair.startup", obj) ) {
                // should be: {replacepeer:1}
                replacePeer = true;
                pairSync->setInitialSyncCompleted(); // we are the half that has all the data
            }
        }

        while ( 1 ) {
            try {
                replMain();
                if ( debug_stop_repl )
                    break;
                sleepsecs(5);
            }
            catch ( AssertionException& ) {
                ReplInfo r("Assertion in replSlaveThread(): sleeping 5 minutes before retry");
                problem() << "Assertion in replSlaveThread(): sleeping 5 minutes before retry" << endl;
                sleepsecs(300);
            }
        }
    }

    void tempThread() {
        while ( 1 ) {
            out() << dbMutexInfo.isLocked() << endl;
            sleepmillis(100);
        }
    }

    void createOplog() {
        dblock lk;

        const char * ns = "local.oplog.$main";
        setClient(ns);
        
        if ( nsdetails( ns ) )
            return;
        
        /* create an oplog collection, if it doesn't yet exist. */
        BSONObjBuilder b;
        double sz;
        if ( cmdLine.oplogSize != 0 )
            sz = (double)cmdLine.oplogSize;
        else {
            sz = 50.0 * 1000 * 1000;
            if ( sizeof(int *) >= 8 ) {
                sz = 990.0 * 1000 * 1000;
                boost::intmax_t free = freeSpace(); //-1 if call not supported.
                double fivePct = free * 0.05;
                if ( fivePct > sz )
                    sz = fivePct;
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
        cc().clearns();
    }
    
    void startReplication() {
        /* this was just to see if anything locks for longer than it should -- we need to be careful
           not to be locked when trying to connect() or query() the other side.
           */
        //boost::thread tempt(tempThread);

        if ( !slave && !master && !replPair )
            return;

        {
            dblock lk;
            pairSync->init();
        }

        if ( slave || replPair ) {
            if ( slave ) {
				assert( slave == SimpleSlave );
                log(1) << "slave=true" << endl;
			}
			else
				slave = ReplPairSlave;
            boost::thread repl_thread(replSlaveThread);
        }

        if ( master || replPair ) {
            if ( master  )
                log(1) << "master=true" << endl;
            master = true;
            createOplog();
        }
    }

    /* called from main at server startup */
    void pairWith(const char *remoteEnd, const char *arb) {
        replPair = new ReplPair(remoteEnd, arb);
    }

    class CmdLogCollection : public Command {
    public:
        virtual bool slaveOk() {
            return false;
        }
        CmdLogCollection() : Command( "logCollection" ) {}
        virtual void help( stringstream &help ) const {
            help << "examples: { logCollection: <collection ns>, start: 1 }, "
                 << "{ logCollection: <collection ns>, validateComplete: 1 }";
        }
        virtual bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
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
            NamespaceDetailsTransient &t = NamespaceDetailsTransient::get( logCollection.c_str() );
            if ( start ) {
                if ( t.logNS().empty() ) {
                    if ( logSizeMb == INT_MIN ) {
                        t.startLog();
                    } else {
                        t.startLog( logSizeMb );
                    }
                } else {
                    errmsg = "Log already started for ns: " + logCollection;
                    return false;
                }
            } else {
                if ( t.logNS().empty() ) {
                    errmsg = "No log to validateComplete for ns: " + logCollection;
                    return false;
                } else {
                    if ( !t.validateCompleteLog() ) {
                        errmsg = "Oplog failure, insufficient space allocated";
                        return false;
                    }
                }
            }
            return true;
        }
    } cmdlogcollection;
    
} // namespace mongo
