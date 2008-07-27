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

/* message

   todo: authenticate; encrypt?
*/

#include "stdafx.h"
#include "message.h"
#include <time.h>
#include "../util/goodies.h"

// if you want trace output:
#define mmm(x)

/* listener ------------------------------------------------------------------- */

void Listener::listen() {
	SockAddr me(port);
	int sock = socket(AF_INET, SOCK_STREAM, 0);
	if( sock == INVALID_SOCKET ) {
		cout << "ERROR: listen(): invalid socket? " << errno << endl;
		return;
	}
	prebindOptions( sock );
	if( bind(sock, (sockaddr *) &me.sa, me.addressSize) != 0 ) { 
		cout << "listen(): bind() failed errno:" << errno << endl;
		if( errno == 98 )
			cout << "98 == addr already in use" << endl;
		closesocket(sock);
		return;
	}

	if( ::listen(sock, 128) != 0 ) { 
		cout << "listen(): listen() failed " << errno << endl;
		closesocket(sock);
		return;
	}

	SockAddr from;
	while( 1 ) { 
		int s = accept(sock, (sockaddr *) &from.sa, &from.addressSize);
		if( s < 0 ) {
			cout << "Listener: accept() returns " << s << " errno:" << errno << endl;
			continue;
		}
		disableNagle(s);
		cout << "Listener: connection accepted from " << from.toString() << endl;
		accepted( new MessagingPort(s, from) );
	}
}

/* messagingport -------------------------------------------------------------- */

MSGID NextMsgId;
struct MsgStart {
	MsgStart() {
		NextMsgId = (((unsigned) time(0)) << 16) ^ curTimeMillis();
		assert(MsgDataHeaderSize == 16);
	}
} msgstart;

set<MessagingPort*> ports;

void closeAllSockets() { 
	for( set<MessagingPort*>::iterator i = ports.begin(); i != ports.end(); i++ )
		(*i)->shutdown();
}

MessagingPort::MessagingPort(int _sock, SockAddr& _far) : sock(_sock), farEnd(_far) { 
	ports.insert(this);
}

MessagingPort::MessagingPort() {
	ports.insert(this);
	sock = -1;
}

void MessagingPort::shutdown() { 
	if( sock >= 0 ) { 
		closesocket(sock);
		sock = -1;
	}
}

MessagingPort::~MessagingPort() { 
	shutdown();
	ports.erase(this);
}

bool MessagingPort::connect(SockAddr& _far)
{
	farEnd = _far;

	sock = socket(AF_INET, SOCK_STREAM, 0);
	if( sock == INVALID_SOCKET ) {
		cout << "ERROR: connect(): invalid socket? " << errno << endl;
		return false;
	}
	if( ::connect(sock, (sockaddr *) &farEnd.sa, farEnd.addressSize) ) { 
		cout << "ERROR: connect(): connect() failed " << errno << ' ' << farEnd.getPort() << endl;
		closesocket(sock); sock = -1;
		return false;
	}
	disableNagle(sock);
	return true;
}

bool MessagingPort::recv(Message& m) {
	mmm( cout << "*  recv() sock:" << this->sock << endl; )
	int len = -1;

	char *lenbuf = (char *) &len;
	int lft = 4;
	while( 1 ) {
		int x = ::recv(sock, lenbuf, lft, 0);
		if( x == 0 ) {
			DEV cout << "MessagingPort::recv(): conn closed? " << farEnd.toString() << endl;
			m.reset();
			return false;
		}
		if( x < 0 ) { 
			cout << "MessagingPort::recv(): recv() error " << errno << ' ' << farEnd.toString()<<endl;
			m.reset();
			return false;
		}
		lft -= x;
		if( lft == 0 )
			break;
		lenbuf += x;
		cout << "MessagingPort::recv(): got " << x << " bytes wanted 4, lft=" << lft << endl;
		assert( lft > 0 );
	}

//	assert( x == 4 );

	if( len < 0 || len > 16000000 ) { 
		cout << "bad recv() len: " << len << '\n';
		return false;
	}
        
	int z = (len+1023)&0xfffffc00; assert(z>=len);
	MsgData *md = (MsgData *) malloc(z);
	md->len = len;
        
	if ( len <= 0 ){
		cout << "got a length of 0, something is wrong" << endl;
		return false;
	}

	char *p = (char *) &md->id;
	int left = len -4;
	while( 1 ) {
		int x = ::recv(sock, p, left, 0);
		if( x == 0 ) {
			DEV cout << "MessagingPort::recv(): conn closed? " << farEnd.toString() << endl;
			m.reset();
			return false;
		}
		if( x < 0 ) { 
			cout << "MessagingPort::recv(): recv() error " << errno << ' ' << farEnd.toString() << endl;
			m.reset();
			return false;
		}
		left -= x;
		p += x;
		if( left <= 0 )
			break;
	}

	m.setData(md, true);
	return true;
}

void MessagingPort::reply(Message& received, Message& response) {
	say(/*received.from, */response, received.data->id);
}

void MessagingPort::reply(Message& received, Message& response, MSGID responseTo) {
	say(/*received.from, */response, responseTo);
}

bool MessagingPort::call(Message& toSend, Message& response) {
	mmm( cout << "*call()" << endl; )
	MSGID old = toSend.data->id;
	say(/*to,*/ toSend);
	while( 1 ) {
		bool ok = recv(response);
		if( !ok )
			return false;
		//cout << "got response: " << response.data->responseTo << endl;
		if( response.data->responseTo == toSend.data->id ) 
			break;
		cout << "********************" << endl;
		cout << "ERROR: MessagingPort::call() wrong id got:" << response.data->responseTo << " expect:" << toSend.data->id << endl;
		cout << "  old:" << old << endl;
		cout << "  response msgid:" << response.data->id << endl;
		cout << "  response len:  " << response.data->len << endl;
		assert(false);
		response.reset();
	}
	mmm( cout << "*call() end" << endl; )
	return true;
}

void MessagingPort::say(Message& toSend, int responseTo) {
	mmm( cout << "*  say() sock:" << this->sock << " thr:" << GetCurrentThreadId() << endl; )
	MSGID msgid = NextMsgId;
	++NextMsgId;
	toSend.data->id = msgid;
	toSend.data->responseTo = responseTo;
	int x = ::send(sock, (char *) toSend.data, toSend.data->len, 0);
	if( x <= 0 ) { 
		cout << "MessagingPort::say: send() error " << errno << ' ' << farEnd.toString() << endl;
	}
}
