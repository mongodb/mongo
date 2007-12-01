/* message

   todo: authenticate; encrypt?
*/

#include "stdafx.h"
#include "message.h"
#include <time.h>
#include "../util/goodies.h"
#include "protocol.h"
#include "protoimpl.h"

MSGID NextMsgId;
struct MsgStart {
	MsgStart() {
		NextMsgId = (((unsigned) time(0)) << 16) ^ curTimeMillis();
		assert(MsgDataHeaderSize == 16);
		assert(sizeof(Fragment) == FragHeader+16);
	}
} msgstart;

int nextChannel = curTimeMillis() & 0x3fff;

MessagingPort::MessagingPort(int c) { 
	myChannel = c;
	if( c == AUTOASSIGNCHANNEL ) {
		myChannel = nextChannel++;
		if( myChannel > 0x7000 ) {
			cout << "warning: myChannel is getting high and there is no checks on this!" << endl;
			assert(false);
		}
	}
	pc = 0;
}

MessagingPort::~MessagingPort() { 
	delete pc; pc = 0;
}

extern boost::mutex biglock;

void MessagingPort::init(int myUdpPort, SockAddr *farEnd) {
	SockAddr me(myUdpPort);
	if( !conn.init(me) ) {
		cout << "/conn init failure in MessagingPort::init " << myUdpPort << endl;
		exit(2);
	}
	EndPoint ep;
	ep.channel = myChannel;
	ep.sa = me;
	cout << "/Initializing MessagingPort " << ep.toString() << endl;
	pc = new ProtocolConnection(conn, ep, farEnd);
}

bool MessagingPort::recv(Message& m) {
	MR *mr = pc->cr.recv();

	Fragment *first = mr->f[0]->internals;
	m.channel = first->channel;
	m.from = mr->from.sa;
	MsgData *somd = first->startOfMsgData();
	int totalLen = somd->len;

	if( mr->n() == 1 ) { 
		// only one fragment, so use its buffer instead of making
		// a copy
		m.setData(somd, false);
		return true;
	}

	MsgData *fullmsg = (MsgData *) malloc(totalLen);
	char *p = (char *) fullmsg;
	for( int i = 0; i < mr->n(); i++ ) { 
		Fragment *frag = mr->f[i]->internals;
		memcpy(p, frag->data, frag->fragmentDataLen());
		p += frag->fragmentDataLen();
	}
	assert( p - ((char *)fullmsg) == totalLen );

	mr->freeFrags();
	m.setData(fullmsg, true);

	return true;
}

void MessagingPort::reply(Message& received, Message& response) {
	say(received.channel, received.from, response, received.data->id);
}

bool MessagingPort::call(SockAddr& to, Message& toSend, Message& response) {
	assert( myChannel >= 0 );
	say(myChannel, to, toSend);
	while( 1 ) {
		bool ok = recv(response);
		if( !ok )
			return false;
		//cout << "got response: " << response.data->responseTo << endl;
		if( response.data->responseTo == toSend.data->id ) 
			break;
		cout << "warning: MessagingPort::call() wrong id, skipping. got:" << response.data->responseTo << " expect:" << toSend.data->id << endl;
		response.reset();
	}
	return true;
}

void MessagingPort::say(int channel, SockAddr& to, Message& toSend, int responseTo) {
	MSGID msgid = NextMsgId;
	++NextMsgId;
	toSend.data->id = msgid;
	toSend.data->responseTo = responseTo;
	toSend.channel = channel; assert(channel>0);

	EndPoint ep;
	ep.channel = channel;
	ep.sa = to;
	MS *ms = new MS(pc, ep, msgid);

	int mss = conn.mtu(to) - FragHeader;
	int left = toSend.data->len;
	cout << "say() len:" << left << endl;
	int i = 0;
	char *p = (char *) toSend.data;
	while( left>0 ) { 
		int datalen = left>=mss ? mss : left;
		Fragment *frag = (Fragment *)malloc(mss+FragHeader);
		frag->msgId = msgid;
		frag->channel = channel;
		frag->fragmentLen = datalen + FragHeader;
		frag->fragmentNo = i++;
		memcpy(frag->data, p, datalen);
		p += datalen;
		ms->fragments.push_back(new F(frag));
		left -= datalen;
	}

	ms->send();
}
