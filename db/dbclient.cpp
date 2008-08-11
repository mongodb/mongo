// dbclient.cpp - connect to a Mongo database as a client, from C++

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

JSObj DBClientConnection::findOne(const char *ns, JSObj query, JSObj *fieldsToReturn) { 
	auto_ptr<DBClientCursor> c = 
		this->query(ns, query, 1, 0, fieldsToReturn);

	if( !c->more() )
		return JSObj();

	return c->next().copy();
}

bool DBClientConnection::connect(const char *serverAddress, string& errmsg) { 
	/* not reentrant! 
	   ok as used right now (we are in a big lock), but won't be later, so fix. */
  
  	int port = DBPort;

	string ip = hostbyname_nonreentrant(serverAddress);
	if( ip.empty() ) 
		ip = serverAddress;

	int idx = ip.find( ":" );
	if ( idx != string::npos ){
	  cout << "port string:" << ip.substr( idx ) << endl;
	  port = atoi( ip.substr( idx + 1 ).c_str() );
	  ip = ip.substr( 0 , idx );
	  ip = hostbyname_nonreentrant(ip.c_str());

	}
	if( ip.empty() ) 
		ip = serverAddress;
	
	cout << "port:" << port << endl;
	server = auto_ptr<SockAddr>(new SockAddr(ip.c_str(), port));
	if( !p.connect(*server) ) {
	        errmsg = string("couldn't connect to server ") + serverAddress + ' ' + ip;
		return false;
	}
	return true;
}

auto_ptr<DBClientCursor> DBClientConnection::query(const char *ns, JSObj query, int nToReturn, int nToSkip, JSObj *fieldsToReturn, bool sticky) {
	// see query.h for the protocol we are using here.
	BufBuilder b;
    int opts = sticky ? Option_CursorSticky : 0;
    b.append(opts);
	b.append(ns);
	b.append(nToSkip);
	b.append(nToReturn);
	query.appendSelfToBufBuilder(b);
	if( fieldsToReturn )
		fieldsToReturn->appendSelfToBufBuilder(b);
	Message toSend;
	toSend.setData(dbQuery, b.buf(), b.len());
	auto_ptr<Message> response(new Message());
	bool ok = p.call(toSend, *response);
	if( !ok )
		return auto_ptr<DBClientCursor>(0);

	auto_ptr<DBClientCursor> c(new DBClientCursor(p, response, opts));
	c->ns = ns;
	c->nToReturn = nToReturn;

	return c;
}

/* -- DBClientCursor ---------------------------------------------- */

void DBClientCursor::requestMore() { 
	assert( cursorId && pos == nReturned );

	BufBuilder b;
    b.append(opts);
	b.append(ns.c_str());
	b.append(nToReturn);
	b.append(cursorId);

	Message toSend;
	toSend.setData(dbGetMore, b.buf(), b.len());
	auto_ptr<Message> response(new Message());
	bool ok = p.call(toSend, *response);
	assert( ok );

	m = response;
	dataReceived();
}

void DBClientCursor::dataReceived() { 
	QueryResult *qr = (QueryResult *) m->data;
    if( qr->resultOptions() & ResultOption_CursorNotFound )
        dead = true;
    if( cursorId == 0 ) {
        // only set initially: we don't want to kill it on end of data 
        // if it's a sticky cursor
        cursorId = qr->cursorId;
    }
	nReturned = qr->nReturned;
	pos = 0;
	data = qr->data();
	assert( nReturned || cursorId == 0 );
}

bool DBClientCursor::more() { 
	if( pos < nReturned ) 
		return true;

	if( cursorId == 0 )
		return false;

	requestMore();
	return pos < nReturned;
}

JSObj DBClientCursor::next() {
	assert( more() );
	pos++;
	JSObj o(data);
	data += o.objsize();
	return o;
}

/* ------------------------------------------------------ */

// "./db testclient" to invoke
extern JSObj emptyObj;
void testClient() {
	cout << "testClient()" << endl;
	DBClientConnection c;
	string err;
	assert( c.connect("127.0.0.1", err) );
	auto_ptr<DBClientCursor> cursor = 
		c.query("foo.bar", emptyObj);
	DBClientCursor *cc = cursor.get();
	cout << "more: " << cc->more() << endl;
	while( cc->more() ) { 
		cout << cc->next().toString() << endl;
	}
}
