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
   local.dbinfo.<dbname> - as master, have we already logged events to the oplog for this database?
							{ haveLogged : true }
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

namespace mongo {

    extern bool quiet;
    extern boost::mutex &dbMutex;
    extern long long oplogSize;
    int _updateObjects(const char *ns, BSONObj updateobj, BSONObj pattern, bool upsert, stringstream& ss, bool logOp=false);
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
        if ( n != state && !quiet )
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
                vector<ReplSource*> sources;
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
            if ( !replAllDead ) {
                errmsg = "not dead, no need to resync";
                return false;
            }
            ReplSource::forceResyncDead( "user" );
            result.append( "info", "triggered resync for all sources" );
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
			AuthenticationInfo *ai = authInfo.get();
			bool authed = ai == 0 || ai->isAuthorized("admin");

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
                result.append("ismaster", 1);
				result.append("msg", "not paired");
            }
            
            return true;
        }
    } cmdismaster;

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
                auto_ptr<DBClientConnection> conn( new DBClientConnection() );
                if ( !conn->connect( remote.c_str(), errmsg ) ) {
                    result.append( "you_are", ReplPair::State_Master );
                    return true;
                }
                BSONObj ret = conn->findOne( "admin.$cmd", cmdObj );
                BSONObjIterator i( ret );
                while( i.more() ) {
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
            if ( myname.empty() || was < -1 ) {
                errmsg = "your_name/i_was not specified";
                return false;
            }

            int N = ReplPair::State_Negotiating;
            int M = ReplPair::State_Master;
            int S = ReplPair::State_Slave;

            if ( !replPair->dominant( myname ) ) {
                result.append( "you_are", N );
                result.append( "i_am", N );
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
    
    void ReplPair::negotiate(DBClientConnection *conn, string method) {
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
            return;
        }
        int x = res.getIntField("you_are");
        // State_Negotiating means the remote node is not dominant and cannot
        // choose who is master.
        if ( x != State_Slave && x != State_Master && x != State_Negotiating ) {
            problem() << method << " negotiate: bad you_are value " << res.toString() << endl;
            return;
        }
        if ( x != State_Negotiating ) {
            string message = method + " negotiation";
            setMasterLocked(x, message.c_str());
        }
    }

    OpTime last(0, 0);

    OpTime OpTime::now() {
        unsigned t = (unsigned) time(0);
        if ( last.secs == t ) {
            last.i++;
            return last;
        }
        last = OpTime(t, 1);
        return last;
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
            uassert( "bad sources 'syncedTo' field value", e.type() == Date );
            OpTime tmp( e.date() );
            syncedTo = tmp;
            //syncedTo.asDate() = e.date();
        }

        BSONObj dbsObj = o.getObjectField("dbs");
        if ( !dbsObj.isEmpty() ) {
            BSONObjIterator i(dbsObj);
            while ( 1 ) {
                BSONElement e = i.next();
                if ( e.eoo() )
                    break;
                dbs.insert( e.fieldName() );
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
            b.appendDate("syncedTo", syncedTo.asDate());

        BSONObjBuilder dbs_builder;
        int n = 0;
        for ( set<string>::iterator i = dbs.begin(); i != dbs.end(); i++ ) {
            n++;
            dbs_builder.appendBool(i->c_str(), 1);
        }
        if ( n )
            b.append("dbs", dbs_builder.done());

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

        stringstream ss;
        setClient("local.sources");
        int u = _updateObjects("local.sources", o, pattern, true/*upsert for pair feature*/, ss);
        assert( u == 1 || u == 4 );
        database = 0;

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

    void ReplSource::cleanup(vector<ReplSource*>& v) {
        for ( vector<ReplSource*>::iterator i = v.begin(); i != v.end(); i++ )
            delete *i;
    }

    string dashDashSource;
    string dashDashOnly;

    static void addSourceToList(vector<ReplSource*>&v, ReplSource& s, vector<ReplSource*>&old) {
        for ( vector<ReplSource*>::iterator i = old.begin(); i != old.end();  ) {
            if ( s == **i ) {
                v.push_back(*i);
                old.erase(i);
                return;
            }
            i++;
        }

        v.push_back( new ReplSource(s) );
    }

    /* we reuse our existing objects so that we can keep our existing connection
       and cursor in effect.
    */
    void ReplSource::loadAll(vector<ReplSource*>& v) {
        vector<ReplSource *> old = v;
        v.erase(v.begin(), v.end());

        bool gotPairWith = false;

        if ( !dashDashSource.empty() ) {
            setClient("local.sources");
            // --source <host> specified.
            // check that no items are in sources other than that
            // add if missing
            auto_ptr<Cursor> c = findTableScan("local.sources", emptyObj);
            int n = 0;
            while ( c->ok() ) {
                n++;
                ReplSource tmp(c->current());
                if ( tmp.hostName != dashDashSource ) {
                    log() << "E10000 --source " << dashDashSource << " != " << tmp.hostName << " from local.sources collection" << endl;
                    log() << "terminating after 30 seconds" << endl;
                    sleepsecs(30);
                    dbexit(18);
                }
                if ( tmp.only != dashDashOnly ) {
                    log() << "E10001 --only " << dashDashOnly << " != " << tmp.only << " from local.sources collection" << endl;
                    log() << "terminating after 30 seconds" << endl;
                    sleepsecs(30);
                    dbexit(18);
                }
                c->advance();
            }
            uassert( "E10002 local.sources collection corrupt?", n<2 );
            if ( n == 0 ) {
                // source missing.  add.
                ReplSource s;
                s.hostName = dashDashSource;
                s.only = dashDashOnly;
                s.save();
            }
        }
        else { 
            massert("--only requires use of --source", dashDashOnly.empty());
        }

        setClient("local.sources");
        auto_ptr<Cursor> c = findTableScan("local.sources", emptyObj);
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
            addSourceToList(v, tmp, old);
            c->advance();
        }
        database = 0;

        if ( !gotPairWith && replPair ) {
            /* add the --pairwith server */
            ReplSource *s = new ReplSource();
            s->paired = true;
            s->hostName = replPair->remote;
            s->replacing = replacePeer;
            v.push_back(s);
        }

        for ( vector<ReplSource*>::iterator i = old.begin(); i != old.end(); i++ )
            delete *i;
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
        vector<ReplSource*> sources;
        ReplSource::loadAll(sources);
        for( vector< ReplSource * >::iterator i = sources.begin(); i != sources.end(); ++i ) {
            (*i)->forceResync( requester );
        }
        replAllDead = 0;        
    }
    
    void ReplSource::forceResync( const char *requester ) {
        for( set< string >::iterator i = dbs.begin(); i != dbs.end(); ++i ) {
            log() << requester << " resync: dropping database " << *i << endl;
            string dummyns = *i + ".";
            setClientTempNs( dummyns.c_str() );
            assert( database->name == *i );
            dropDatabase( dummyns.c_str() );
        }
        dbs.clear();
        syncedTo = OpTime();
    }
    
    bool ReplSource::resync(string db) {
        {
            log(1) << "resync: dropping database " << db << endl;
            string dummyns = db + ".";
            assert( database->name == db );
            dropDatabase(dummyns.c_str());
            setClientTempNs(dummyns.c_str());
        }

        {
            log() << "resync: cloning database " << db << endl;
            ReplInfo r("resync: cloning a database");
            string errmsg;
            bool ok = cloneFrom(hostName.c_str(), errmsg, database->name, false, /*slaveok*/ true, /*replauth*/ true);
            if ( !ok ) {
                problem() << "resync of " << db << " from " << hostName << " failed " << errmsg << endl;
                throw SyncException();
            }
        }

        log() << "resync: done " << db << endl;

        /* add the db to our dbs array which we will write back to local.sources.
           note we are not in a consistent state until the oplog gets applied,
           which happens next when this returns.
           */
        dbs.insert(db);
        return true;
    }

    void ReplSource::applyOperation(const BSONObj& op) {
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
                        _updateObjects(ns, o, o, true, ss);
                        if( t.millis() >= 2 ) {
                            RARELY OCCASIONALLY log() << "warning, repl doing slow updates (no _id field) for " << ns << endl;
                        }
                    }
                    else {
                        BSONObjBuilder b;
						b.append(_id);
                        RARELY ensureHaveIdIndex(ns); // otherwise updates will be slow
                        _updateObjects(ns, o, b.done(), true, ss);
                    }
                }
            }
            else if ( *opType == 'u' ) {
                RARELY ensureHaveIdIndex(ns); // otherwise updates will be super slow
                _updateObjects(ns, o, op.getObjectField("o2"), op.getBoolField("b"), ss);
            }
            else if ( *opType == 'd' ) {
                if ( opType[1] == 0 )
                    deleteObjects(ns, o, op.getBoolField("b"));
                else
                    assert( opType[1] == 'b' ); // "db" advertisement
            }
            else {
                BufBuilder bb;
                BSONObjBuilder ob;
                assert( *opType == 'c' );
                _runCommands(ns, o, ss, bb, ob, true, 0);
            }
        }
        catch ( UserException& e ) {
            log() << "sync: caught user assertion " << e.msg << '\n';
        }        
    }
    
    /* local.$oplog.main is of the form:
         { ts: ..., op: <optype>, ns: ..., o: <obj> , o2: <extraobj>, b: <boolflag> }
         ...
       see logOp() comments.
    */
    void ReplSource::sync_pullOpLog_applyOperation(BSONObj& op) {
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

        bool newDb = dbs.count(clientName) == 0;
        if ( newDb && nClonedThisPass ) {
            /* we only clone one database per pass, even if a lot need done.  This helps us
               avoid overflowing the master's transaction log by doing too much work before going
               back to read more transactions. (Imagine a scenario of slave startup where we try to
               clone 100 databases in one pass.)
            */
            addDbNextPass.insert(clientName);
            return;
        }

        dblock lk;
        bool justCreated;
        try {
            justCreated = setClientTempNs(ns);
        } catch ( AssertionException& ) {
            problem() << "skipping bad(?) op in oplog, setClient() failed, ns: '" << ns << "'\n";
            addDbNextPass.erase(clientName);
            return;
        }

        if ( replAllDead ) {
            // hmmm why is this check here and not at top of this function? does it get set between top and here?
            log() << "replAllDead, throwing SyncException\n";
            throw SyncException();
        }

        // operation type -- see logOp() comments for types
        const char *opType = op.getStringField("op");

        if ( justCreated || /* datafiles were missing.  so we need everything, no matter what sources object says */
                newDb ) /* if not in dbs, we've never synced this database before, so we need everything */
        {
            if ( op.getBoolField("first") &&
                    pairSync->initialSyncCompleted() /*<- when false, we are a replacement volume for a pair and need a full sync */
               ) {
                log() << "pull: got {first:true} op ns:" << ns << '\n';
                /* this is the first thing in the oplog ever, so we don't need to resync(). */
                if ( newDb )
                    dbs.insert(clientName);
                else
                    problem() << "warning: justCreated && !newDb in repl " << op.toString() << endl;
            }
            else if ( paired && !justCreated ) {
                if ( strcmp(opType,"db") == 0 && strcmp(ns, "admin.") == 0 ) {
                    // "admin" is a special namespace we use for priviledged commands -- ok if it exists first on
                    // either side
                }
                else {
                    /* the other half of our pair has some operations. yet we already had a db on our
                    disk even though the db in question is not listed in the source.
                    */
                    replAllDead = "pair: historical image missing for a db";
                    problem() << "pair: historical image missing for " << clientName << ", setting replAllDead=true" << endl;
                    log() << "op:" << op.toString() << endl;
                    /*
                    log() << "TEMP: pair: assuming we have the historical image for: " <<
                    clientName << ". add extra checks here." << endl;
                    dbs.insert(clientName);
                    */
                }
            }
            else {
                nClonedThisPass++;
                resync(database->name);
            }
            addDbNextPass.erase(clientName);
        }

        applyOperation( op );
        database = 0;
    }

    /* note: not yet in mutex at this point. */
    bool ReplSource::sync_pullOpLog() {
        string ns = string("local.oplog.$") + sourceName();
        log(2) << "repl: sync_pullOpLog " << ns << " syncedTo:" << syncedTo.toStringLong() << '\n';

        bool tailing = true;
        DBClientCursor *c = cursor.get();
        if ( c && c->isDead() ) {
            log() << "pull:   old cursor isDead, initiating a new one\n";
            c = 0;
        }

        if ( c == 0 ) {
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
            cursor = conn->query( ns.c_str(), queryObj, 0, 0, 0, Option_CursorTailable | Option_SlaveOk );
            c = cursor.get();
            tailing = false;
        }
        else {
            log(2) << "repl: tailing=true\n";
        }

        if ( c == 0 ) {
            problem() << "pull:   dbclient::query returns null (conn closed?)" << endl;
            resetConnection();
            sleepsecs(3);
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
                sync_pullOpLog_applyOperation(op);
            }
        }

        if ( !c->more() ) {
            if ( tailing ) {
                log(2) << "repl: tailing & no new activity\n";
            } else
                log() << "pull:   " << ns << " oplog is empty\n";
            sleepsecs(3);
            return true;
        }

        int n = 0;
        BSONObj op = c->next();
        BSONElement ts = op.findElement("ts");
        if ( ts.type() != Date ) {
            string err = op.getStringField("$err");
            if ( !err.empty() ) {
                problem() << "pull: $err reading remote oplog: " + err << '\n';
                massert( "got $err reading remote oplog", false );
            }
            else {
                problem() << "pull: bad object read from remote oplog: " << op.toString() << '\n';
                massert("pull: bad object read from remote oplog", false);
            }
        }
        OpTime nextOpTime( ts.date() );
        log(2) << "repl: first op time received: " << nextOpTime.toString() << '\n';
        bool initial = syncedTo.isNull();
        if ( initial || tailing ) {
            if ( tailing ) {
                assert( syncedTo < nextOpTime );
            }
            else {
                log(1) << "pull:   initial run\n";
            }
            {
                sync_pullOpLog_applyOperation(op);
                n++;
            }
        }
        else if ( nextOpTime != syncedTo ) {
            Nullstream& l = log();
            l << "pull:   nextOpTime " << nextOpTime.toStringLong() << ' ';
            if ( nextOpTime < syncedTo )
                l << "<??";
            else
                l << ">";

            l << " syncedTo " << syncedTo.toStringLong() << '\n';
            log() << "pull:   time diff: " << (nextOpTime.getSecs() - syncedTo.getSecs()) << "sec\n";
            log() << "pull:   tailing: " << tailing << '\n';
            log() << "pull:   data too stale, halting replication" << endl;
            replInfo = replAllDead = "data too stale halted replication";
            assert( syncedTo < nextOpTime );
            throw SyncException();
        }
        else {
            /* t == syncedTo, so the first op was applied previously, no need to redo it. */
        }

        // apply operations
        {
			unsigned nSaveLast = 0;
			time_t saveLast = time(0);
            while ( 1 ) {
                if ( !c->more() ) {
                    log() << "pull:   applied " << n << " operations" << endl;
                    syncedTo = nextOpTime;
                    log(2) << "repl: end sync_pullOpLog syncedTo: " << syncedTo.toStringLong() << '\n';
                    dblock lk;
                    save(); // note how far we are synced up to now
                    break;
                }

				nSaveLast++;
				OCCASIONALLY if( nSaveLast > 100000 || time(0) - saveLast > 5 * 60 ) { 
					// periodically note our progress, in case we are doing a lot of work and crash
					dblock lk;
					save();
					saveLast = time(0);
					nSaveLast = 0;
				}

                /* todo: get out of the mutex for the next()? */
                BSONObj op = c->next();
                ts = op.findElement("ts");
                assert( ts.type() == Date );
                OpTime last = nextOpTime;
                OpTime tmp( ts.date() );
                nextOpTime = tmp;
                if ( !( last < nextOpTime ) ) {
                    problem() << "sync error: last " << last.toString() << " >= nextOpTime " << nextOpTime.toString() << endl;
                    uassert("bad 'ts' value in sources", false);
                }

                sync_pullOpLog_applyOperation(op);
                n++;
            }
        }

        return true;
    }

	BSONObj userReplQuery = fromjson("{\"user\":\"repl\"}");

	bool replAuthenticate(DBClientConnection *conn) {
		AuthenticationInfo *ai = authInfo.get();
		if( ai && !ai->isAuthorized("admin") ) { 
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

    /* note: not yet in mutex at this point.
       returns true if everything happy.  return false if you want to reconnect.
    */
    bool ReplSource::sync() {
        ReplInfo r("sync");
        if ( !quiet )
            log() << "pull: " << sourceName() << '@' << hostName << endl;
        nClonedThisPass = 0;

        // FIXME Handle cases where this db isn't on default port, or default port is spec'd in hostName.
        if ( (string("localhost") == hostName || string("127.0.0.1") == hostName) && port == DBPort ) {
            log() << "pull:   can't sync from self (localhost). sources configuration may be wrong." << endl;
            sleepsecs(5);
            return false;
        }

        if ( conn.get() == 0 ) {
            conn = auto_ptr<DBClientConnection>(new DBClientConnection());
            string errmsg;
            ReplInfo r("trying to connect to sync source");
            if ( !conn->connect(hostName.c_str(), errmsg) || !replAuthenticate(conn.get()) ) {
                resetConnection();
                log() << "pull:   cantconn " << errmsg << endl;
                if ( replPair && paired ) {
                    assert( startsWith(hostName.c_str(), replPair->remoteHost.c_str()) );
                    replPair->arbitrate();
                }
                {
                    ReplInfo r("can't connect to sync source, sleeping");
                    sleepsecs(1);
                }
                return false;
            }
        }

        if ( paired )
            replPair->negotiate(conn.get(), "direct");

        /*
        	// get current mtime at the server.
        	BSONObj o = conn->findOne("admin.$cmd", opTimeQuery);
        	BSONElement e = o.findElement("optime");
        	if( e.eoo() ) {
        		log() << "pull:   failed to get cur optime from master" << endl;
        		log() << "        " << o.toString() << endl;
        		return false;
        	}
        	uassert( e.type() == Date );
        	OpTime serverCurTime;
        	serverCurTime.asDate() = e.date();
        */
        return sync_pullOpLog();
    }

    /* -- Logging of operations -------------------------------------*/

// cached copies of these...
    NamespaceDetails *localOplogMainDetails = 0;
    Database *localOplogClient = 0;

    void logOp(const char *opstr, const char *ns, BSONObj& obj, BSONObj *patt, bool *b) {
        if ( master )
            _logOp(opstr, ns, "local.oplog.$main", obj, patt, b);
        NamespaceDetailsTransient &t = NamespaceDetailsTransient::get( ns );
        if ( t.logValid() )
            _logOp(opstr, ns, t.logNS().c_str(), obj, patt, b);
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
       bb:
         if not null, specifies a boolean to pass along to the other side as b: param.
         used for "justOne" or "upsert" flags on 'd', 'u'
       first: true
         when set, indicates this is the first thing we have logged for this database.
         thus, the slave does not need to copy down all the data when it sees this.
    */
    void _logOp(const char *opstr, const char *ns, const char *logNS, BSONObj& obj, BSONObj *o2, bool *bb) {
        if ( strncmp(ns, "local.", 6) == 0 )
            return;

        Database *oldClient = database;
        bool haveLogged = database && database->haveLogged();

        /* we jump through a bunch of hoops here to avoid copying the obj buffer twice --
           instead we do a single copy to the destination position in the memory mapped file.
        */

        BSONObjBuilder b;
        b.appendDate("ts", OpTime::now().asDate());
        b.append("op", opstr);
        b.append("ns", ns);
        if ( bb )
            b.appendBool("b", *bb);
        if ( o2 )
            b.append("o2", *o2);
        if ( !haveLogged ) {
            b.appendBool("first", true);
            if ( database ) // null on dropDatabase()'s logging.
                database->setHaveLogged();
        }
        BSONObj partial = b.done();
        int posz = partial.objsize();
        int len = posz + obj.objsize() + 1 + 2 /*o:*/;

        Record *r;
        if ( strncmp( logNS, "local.", 6 ) == 0 ) { // For now, assume this is olog main
            if ( localOplogMainDetails == 0 ) {
                setClientTempNs("local.");
                localOplogClient = database;
                localOplogMainDetails = nsdetails(logNS);
            }
            database = localOplogClient;
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
        
        //BSONObj temp(r);
        //out() << "temp:" << temp.toString() << endl;

        database = oldClient;
    }

    /* --------------------------------------------------------------*/

    /*
    TODO:
    _ source has autoptr to the cursor
    _ reuse that cursor when we can
    */

    /* returns: # of seconds to sleep before next pass */
    int _replMain(vector<ReplSource*>& sources) {
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

        bool sleep = true;
        for ( vector<ReplSource*>::iterator i = sources.begin(); i != sources.end(); i++ ) {
            ReplSource *s = *i;
            bool ok = false;
            try {
                ok = s->sync();
                bool moreToSync = s->haveMoreDbsToSync();
                sleep = !moreToSync;
                if ( ok && !moreToSync /*&& !s->syncedTo.isNull()*/ ) {
                    pairSync->setInitialSyncCompletedLocking();
                }
            }
            catch ( SyncException& ) {
                log() << "caught SyncException, sleeping 10 secs" << endl;
                return 10;
            }
            catch ( AssertionException& e ) {
                if ( e.severe() ) {
                    log() << "replMain exception " << e.what() << ", sleeping 1 minutes" << endl;
                    return 60;
                }
                else {
                    log() << "replMain exception " << e.what() << '\n';
                }
                replInfo = "replMain caught AssertionException";
            }
            catch ( ... ) { 
                log() << "unexpected assertion during replication.  replication will halt" << endl;
                replAllDead = "caught unexpected assertion during replication";
            }
            if ( !ok )
                s->resetConnection();
        }
        if ( sleep ) {
            return 3;
        }
        return 0;
    }

    void replMain() {
        vector<ReplSource*> sources;
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
                s = _replMain(sources);
            } catch (...) {
                out() << "TEMP: caught exception in _replMain" << endl;
            }
            {
                dblock lk;
                assert( syncing == 1 );
                syncing--;
            }
            if ( s ) {
                stringstream ss;
                ss << "replMain: sleep " << s << " before next pass";
                string msg = ss.str();
                ReplInfo r(msg.c_str());
                sleepsecs(s);
            }
        }

//    assert(false);
//	ReplSource::cleanup(sources);
    }

    int debug_stop_repl = 0;

    void replSlaveThread() {
        sleepsecs(1);

        {
            dblock lk;

			AuthenticationInfo *ai = new AuthenticationInfo();
			ai->authorize("admin");
			authInfo.reset(ai);
        
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

    /* used to verify that slave knows what databases we have */
    void logOurDbsPresence() {
        path dbs(dbpath);
        directory_iterator end;
        directory_iterator i(dbs);

        dblock lk;

        while ( i != end ) {
            path p = *i;
            string f = p.leaf();
            if ( endsWith(f.c_str(), ".ns") ) {
                /* note: we keep trailing "." so that when slave calls setClient(ns) everything is happy; e.g.,
                         valid namespaces must always have a dot, even though here it is just a placeholder not
                  	   a real one
                  	   */
                string dbname = string(f.c_str(), f.size() - 2);
                if ( dbname != "local." ) {
                    setClientTempNs(dbname.c_str());
                    logOp("db", dbname.c_str(), emptyObj);
                }
            }
            i++;
        }

        database = 0;
    }

    /* we have to log the db presence periodically as that "advertisement" will roll out of the log
       as it is of finite length.  also as we only do one db cloning per pass, we could skip over a bunch of
       advertisements and thus need to see them again later.  so this mechanism can actually be very slow to
       work, and should be improved.
    */
    void replMasterThread() {
        sleepsecs(15);
        logOurDbsPresence();

        // if you are testing, you might finish test and shutdown in less than 10
        // minutes yet not have done something in first 15 -- this is to exercise
        // this code some.
        sleepsecs(90);
        logOurDbsPresence();

        while ( 1 ) {
            logOurDbsPresence();
            sleepsecs(60 * 10);
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
        /* create an oplog collection, if it doesn't yet exist. */
        BSONObjBuilder b;
        double sz;
        if ( oplogSize != 0 )
            sz = (double) oplogSize;
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
        b.append("size", sz);
        b.appendBool("capped", 1);
        setClientTempNs("local.oplog.$main");
        string err;
        BSONObj o = b.done();
        userCreateNS("local.oplog.$main", o, err, false);
        database = 0;
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
            if ( slave )
                log(1) << "slave=true" << endl;
            slave = true;
            boost::thread repl_thread(replSlaveThread);
        }

        if ( master || replPair ) {
            if ( master  )
                log(1) << "master=true" << endl;
            master = true;
            createOplog();
            boost::thread mt(replMasterThread);
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
            NamespaceDetailsTransient &t = NamespaceDetailsTransient::get( logCollection.c_str() );
            if ( start ) {
                if ( t.logNS().empty() ) {
                    t.startLog();
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
