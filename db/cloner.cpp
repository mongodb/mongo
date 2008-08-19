// cloner.cpp - copy a database (export/import basically)

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
#include "pdfile.h"
#include "dbclient.h"
#include "../util/builder.h"
#include "jsobj.h"
#include "query.h"

extern int port;
bool userCreateNS(const char *ns, JSObj& j, string& err);

class Cloner: boost::noncopyable { 
	DBClientConnection conn;
	void copy(const char *collection);
public:
	Cloner() { }
	bool go(const char *masterHost, string& errmsg);
};

void Cloner::copy(const char *collection) {
	auto_ptr<DBClientCursor> c( conn.query(collection, emptyObj) );
	assert( c.get() );
	while( c->more() ) { 
		JSObj js = c->next();
		theDataFileMgr.insert(collection, (void*) js.objdata(), js.objsize());
	}
}

bool Cloner::go(const char *masterHost, string& errmsg) { 
	if( (string("localhost") == masterHost || string("127.0.0.1") == masterHost) && port == DBPort ) { 
		errmsg = "can't clone from self (localhost).  sources configuration may be wrong.";
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
