// message.h

#pragma once

#include "../util/sock.h"

class Message;
class MessagingPort; 
typedef WrappingInt MSGID;
const int DBPort = 27017;

class Listener { 
public:
	Listener(int p) : port(p) { } 
	void listen(); // never returns (start a thread)

	/* spawn a thread, etc., then return */
	virtual void accepted(MessagingPort *mp) = 0;
private:
	int port;
};

class AbstractMessagingPort { 
public:
	virtual void reply(Message& received, Message& response, MSGID responseTo) = 0; // like the reply below, but doesn't rely on received.data still being available
	virtual void reply(Message& received, Message& response) = 0;
};

class MessagingPort : public AbstractMessagingPort {
public:
	MessagingPort(int sock, SockAddr& farEnd);
	MessagingPort();
	~MessagingPort();

	void shutdown();

	bool connect(SockAddr& farEnd);

	/* it's assumed if you reuse a message object, that it doesn't cross MessagingPort's.
	   also, the Message data will go out of scope on the subsequent recv call. 
	*/
	bool recv(Message& m);
	void reply(Message& received, Message& response, MSGID responseTo);
	void reply(Message& received, Message& response);
	bool call(SockAddr& to, Message& toSend, Message& response);
	void say(SockAddr& to, Message& toSend, int responseTo = -1);

private:
	int sock;
public:
	SockAddr farEnd;
};

#pragma pack(push)
#pragma pack(1)

enum Operations { 
	opReply = 1,     /* reply. responseTo is set. */

	dbMsg = 1000,    /* generic msg command followed by a string */

	dbUpdate = 2001, /* update object */
	dbInsert = 2002,
//	dbGetByOID = 2003,
	dbQuery = 2004,
	dbGetMore = 2005,
	dbDelete = 2006,
	dbKillCursors = 2007
};

struct MsgData {
	int len; /* len of the msg, including this field */
	MSGID id; /* request/reply id's match... */
	int responseTo; /* id of the message we are responding to */
	int operation;
	char _data[4];

	int dataLen(); // len without header
};
const int MsgDataHeaderSize = sizeof(MsgData) - 4;
inline int MsgData::dataLen() { return len - MsgDataHeaderSize; }

#pragma pack(pop)

class Message {
public:
	Message() { data = 0; freeIt = false; }
    Message( void * _data , bool _freeIt ){ data = (MsgData*)_data; freeIt = _freeIt; };
	~Message() { reset(); }

	SockAddr from;
	MsgData *data;

	Message& operator=(Message& r) { 
		assert( data == 0 );
		data = r.data;
		assert( r.freeIt );
		r.freeIt = false;
		r.data = 0;
		freeIt = true;
		return *this;
	}

	void reset() {
		if( freeIt && data )
			free(data);
		data = 0; freeIt = false;
	}

	void setData(MsgData *d, bool _freeIt) {
		assert( data == 0 ); 
		freeIt = _freeIt;
		data = d;
	}
	void setData(int operation, const char *msgtxt) {
		setData(operation, msgtxt, strlen(msgtxt)+1);
	}
	void setData(int operation, const char *msgdata, int len) {
		assert(data == 0);
		int dataLen = len + sizeof(MsgData) - 4;
		MsgData *d = (MsgData *) malloc(dataLen);
		memcpy(d->_data, msgdata, len);
		d->len = dataLen;
		d->operation = operation;
		freeIt= true;
		data = d;
	}

private:
	bool freeIt;
};
