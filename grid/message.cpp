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

MessagingPort::MessagingPort(int _sock, SockAddr& _far) : sock(_sock), farEnd(_far) { }

MessagingPort::MessagingPort() {
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
		cout << "ERROR: connect(): connect() failed" << errno << endl;
		closesocket(sock); sock = -1;
		return false;
	}
	disableNagle(sock);
	return true;
}

bool MessagingPort::recv(Message& m) {
	mmm( cout << "*  recv() sock:" << this->sock << endl; )
	int len;

	int x = ::recv(sock, (char *) &len, 4, 0);
	if( x == 0 ) {
		cout << "MessagingPort::recv(): conn closed? " << farEnd.toString() << endl;
		m.reset();
		return false;
	}
	if( x < 0 ) { 
		cout << "MessagingPort::recv(): recv() error " << errno << ' ' << farEnd.toString()<<endl;
		m.reset();
		return false;
	}

	assert( x == 4 );

	int z = (len+1023)&0xfffffc00; assert(z>=len);
	MsgData *md = (MsgData *) malloc(z);
	md->len = len;
	char *p = (char *) &md->id;
	int left = len -4;
	while( 1 ) {
		x = ::recv(sock, p, left, 0);
		if( x == 0 ) {
			cout << "MessagingPort::recv(): conn closed? " << farEnd.toString() << endl;
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
	say(received.from, response, received.data->id);
}

bool MessagingPort::call(SockAddr& to, Message& toSend, Message& response) {
	mmm( cout << "*call()" << endl; )
	MSGID old = toSend.data->id;
	say(to, toSend);
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

void MessagingPort::say(SockAddr& to, Message& toSend, int responseTo) {
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
