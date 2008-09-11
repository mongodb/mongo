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

#include "stdafx.h"
#include "jsobj.h"
#include "../util/goodies.h"
#include "repl.h"
#include "../grid/message.h"
#include "dbclient.h"
#include "pdfile.h"
#include "query.h"
#include "json.h"
#include "db.h"
#include "commands.h"

extern boost::mutex dbMutex;
auto_ptr<Cursor> findTableScan(const char *ns, JSObj& order);
bool userCreateNS(const char *ns, JSObj& j, string& err);
int _updateObjects(const char *ns, JSObj updateobj, JSObj pattern, bool upsert, stringstream& ss, bool logOp=false);
bool _runCommands(const char *ns, JSObj& jsobj, stringstream& ss, BufBuilder &b, JSObjBuilder& anObjBuilder);
bool cloneFrom(const char *masterHost, string& errmsg);
void ensureHaveIdIndex(const char *ns);

#include "replset.h"

ReplPair *replPair = 0;

class CmdIsMaster : public Command { 
public:
    CmdIsMaster() : Command("ismaster") { }

    virtual bool run(const char *ns, JSObj& cmdObj, string& errmsg, JSObjBuilder& result) {
        int x = -2;
        if( replPair ) {
            x = replPair->state;
            result.append("ismaster", x);
            result.append("remote", replPair->remote);
        }
        else { 
            result.append("ismaster", x);
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

    virtual bool adminOnly() { return true; }

    virtual bool run(const char *ns, JSObj& cmdObj, string& errmsg, JSObjBuilder& result) {
        if( replPair == 0 ) { 
            problem() << "got negotiatemaster cmd but we are not in paired mode." << endl;
            errmsg = "not paired";
            return false;
        }

        int was = cmdObj.getIntField("i_was");
        string myname = cmdObj.getStringField("your_name");
        if( myname.empty() || was < -1 ) {
            errmsg = "your_name/i_was not specified";
            return false;
        }

        int me, you;
        if( was == replPair->state ) { 
            if( replPair->dominant(myname) ) {
                me=1;you=0;
            }
            else {
                me=0;you=1;
            }
        }
        else if( was == 1 ) { 
            me=0;you=1;
        }
        else { 
            me=1;you=0;
        }

        replPair->state = me;
        result.append("you_are", you);
        result.append("i_am", me);

        return true;
    }
} cmdnegotiatemaster;

void ReplPair::negotiate(DBClientConnection *conn) { 
    JSObjBuilder b;
    b.append("negotiatemaster",1);
    b.append("i_was", state);
    b.append("your_name", remoteHost);
    JSObj cmd = b.done();
    JSObj res = conn->findOne("admin.$cmd", cmd);
    if( res.getIntField("ok") != 1 ) { 
        problem() << "negotiate fails: " << res.toString() << '\n';
        return;
    }
    int x = res.getIntField("you_are");
    if( x != 0 && x != 1 ) { 
        problem() << "negotiate: bad you_are value " << res.toString() << endl;
        return;
    }
    setMaster(x);
}

OpTime last(0, 0);

OpTime OpTime::now() { 
	unsigned t = (unsigned) time(0);
	if( last.secs == t ) {
		last.i++;
		return last;
	}
	last = OpTime(t, 1);
	return last;
}

struct TestOpTime { 
	TestOpTime() {
		OpTime t;
		for( int i = 0; i < 10; i++ ) { 
			OpTime s = OpTime::now();
			assert( s != t );
			t = s;
		}
		OpTime q = t;
		assert( q == t );
		assert( !(q != t) );
	}
} testoptime;

int test2() { 
	return 0;
}

/* --------------------------------------------------------------*/

ReplSource::ReplSource() {
	nClonedThisPass = 0; 
	paired = false;
	sourceName = "main";
}

ReplSource::ReplSource(JSObj o) : nClonedThisPass(0) {
	paired = false;
	only = o.getStringField("only");
	hostName = o.getStringField("host");
	sourceName = o.getStringField("source");
	uassert( "'host' field not set in sources collection object", !hostName.empty() );
	uassert( "'source' field not set in sources collection object", !sourceName.empty() );
	Element e = o.getField("syncedTo");
	if( !e.eoo() ) {
		uassert( "bad sources 'syncedTo' field value", e.type() == Date );
		OpTime tmp( e.date() );
		syncedTo = tmp;
		//syncedTo.asDate() = e.date();
	}

	JSObj dbsObj = o.getObjectField("dbs");
	if( !dbsObj.isEmpty() ) {
		JSElemIter i(dbsObj);
		while( 1 ) { 
			Element e = i.next();
			if( e.eoo() ) 
				break;
			dbs.insert( e.fieldName() );
		}
	}
}

/* Turn our C++ Source object into a JSObj */
JSObj ReplSource::jsobj() {
	JSObjBuilder b;
	b.append("host", hostName);
	b.append("source", sourceName);
	if( !only.empty() )
		b.append("only", only);
	b.appendDate("syncedTo", syncedTo.asDate());

	JSObjBuilder dbs_builder;
	for( set<string>::iterator i = dbs.begin(); i != dbs.end(); i++ ) {
		dbs_builder.appendBool(i->c_str(), 1);
	}
	b.append("dbs", dbs_builder.done());

	return b.doneAndDecouple();
}

void ReplSource::save() { 
	JSObjBuilder b;
	b.append("host", hostName);
	b.append("source", sourceName);
	JSObj pattern = b.done();

	JSObj o = jsobj();

	stringstream ss;
	setClient("local.sources");
	//cout << o.toString() << endl;
	//cout << pattern.toString() << endl;
	int u = _updateObjects("local.sources", o, pattern, true/*upsert for pair feature*/, ss);
	assert( u == 1 || u == 4 );
	client = 0;
}

void ReplSource::cleanup(vector<ReplSource*>& v) { 
	for( vector<ReplSource*>::iterator i = v.begin(); i != v.end(); i++ )
		delete *i;
}

static void addSourceToList(vector<ReplSource*>&v, ReplSource& s, vector<ReplSource*>&old) { 
	for( vector<ReplSource*>::iterator i = old.begin(); i != old.end();  ) {
		if( s == **i ) {
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
	setClient("local.sources");
	auto_ptr<Cursor> c = findTableScan("local.sources", emptyObj);
	while( c->ok() ) { 
		ReplSource tmp(c->current());
        if( replPair && tmp.hostName == replPair->remote && tmp.sourceName == "main" ) {
			gotPairWith = true;
            tmp.paired = true;
        }
		addSourceToList(v, tmp, old);
		c->advance();
	}
	client = 0;

	if( !gotPairWith && replPair ) {
		/* add the --pairwith server */
		ReplSource *s = new ReplSource();
		s->paired = true;
		s->hostName = replPair->remote;
		v.push_back(s);
	}

    for( vector<ReplSource*>::iterator i = old.begin(); i != old.end(); i++ )
        delete *i;
}

JSObj opTimeQuery = fromjson("{getoptime:1}");

bool ReplSource::resync(string db) {
	{
		log() << "resync: dropping database " << db << endl;
		string dummyns = db + ".";
		assert( client->name == db );
		dropDatabase(dummyns.c_str());
		setClientTempNs(dummyns.c_str());
	}

	{
		log() << "resync: cloning database " << db << endl;
		//Cloner c;
		string errmsg;
		bool ok = cloneFrom(hostName.c_str(), errmsg);
		//bool ok = c.go(hostName.c_str(), errmsg);
		if( !ok ) { 
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

/* local.$oplog.main is of the form:
     { ts: ..., op: <optype>, ns: ..., o: <obj> , o2: <extraobj>, b: <boolflag> } 
     ...
   see logOp() comments.
*/
void ReplSource::sync_pullOpLog_applyOperation(JSObj& op) {
	char clientName[MaxClientLen];
	const char *ns = op.getStringField("ns");
	nsToClient(ns, clientName);

	if( !only.empty() && only != clientName )
		return;

	bool newDb = dbs.count(clientName) == 0;
	if( newDb && nClonedThisPass ) { 
		/* we only clone one database per pass, even if a lot need done.  This helps us 
		   avoid overflowing the master's transaction log by doing too much work before going 
		   back to read more transactions. (Imagine a scenario of slave startup where we try to 
		   clone 100 databases in one pass.)
		*/
		addDbNextPass.insert(clientName);
		return;
	}

	dblock lk;
	bool justCreated = setClientTempNs(ns);

	if( justCreated || /* datafiles were missing.  so we need everything, no matter what sources object says */
	    newDb ) /* if not in dbs, we've never synced this database before, so we need everything */
	{
		if( paired && !justCreated ) { 
			/* the other half of our pair has some operations. yet we already had a db on our 
			   disk even though the db in question is not listed in the source.  this is normal 
			   near the beginning of paired operation. 

			   todo: we should echo back an optime on the initial cloning, and then we know 
			   we are safely in sync, and if we get here without that, we can then error out.
			   */
			log() << "TEMP: pair: assuming we have the historical image for: " << 
				clientName << ". add extra checks here." << endl;
			dbs.insert(clientName);
		}
		else { 
			nClonedThisPass++;
			resync(client->name);
		}
		addDbNextPass.erase(clientName);
	}

	stringstream ss;
	const char *opType = op.getStringField("op");
	JSObj o = op.getObjectField("o");
	try { 
		if( *opType == 'i' ) {
			const char *p = strchr(ns, '.');
			if( p && strcmp(p, ".system.indexes") == 0 ) { 
				// updates aren't allowed for indexes -- so we will do a regular insert. if index already 
				// exists, that is ok.
				theDataFileMgr.insert(ns, (void*) o.objdata(), o.objsize());
			}
			else { 
				// do upserts for inserts as we might get replayed more than once
				OID *oid = o.getOID();
				if( oid == 0 ) {
					_updateObjects(ns, o, o, true, ss);
				}
				else { 
					JSObjBuilder b;
					b.appendOID("_id", oid);
					RARELY ensureHaveIdIndex(ns); // otherwise updates will be super slow
					_updateObjects(ns, o, b.done(), true, ss);
				}
			}
		}
		else if( *opType == 'u' ) { 
			RARELY ensureHaveIdIndex(ns); // otherwise updates will be super slow
			_updateObjects(ns, o, op.getObjectField("o2"), op.getBoolField("b"), ss);
		}
		else if( *opType == 'd' ) { 
			if( opType[1] == 0 )
				deleteObjects(ns, o, op.getBoolField("b"));
			else
				assert( opType[1] == 'b' ); // "db" advertisement
		}
		else { 
			BufBuilder bb;
			JSObjBuilder ob;
			assert( *opType == 'c' );
			_runCommands(ns, o, ss, bb, ob);
		}
	}
	catch( UserAssertionException e ) { 
		log() << "sync: caught user assertion " << e.msg << '\n';
	}
	client = 0;
}

/* note: not yet in mutex at this point. */
void ReplSource::sync_pullOpLog() { 
	string ns = string("local.oplog.$") + sourceName;

	bool tailing = true;
	DBClientCursor *c = cursor.get();
	if( c && c->isDead() ) { 
		log() << "pull:   old cursor isDead, initiating a new one\n";
		c = 0;
	}

	if( c == 0 ) {
		JSObjBuilder q;
		q.appendDate("$gte", syncedTo.asDate());
		JSObjBuilder query;
		query.append("ts", q.done());
		// query = { ts: { $gte: syncedTo } }

		cursor = conn->query( ns.c_str(), query.done(), 0, 0, 0, Option_CursorTailable );
		c = cursor.get();
		tailing = false;
	}

    if( c == 0 ) { 
        problem() << "pull:   dbclient::query returns null (conn closed?)" << endl;
        resetConnection();
        sleepsecs(3);
        return;
    }

	// show any deferred database creates from a previous pass
	{
		set<string>::iterator i = addDbNextPass.begin();
		if( i != addDbNextPass.end() ) { 
			JSObjBuilder b;
			b.append("ns", *i + '.');
			b.append("op", "db");
			JSObj op = b.done();
			sync_pullOpLog_applyOperation(op);
		}
	}

	if( !c->more() ) { 
		if( tailing ) 
			; //log() << "pull:   " << ns << " no new activity\n";
		else
			log() << "pull:   " << ns << " oplog is empty\n";
		sleepsecs(3);
		return;
	}

    int n = 0;
	JSObj op = c->next();
	Element ts = op.findElement("ts");
    if( ts.type() != Date ) { 
        problem() << "pull: bad object read from remote oplog: " << op.toString() << '\n';
        assert(false);
    }
	OpTime t( ts.date() );
	bool initial = syncedTo.isNull();
	if( initial || tailing ) { 
		if( tailing ) { 
			assert( syncedTo < t );
		} 
		else {
			log() << "pull:   initial run\n";
		}
        {
            sync_pullOpLog_applyOperation(op);
            n++;
        }
	}
	else if( t != syncedTo ) { 
		log() << "pull:   t " << t.toString() << " != syncedTo " << syncedTo.toString() << '\n';
        log() << "pull:    data too stale, halting replication" << endl;
		assert( syncedTo < t );
		throw SyncException();
	}
    else { 
        /* t == syncedTo, so the first op was applied previously, no need to redo it. */
    }

	// apply operations
	{
		while( 1 ) {
			if( !c->more() ) {
				log() << "pull:   applied " << n << " operations" << endl;
				syncedTo = t;
				dblock lk;
				save(); // note how far we are synced up to now
				break;
			}
			/* todo: get out of the mutex for the next()? */
			JSObj op = c->next();
			ts = op.findElement("ts");
			assert( ts.type() == Date );
			OpTime last = t;
			OpTime tmp( ts.date() );
			t = tmp;
			if( !( last < t ) ) { 
				problem() << "sync error: last " << last.toString() << " >= t " << t.toString() << endl;
				uassert("bad 'ts' value in sources", false);
			}

			sync_pullOpLog_applyOperation(op);
			n++;
		}
	}
}

/* note: not yet in mutex at this point. 
   returns true if everything happy.  return false if you want to reconnect.
*/
bool ReplSource::sync() { 
	log() << "pull: " << sourceName << '@' << hostName << endl;
	nClonedThisPass = 0;

	if( (string("localhost") == hostName || string("127.0.0.1") == hostName) && port == DBPort ) { 
        log() << "pull:   can't sync from self (localhost). sources configuration may be wrong." << endl;
		sleepsecs(5);
        return false;
    }

	if( conn.get() == 0 ) {
		conn = auto_ptr<DBClientConnection>(new DBClientConnection());
		string errmsg;
		if( !conn->connect(hostName.c_str(), errmsg) ) {
            if( replPair && paired ) {
                assert( startsWith(hostName.c_str(), replPair->remoteHost.c_str()) );
                replPair->setMaster(1);
            }
			resetConnection();
			log() << "pull:   cantconn " << errmsg << endl;
            sleepsecs(1);
			return false;
		}
	}

    if( paired ) 
        replPair->negotiate(conn.get());

/*
	// get current mtime at the server.
	JSObj o = conn->findOne("admin.$cmd", opTimeQuery);
	Element e = o.findElement("optime");
	if( e.eoo() ) {
		log() << "pull:   failed to get cur optime from master" << endl;
		log() << "        " << o.toString() << endl;
		return false;
	}
	uassert( e.type() == Date );
	OpTime serverCurTime;
	serverCurTime.asDate() = e.date();
*/
	sync_pullOpLog();
	return true;
}

/* -- Logging of operations -------------------------------------*/

// cached copies of these...
NamespaceDetails *localOplogMainDetails = 0;
Client *localOplogClient = 0;

/* we write to local.opload.$main:
     { ts : ..., op: ..., ns: ..., o: ... }
   ts: an OpTime timestamp
   op: 
    "i" insert
    "u" update
    "d" delete
    "c" db cmd
   bb:
     if not null, specifies a boolean to pass along to the other side as b: param.
     used for "justOne" or "upsert" flags on 'd', 'u'
*/
void _logOp(const char *opstr, const char *ns, JSObj& obj, JSObj *o2, bool *bb) {
	if( strncmp(ns, "local.", 6) == 0 )
		return;

	Client *oldClient = client;
	if( localOplogMainDetails == 0 ) { 
		setClientTempNs("local.");
		localOplogClient = client;
		localOplogMainDetails = nsdetails("local.oplog.$main");
	}
	client = localOplogClient;

	/* we jump through a bunch of hoops here to avoid copying the obj buffer twice -- 
	   instead we do a single copy to the destination position in the memory mapped file.
    */

	JSObjBuilder b;
	b.appendDate("ts", OpTime::now().asDate());
	b.append("op", opstr);
	b.append("ns", ns);
	if( bb ) 
		b.appendBool("b", *bb);
	if( o2 )
		b.append("o2", *o2);
	JSObj partial = b.done();
	int posz = partial.objsize();
	int len = posz + obj.objsize() + 1 + 2 /*o:*/;

	Record *r = theDataFileMgr.fast_oplog_insert(localOplogMainDetails, "local.oplog.$main", len);

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

	client = oldClient;
}

/* --------------------------------------------------------------*/

/*
TODO:
_ source has autoptr to the cursor
_ reuse that cursor when we can
*/

void replMain() { 
	vector<ReplSource*> sources;

	while( 1 ) { 
		{	
			dblock lk;
			ReplSource::loadAll(sources);
		}
		
		if( sources.empty() )
			sleepsecs(20);
		
		for( vector<ReplSource*>::iterator i = sources.begin(); i != sources.end(); i++ ) {
			ReplSource *s = *i;	
			bool ok = false;	
			try {
				ok = s->sync();
			}
			catch( SyncException ) {
				log() << "caught SyncException, sleeping 1 minutes" << endl;
				sleepsecs(60);
			}
            catch( AssertionException ) { 
                log() << "replMain caught AssertionException, sleeping 1 minutes" << endl;
                sleepsecs(60);
            }
			if( !ok ) 
				s->resetConnection();
		}

        sleepsecs(3);
	}

	ReplSource::cleanup(sources);
}

int debug_stop_repl = 0;

void replSlaveThread() { 
    sleepsecs(1);
	while( 1 ) { 
		try { 
			replMain();
			if( debug_stop_repl )
				break;
			sleepsecs(5);
		}
		catch( AssertionException ) { 
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

	int k = 0;
    while( i != end ) {
      path p = *i;
	  string f = p.leaf();
	  if( endsWith(f.c_str(), ".ns") ) {
		  /* note: we keep trailing "." so that when slave calls setClient(ns) everything is happy; e.g., 
		           valid namespaces must always have a dot, even though here it is just a placeholder not 
				   a real one
				   */
		  string dbname = string(f.c_str(), f.size() - 2);
		  if( dbname != "local." ) {
			  logOp("db", dbname.c_str(), emptyObj);
		  }
	  }
      i++;
    }
}

/* we have to log the db presence periodically as that "advertisement" will roll out of the log
   as it is of finite length.  also as we only do one db cloning per pass, we could skip over a bunch of 
   advertisements and thus need to see them again later.  so this mechanism can actually be very slow to 
   work, and should be improved.
*/
void replMasterThread() { 
    sleepsecs(15);
	while( 1 ) {
		logOurDbsPresence();
		sleepsecs(60 * 10);
	}
}

void startReplication() { 
	if( slave || replPair ) {
		if( slave )
			log() << "slave=true" << endl;
		slave = true;
		boost::thread repl_thread(replSlaveThread);
	}

	if( master || replPair ) {
		if( master )
			log() << "master=true" << endl;
		master = true;
		{
			dblock lk;
			/* create an oplog collection, if it doesn't yet exist. */
			JSObjBuilder b;
			b.append("size", 254.0 * 1000 * 1000);
			b.appendBool("capped", 1);
			setClientTempNs("local.oplog.$main");
			string err;
			JSObj o = b.done();
			userCreateNS("local.oplog.$main", o, err);
			client = 0;
		}

		boost::thread mt(replMasterThread);
	}
}

/* called from main at server startup */
void pairWith(const char *remoteEnd) {
	replPair = new ReplPair(remoteEnd);
}
