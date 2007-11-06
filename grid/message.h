// message.h

#pragma once

#include "../util/sock.h"

class Message;

class MessagingPort {
public:
	enum { DBPort = 27017 };

	MessagingPort();
	~MessagingPort();

	void init(int myUdpPort);

	/* it's assumed if you reuse a message object, that it doesn't cross MessagingPort's.
	   also, the Message data will go out of scope on the subsequent recv call. 
	*/
	bool recv(Message& m);
	void reply(Message& received, Message& response);
	bool call(SockAddr& to, Message& toSend, Message& response);
	void say(SockAddr& to, Message& toSend, int responseTo = -1);

private:
	UDPConnection conn;
	enum { BufSize = 64 * 1024 };
	char buf[BufSize];
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
	dbDelete = 2006
};

struct MsgData {
	int len; /* len of the msg, including this field */
	int reserved;
	int id; /* request/reply id's match... */
	int responseTo; /* id of the message we are responding to */
	int operation;
	char _data[4];

	int dataLen();
};
const int MsgDataHeaderSize = sizeof(MsgData) - 4;
inline int MsgData::dataLen() { return len - MsgDataHeaderSize; }

#pragma pack(pop)

class Message {
public:
	Message() { data = 0; freeIt = false; }
	~Message() { reset(); }

	SockAddr from;
	MsgData *data;

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

