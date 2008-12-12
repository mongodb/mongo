// dbclient.cpp - connect to a Mongo database as a database, from C++

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
#include "../db/pdfile.h"
#include "dbclient.h"
#include "../util/builder.h"
#include "../db/jsobj.h"
#include "../db/query.h"
#include "../db/json.h"

/* --- dbclientcommands --- */

BSONObj ismastercmdobj = fromjson("{ismaster:1}");

BSONObj DBClientWithCommands::cmdIsMaster(bool& isMaster) {
    BSONObj o = findOne("admin.$cmd", ismastercmdobj);
    isMaster = (o.getIntField("ismaster") == 1);
    return o;
}

/* --- dbclientconnection --- */

BSONObj DBClientConnection::findOne(const char *ns, BSONObj query, BSONObj *fieldsToReturn, int queryOptions) { 
	auto_ptr<DBClientCursor> c = 
		this->query(ns, query, 1, 0, fieldsToReturn, queryOptions);

    massert( "DBClientConnection::findOne: transport error", c.get() );

	if( !c->more() )
		return BSONObj();

	return c->next().copy();
}

bool DBClientConnection::connect(const char *_serverAddress, string& errmsg) { 
    serverAddress = _serverAddress;

  	int port = DBPort;
	string ip = hostbyname(_serverAddress);
	if( ip.empty() ) 
		ip = serverAddress;

	unsigned int idx = ip.find( ":" );
	if ( idx != string::npos ){
	  //cout << "port string:" << ip.substr( idx ) << endl;
	  port = atoi( ip.substr( idx + 1 ).c_str() );
	  ip = ip.substr( 0 , idx );
	  ip = hostbyname(ip.c_str());

	}
	if( ip.empty() ) 
		ip = serverAddress;
	
    // we keep around SockAddr for connection life -- maybe MessagingPort
    // requires that?
	server = auto_ptr<SockAddr>(new SockAddr(ip.c_str(), port));
    p = auto_ptr<MessagingPort>(new MessagingPort());

	if( !p->connect(*server) ) {
        errmsg = string("couldn't connect to server ") + serverAddress + ' ' + ip;
        failed = true;
		return false;
	}
	return true;
}

void DBClientConnection::checkConnection() { 
    if( !failed ) 
        return;
    if( lastReconnectTry && time(0)-lastReconnectTry < 2 )
        return;
    if( !autoReconnect )
        return;

    lastReconnectTry = time(0);
    log() << "trying reconnect to " << serverAddress << endl;
    string errmsg;
    string tmp = serverAddress;
    failed = false;
    if( !connect(tmp.c_str(), errmsg) )
        log() << "reconnect " << serverAddress << " failed " << errmsg << endl;
    else
        log() << "reconnect " << serverAddress << " ok" << endl;
}

auto_ptr<DBClientCursor> DBClientConnection::query(const char *ns, BSONObj query, int nToReturn, int nToSkip, BSONObj *fieldsToReturn, int queryOptions) {
    checkConnection();

	// see query.h for the protocol we are using here.
	BufBuilder b;
    int opts = queryOptions;
    assert( (opts&Option_ALLMASK) == opts );
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
    if( !p->call(toSend, *response) ) {
        failed = true;
		return auto_ptr<DBClientCursor>(0);
    }

	auto_ptr<DBClientCursor> c(new DBClientCursor(this, *p.get(), response, opts));
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
    if( !p.call(toSend, *response) ) {
        conn->failed = true;
        massert("dbclient error communicating with server", false);
    }

	m = response;
	dataReceived();
}

void DBClientCursor::dataReceived() { 
	QueryResult *qr = (QueryResult *) m->data;
	if( qr->resultFlags() & ResultFlag_CursorNotFound ) {
		// cursor id no longer valid at the server.
		assert( qr->cursorId == 0 );
		cursorId = 0; // 0 indicates no longer valid (dead)
	}
    if( cursorId == 0 ) {
        // only set initially: we don't want to kill it on end of data 
        // if it's a tailable cursor
        cursorId = qr->cursorId;
    }
	nReturned = qr->nReturned;
	pos = 0;
	data = qr->data();

    /* check for errors.  the only one we really care about at 
       this stage is "not master" */
    if( conn->clientPaired && nReturned ) {
        BSONObj o(data);
        BSONElement e = o.firstElement();
        if( strcmp(e.fieldName(), "$err") == 0 && 
            e.type() == String && strncmp(e.valuestr(), "not master", 10) == 0 ) {
                conn->clientPaired->isntMaster();
        }
    }

	/* this assert would fire the way we currently work:
	    assert( nReturned || cursorId == 0 );
    */
}

bool DBClientCursor::more() { 
	if( pos < nReturned ) 
		return true;

	if( cursorId == 0 )
		return false;

	requestMore();
	return pos < nReturned;
}

BSONObj DBClientCursor::next() {
	assert( more() );
	pos++;
	BSONObj o(data);
	data += o.objsize();
	return o;
}

/* ------------------------------------------------------ */

// "./db testclient" to invoke
extern BSONObj emptyObj;
void testClient() {
	cout << "testClient()" << endl;
//	DBClientConnection c(true);
    DBClientPaired c;
	string err;
    if( !c.connect("10.211.55.2", "1.2.3.4") ) {
//    if( !c.connect("10.211.55.2", err) ) {
        cout << "testClient: connect() failed" << endl;
    }
    else { 
        // temp:
        cout << "test query returns: " << c.findOne("foo.bar", fromjson("{}")).toString() << endl;
    }
again:
	cout << "query foo.bar..." << endl;
	auto_ptr<DBClientCursor> cursor = 
		c.query("foo.bar", emptyObj, 0, 0, 0, Option_CursorTailable);
	DBClientCursor *cc = cursor.get();
    if( cc == 0 ) { 
        cout << "query() returned 0, sleeping 10 secs" << endl;
        sleepsecs(10);
        goto again;
    }
	while( 1 ) {
		bool m;
        try { 
            m = cc->more();
        } catch(AssertionException&) { 
            cout << "more() asserted, sleeping 10 sec" << endl;
            goto again;
        }
		cout << "more: " << m << " dead:" << cc->isDead() << endl;
		if( !m ) {
			if( cc->isDead() )
				cout << "cursor dead, stopping" << endl;
			else { 
				cout << "Sleeping 10 seconds" << endl;
				sleepsecs(10);
				continue;
			}
			break;
		}
		cout << cc->next().toString() << endl;
	}
}

/* --- class dbclientpaired --- */

string DBClientPaired::toString() { 
    stringstream ss;
    ss << "state: " << master << '\n';
    ss << "left:  " << left.toStringLong() << '\n';
    ss << "right: " << right.toStringLong() << '\n';
    return ss.str();
}

DBClientPaired::DBClientPaired() : 
  left(true), right(true)
{ 
    master = NotSetL;
}

/* find which server, the left or right, is currently master mode */
void DBClientPaired::_checkMaster() {
    for( int retry = 0; retry < 2; retry++ ) {
        int x = master;
        for( int pass = 0; pass < 2; pass++ ) {
            DBClientConnection& c = x == 0 ? left : right;
            try {
                bool im;
                BSONObj o = c.cmdIsMaster(im);
                if( retry ) 
                    log() << "checkmaster: " << c.toString() << ' ' << o.toString() << '\n';
                if( im ) {
                    master = (State) (x + 2);
                    return;
                }
            }
            catch(AssertionException&) {
                if( retry ) 
                    log() << "checkmaster: caught exception " << c.toString() << '\n';
            }
            x = x^1;
        }
        sleepsecs(1);
    }

    uassert("checkmaster: no master found", false);
}

inline DBClientConnection& DBClientPaired::checkMaster() { 
    if( master > NotSetR ) {
        // a master is selected.  let's just make sure connection didn't die
        DBClientConnection& c = master == Left ? left : right;
        if( !c.isFailed() )
            return c;
        // after a failure, on the next checkMaster, start with the other 
        // server -- presumably it took over. (not critical which we check first, 
        // just will make the failover slightly faster if we guess right)
        master = master == Left ? NotSetR : NotSetL;
    }

    _checkMaster();
    assert( master > NotSetR );
    return master == Left ? left : right;
}

bool DBClientPaired::connect(const char *serverHostname1, const char *serverHostname2) { 
    string errmsg;
    bool l = left.connect(serverHostname1, errmsg);
    bool r = right.connect(serverHostname2, errmsg);
    master = l ? NotSetL : NotSetR;
    if( !l && !r ) // it would be ok to fall through, but checkMaster will then try an immediate reconnect which is slow
        return false;
    try { checkMaster(); }
    catch(UserAssertionException&) { 
        return false;
    }
    return true;
}

auto_ptr<DBClientCursor> DBClientPaired::query(const char *a, BSONObj b, int c, int d, 
                                               BSONObj *e, int f) 
{
    return checkMaster().query(a,b,c,d,e,f);
}

BSONObj DBClientPaired::findOne(const char *a, BSONObj b, BSONObj *c, int d) {
    return checkMaster().findOne(a,b,c,d);
}
