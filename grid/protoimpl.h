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
			ptrace( cout << ".recv: fragment bad. fragmentLen:" << fragmentLen << " nRead:" << nRead << endl; )
			return false;
		}
		if( fragmentNo == 0 && fragmentLen < MinFragmentLen + MsgDataHeaderSize ) { 
			ptrace( cout << ".recv: bad first fragment. fragmentLen:" << fragmentLen << endl; )
			return false;
		}
		return true;
	} 

	MsgData* startOfMsgData() { assert(fragmentNo == 0); return (MsgData *) data; }
};
#pragma pack(pop)

const bool dumpIP = false;

inline void DUMP(Fragment& f, SockAddr& t, const char *tabs) { 
	cout << tabs << curTimeMillis() % 10000 << ' ';
	short s = f.fragmentNo;
	if( s == -32768 ) 
		cout << "ACK M:" << f.msgId % 1000;
	else if( s == -32767 )
		cout << "MISSING";
	else if( s == -32766 )
		cout << "RESET ch:" << f.channel;
	else if( s < 0 )
		cout << "REQUESTACK";
	else
		cout << '#' << s << ' ' << f.fragmentLen << " M:" << f.msgId % 1000;
	cout << ' ';
	if( dumpIP )
		cout << t.toString();
}

inline void DUMPDATA(Fragment& f, const char *tabs) { 
	if( f.fragmentNo >= 0 ) { 
		cout << '\n' << tabs;
		int x = f.fragmentDataLen();
		if( x > 28 ) x = 28;
		for( int i = 0; i < x; i++ ) {
			if( f.data[i] == 0 ) cout << (char) 254;
			else cout << (f.data[i] >= 32 ? f.data[i] : '.');
		}
	}
	cout << endl;
}

inline void SEND(UDPConnection& c, Fragment &f, SockAddr& to, const char *extra="") { 
	DUMP(f, to, "\t\t\t\t\t>");
	c.sendto((char *) &f, f.fragmentLen, to);
	cout << extra;
	DUMPDATA(f, "\t\t\t\t\t      ");
}

// sender ->
inline void __sendFrag(ProtocolConnection *pc, EndPoint& to, F *f, bool retran) {
	assert( f->internals->channel == to.channel );
	SEND(pc->udpConnection, *f->internals, to.sa, retran ? " retran" : "");
}

inline void __sendREQUESTACK(ProtocolConnection *pc, EndPoint& to, 
							 MSGID msgid, int fragNo) { 
	Fragment f;
	f.msgId = msgid;
	f.channel = to.channel; assert( f.channel >= 0 );
	f.fragmentNo = ((short) -fragNo) -1;
	f.fragmentLen = FragHeader;
	ptrace( cout << ".requesting ack, fragno=" << f.fragmentNo << " msg:" << f.msgId << ' ' << to.toString() << endl; )
	SEND(pc->udpConnection, f, to.sa);
}

// receiver -> 
inline void __sendACK(ProtocolConnection *pc, EndPoint& to, MSGID msgid) {
	ptrace( cout << "...__sendACK() to:" << to.toString() << " msg:" << msgid << endl; )
	Fragment f;
	f.msgId = msgid;
	f.channel = to.channel; assert( f.channel >= 0 );
	f.fragmentNo = -32768;
	f.fragmentLen = FragHeader;
	SEND(pc->udpConnection, f, to.sa);
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
	ptrace( cout << "...__sendRESET() to:" << to.toString() << endl; )
	SEND(pc->udpConnection, f, to.sa);
}

inline void __sendMISSING(ProtocolConnection *pc, EndPoint& to, 
						  MSGID msgid, vector<short>& ids) {
	int n = ids.size(); 
	ptrace( cout << "..sendMISSING n:" << n << " firstmissing:" << ids[0] << ' ' << to.toString() << endl; )
	if( n > 256 ) {
		ptrace( cout << "..info: sendMISSING limiting to 256 ids" << endl; )
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
	ptrace( cout << "...sendMISSING fraglen:" << f->fragmentLen << endl; )
	SEND(pc->udpConnection, *f, to.sa);
	free(f);
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
	DUMP(*f, from, "\t\t\t\t\t\t\t\t\t\t<");
	DUMPDATA(*f,   "\t\t\t\t\t\t\t\t\t\t      ");
	return new F(f);
}

inline F::F(Fragment *f) : internals(f), op(NORMAL) { 
	if( internals->fragmentNo < 0 ) { 
		if( internals->fragmentNo == -32768 ) {
			op = ACK;
			ptrace( cout << ".got ACK msg:" << internals->msgId << endl; )
		} else if( internals->fragmentNo == -32767 ) {
			op = MISSING;
			ptrace( cout << ".got MISSING" << endl; )
		} else if( internals->fragmentNo == -32766 ) {
			op = RESET;
		} else {
			op = REQUESTACK;
			internals->fragmentNo = -(internals->fragmentNo+1);
			ptrace( cout << ".got REQUESTACK frag:" << internals->fragmentNo << " msg:" << internals->msgId << endl; )
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
inline int F::__firstFragMsgLen() { 
	return internals->startOfMsgData()->len;
}
