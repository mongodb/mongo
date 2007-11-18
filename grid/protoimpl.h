// protoimpl.h

#pragma once

#include "message.h"

const int FragMax = 1480;
const int FragHeader = 10;
const int MSS = FragMax - FragHeader;

#pragma pack(push)
#pragma pack(1)

struct Fragment {
	enum { MinFragmentLen = FragHeader + 1 };
	MSGID msgId;
	short channel;
	short fragmentLen;
	short fragmentNo;
	char data[16];
	int fragmentDataLen() { return fragmentLen - FragHeader; }
	char* fragmentData() { return data; }

	bool ok(int nRead) { 
		if( nRead < MinFragmentLen || fragmentLen > nRead || fragmentLen < MinFragmentLen ) {
			cout << "recv: fragment bad. fragmentLen:" << fragmentLen << " nRead:" << nRead << endl;
			return false;
		}
		if( fragmentNo == 0 && fragmentLen < MinFragmentLen + MsgDataHeaderSize ) { 
			cout << "recv: bad first fragment. fragmentLen:" << fragmentLen << endl;
			return false;
		}
		return true;
	} 

	MsgData* startOfMsgData() { assert(fragmentNo == 0); return (MsgData *) data; }
};
#pragma pack(pop)

// sender ->
inline void __sendFrag(ProtocolConnection *pc, EndPoint& to, F *f) {
	assert( f->internals->channel == to.channel );
	pc->udpConnection.sendto((char *) f->internals, f->internals->fragmentLen, to.sa);
}

inline void __sendREQUESTACK(ProtocolConnection *pc, EndPoint& to, 
							 MSGID msgid, int fragNo) { 
	Fragment f;
	f.msgId = msgid;
	f.channel = to.channel; assert( f.channel >= 0 );
	f.fragmentNo = ((short) -fragNo) -1;
	f.fragmentLen = FragHeader;
	cout << ".requesting ack, fragno=" << f.fragmentNo << " msg:" << f.msgId << ' ' << to.toString() << endl;
	pc->udpConnection.sendto((char *)&f, f.fragmentLen, to.sa);
}

// receiver -> 
inline void __sendACK(ProtocolConnection *pc, EndPoint& to, MSGID msgid) {
	cout << "...__sendACK() to:" << to.toString() << " msg:" << msgid << endl;
	Fragment f;
	f.msgId = msgid;
	f.channel = to.channel; assert( f.channel >= 0 );
	f.fragmentNo = -32768;
	f.fragmentLen = FragHeader;
	pc->udpConnection.sendto((char *)&f, f.fragmentLen, to.sa);
}

/* this is to clear old state for the channel in terms of what msgids are 
   already sent. 
*/
inline void __sendRESET(ProtocolConnection *pc, EndPoint& to) {
	Fragment f;
	f.msgId = -1;
	f.channel = to.channel; assert( f.channel >= 0 );
	f.fragmentNo = -32766;
	f.fragmentLen = FragHeader;
	cout << "...__sendRESET() to:" << to.toString() << endl;
	pc->udpConnection.sendto((char *)&f, f.fragmentLen, to.sa);
}

inline void __sendMISSING(ProtocolConnection *pc, EndPoint& to, 
						  MSGID msgid, vector<short>& ids) {
	int n = ids.size(); cout << "..sendMISSING n:" << n << ' ' << to.toString() << endl;
	if( n > 256 ) {
		cout << "..info: sendMISSING: n:" << n << ' ' << to.toString() << endl;
		n = 256;
	}
	Fragment *f = (Fragment*) malloc(FragHeader + n*2);
	f->msgId = msgid;
	f->channel = to.channel; assert( f->channel >= 0 );
	f->fragmentNo = -32767;
	f->fragmentLen = FragHeader + n*2;
	short *s = (short *) f->data;
	for( int i = 0; i < n; i++ )
		*s++ = ids[i];
	cout << "...sendMISSING fraglen:" << f->fragmentLen << endl;
	pc->udpConnection.sendto((char *)f, f->fragmentLen, to.sa);
// TEMPcomment	free(f);
}

// -> receiver
inline F* __recv(UDPConnection& c, SockAddr& from) {
	Fragment *f = (Fragment *) malloc(c.mtu());
	int n;
	while( 1 ) {
		n = c.recvfrom((char*) f, c.mtu(), from);
		if( n >= 0 )
			break;
		if( !goingAway ) 
			sleepsecs(60);
		cout << ".recvfrom returned error " << getLastError() << " socket:" << c.sock << endl;
	}
	assert( f->fragmentLen == n );
	return new F(f);
}

inline F::F(Fragment *f) : internals(f), op(NORMAL) { 
	if( internals->fragmentNo < 0 ) { 
		if( internals->fragmentNo == -32768 ) {
			op = ACK;
			cout << ".got ACK msg:" << internals->msgId << endl;
		} else if( internals->fragmentNo == -32767 ) {
			op = MISSING;
			cout << ".got MISSING" << endl;
		} else if( internals->fragmentNo == -32766 ) {
			op = RESET;
		} else {
			op = REQUESTACK;
			internals->fragmentNo = -(internals->fragmentNo+1);
			cout << ".got REQUESTACK frag:" << internals->fragmentNo << " msg:" << internals->msgId << endl;
		}
	}
}
inline F::~F() { free(internals); internals=0; }
inline int F::__num() { return internals->fragmentNo; }
inline MSGID F::__msgid() { return internals->msgId; }
inline int F::__channel() { return internals->channel; }
inline bool F::__isREQUESTACK() { return op == REQUESTACK; }
inline bool F::__isACK() { return op == ACK; }
inline bool F::__isMISSING() { return op == MISSING; }
inline short* F::__getMissing(int& n) { 
	n = internals->fragmentDataLen() / 2;
	return (short *) internals->fragmentData();
}
