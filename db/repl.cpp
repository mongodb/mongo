// repl.cpp

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

extern JSObj emptyObj;
extern boost::mutex dbMutex;
auto_ptr<Cursor> findTableScan(const char *ns, JSObj& order);
bool userCreateNS(const char *ns, JSObj& j, string& err);

OpTime last((unsigned) time(0), 1);

OpTime OpTime::now() { 
	unsigned t = (unsigned) time(0);
	if( last.secs == t ) {
		last.i++;
		return last;
	}
	return OpTime(t, 1);
}

/* Cloner -----------------------------------------------------------
   makes copy of existing database.
*/

class Cloner: boost::noncopyable { 
	DBClientConnection conn;
	void copy(const char *collection);
public:
	Cloner() { }
	bool go(const char *masterHost, string& errmsg);
};

void Cloner::copy(const char *collection) {
	cout << "TEMP:" <<  collection << endl;

	auto_ptr<DBClientCursor> c( conn.query(collection, emptyObj) );
	assert( c.get() );
	while( c->more() ) { 
		JSObj js = c->next();
		theDataFileMgr.insert(collection, (void*) js.objdata(), js.objsize());
	}
}

bool Cloner::go(const char *masterHost, string& errmsg) { 
	if( string("localhost") == masterHost || string("127.0.0.1") == masterHost ) { 
		errmsg = "can't clone from self";
		return false;
	}
	if( !conn.connect(masterHost, errmsg) )
		return false;

	string ns = client->name + ".system.namespaces";

	auto_ptr<DBClientCursor> c( conn.query(ns.c_str(), emptyObj) );
	if( c.get() == 0 ) {
		errmsg = "query failed system.namespaces";
		return false;
	}

	while( c->more() ) { 
		JSObj collection = c->next();
		Element e = collection.findElement("name");
		assert( !e.eoo() );
		assert( e.type() == String );
		const char *name = e.valuestr();
		if( strstr(name, ".system.") || strchr(name, '$') )
			continue;
		JSObj options = collection.getObjectField("options");
		if( !options.isEmpty() ) {
			string err;
			userCreateNS(name, options, err);
		}
		copy(name);
	}

	// now build the indexes
	string system_indexes = client->name + ".system.indexes";
	copy(system_indexes.c_str());

	return true;
}

bool cloneFrom(const char *masterHost, string& errmsg)
{
	Cloner c;
	return c.go(masterHost, errmsg);
}

/* --------------------------------------------------------------*/

Source::Source(JSObj o) {
	hostName = o.getStringField("host");
	sourceName = o.getStringField("source");
	uassert( !hostName.empty() );
	uassert( !sourceName.empty() );
	Element e = o.getField("syncedTo");
	if( !e.eoo() ) {
		uassert( e.type() == Date );
		syncedTo.asDate() = e.date();
	}
}

JSObj Source::jsobj() {
	JSObjBuilder b;
	b.append("host", hostName);
	b.append("source", sourceName);
	b.appendDate("syncedTo", syncedTo.asDate());
	return b.doneAndDecouple();
}

void Source::updateOnDisk() { 
	JSObjBuilder b;
	b.append("host", hostName);
	b.append("source", sourceName);
	JSObj pattern = b.done();

	JSObj o = jsobj();

	stringstream ss;
	setClient("local.sources");
	updateObjects("local.sources", o, pattern, false, ss);
	client = 0;
}

void Source::cleanup(vector<Source*>& v) { 
	for( vector<Source*>::iterator i = v.begin(); i != v.end(); i++ )
		delete *i;
}

void Source::loadAll(vector<Source*>& v) { 
	setClient("local.sources");
	auto_ptr<Cursor> c = findTableScan("local.sources", emptyObj);
	while( c->ok() ) { 
		v.push_back( new Source(c->current()) );
		c->advance();
	}
	client = 0;
}

JSObj opTimeQuery = fromjson("{getoptime:1}");

/* note: not yet in mutex at this point. */
void Source::pull() { 
	log() << "pull source " << sourceName << '@' << hostName << endl;

//	if( syncedTo.isNull() ) { 
//	}

	DBClientConnection conn;
	string errmsg;
	if( !conn.connect(hostName.c_str(), errmsg) ) {
		cout << "  pull: cantconn " << errmsg << endl;
		return;
	}

	// get current mtime at the server.
	JSObj o = conn.findOne("admin.$cmd", opTimeQuery);
	Element e = o.findElement("optime");
	if( e.eoo() ) {
		cout << "  pull: failed to get curtime from master" << endl;
		cout << "  " << o.toString() << endl;
		return;
	}
	uassert( e.type() == Date );
	OpTime serverCurTime;
	serverCurTime.asDate() = e.date();

}

/* -- Logging of operations -------------------------------------*/

// cached copies of these...
NamespaceDetails *localOplogMainDetails = 0;
Client *localOplogClient = 0;

/* we write to local.opload.$main:
     { ts : ..., op: ..., ns: ..., o: ... }
   ts: an OpTime timestamp
   opstr: 
     'i' = insert
*/
void _logOp(const char *opstr, const char *ns, JSObj& obj, JSObj *o2, bool *bb) {
	if( strncmp(ns, "local.", 6) == 0 )
		return;

	Client *oldClient = client;
	if( localOplogMainDetails == 0 ) { 
		setClient("local.");
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
		b.appendBool("upsert", *bb);
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

void replMain() { 
	vector<Source*> sources;

	{
		lock lk(dbMutex);
		Source::loadAll(sources);
	}

	for( vector<Source*>::iterator i = sources.begin(); i != sources.end(); i++ ) { 
		(*i)->pull();
	}

	Source::cleanup(sources);
}

void replMainThread() { 
	while( 1 ) { 
		try { 
			replMain();
			sleepsecs(5);
		}
		catch( AssertionException ) { 
			problem() << "Assertion in replMainThread(): sleeping 5 minutes before retry" << endl;
			sleepsecs(300);
		}
	}
}

void startReplication() { 
	if( slave ) {
		boost::thread repl_thread(replMainThread);
	}

#if defined(_WIN32)
// temp, remove this.
master = true;
#endif

	if( master ) { 
		JSObjBuilder b;
		b.append("size", 254.0 * 1000 * 1000);
		b.appendBool("capped", 1);
		setClient("local.oplog.$main");
		string err;
		JSObj o = b.done();
		userCreateNS("local.oplog.$main", o, err);
		client = 0;
	}
}
