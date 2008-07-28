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
	if( last.secs == t )
		return OpTime(last.secs, last.i+1);
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
		uassert( e.type() == Number );
		syncedTo.asDouble() = e.number();
	}
}

JSObj Source::jsobj() {
	JSObjBuilder b;
	b.append("host", hostName);
	b.append("source", sourceName);
	b.append("syncedTo", syncedTo.asDouble());
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
	uassert( e.type() == Number );
	OpTime serverCurTime;
	serverCurTime.asDouble() = e.number();

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
//	boost::thread repl_thread(replMainThread);
}
