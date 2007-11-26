// protocol.h

#pragma once

#include "boost/thread/mutex.hpp"
#include "boost/thread/condition.hpp"
#include "../util/sock.h"
#include "../util/goodies.h"

typedef WrappingInt MSGID;

struct Fragment;

#if 1
#define ptrace(x) 
#define etrace(x) 
#else
#define ptrace(x) { cout << curTimeMillis() % 10000; x }
#define etrace(x) { cout << curTimeMillis() % 10000; x }
#endif

class F; // fragment
class MR; // message.  R=receiver side.
class CR; // connection receiver side
class MS; // message   S=sender side.
class CS; // connection sender side
class ProtocolConnection; // connection overall

/* ip:port:channel 
   We have one receiver thread per process (per ip address destination), and then 
   multiplex messages out to multiple connections(generally one per thread) which 
   each have a 'channel'.
*/
class EndPoint { 
public:
	int channel;
	SockAddr sa;
	bool operator<(const EndPoint& r) const {
		if( channel != r.channel )
			return channel < r.channel;
		return sa < r.sa;
	}
	string toString() { 
		stringstream out;
		out << sa.toString() << ':' << channel;
		return out.str();
	}
};

/* the double underscore stuff here is the actual implementation glue.
   wanted to keep that stuff clean and separate.  so put your implementation
   of these in protoimpl.h.
*/

void __sendRESET(ProtocolConnection *pc, EndPoint& to);

// sender -> 
void __sendFrag(ProtocolConnection *pc, EndPoint& to, F *, bool retran=false); // transmit a fragment
void __sendREQUESTACK(ProtocolConnection *pc, EndPoint& to, MSGID msgid, int fragNo); // transmit the REQUEST ACK msg

// receiver -> 
void __sendACK(ProtocolConnection *pc, EndPoint& to, MSGID msgid); // transmit ACK
void __sendMISSING(ProtocolConnection *pc, EndPoint& to, MSGID msgid, vector<short>& ids); // transmit MISSING

// -> receiver
F* __recv(UDPConnection& c, SockAddr& from); /* recv from socket a fragment and pass back */

class F { 
public:
	F(Fragment *f);
	~F();
	int __num();   //frag #
	MSGID __msgid();
	int __channel();
	bool __isREQUESTACK(); // if true, this is just a request for acknowledgement not real data
	int __firstFragMsgLen(); // only works on first fragment

	// sender side:
	bool __isACK(); // if true, this is an ack of a message
	bool __isMISSING(); // if true, list of frags to retransmit
	short* __getMissing(int& n); // get the missing fragno list

	Fragment *internals;
	enum { NORMAL, ACK, MISSING, REQUESTACK, RESET } op;
};

class MR { 
public:
	MR(ProtocolConnection *_pc, MSGID _msgid, EndPoint& _from);
	~MR() { freeFrags(); }
	void freeFrags() { 
		for( unsigned i = 0; i < f.size(); i++ )
			delete f[i];
	}
	bool got(F *f, EndPoint& from); // received a fragment
	bool gotFirst() { return f[0] != 0; }
	ProtocolConnection& pc;
	void removeFromReceivingList();
	bool complete();
	const MSGID msgid;
	int n() { return f.size(); }
public:
	int messageLenExpected;
	int nExpected, nReceived;
	void reportMissings(bool reportAll);
	vector<F*> f;
	vector<unsigned> reportTimes;
	EndPoint from;
};

class MsgTracker {
public:
	std::list<MSGID> recentlyReceivedList;
	std::set<MSGID> recentlyReceived;
	MSGID lastFullyReceived;

	void reset() { 
		recentlyReceivedList.clear();
		recentlyReceived.clear();
		lastFullyReceived = 0;
	}

	void got(MSGID m) {
		unsigned sz = recentlyReceived.size();
		if( sz > 256 ) {
			recentlyReceived.erase(recentlyReceivedList.front());
			recentlyReceivedList.pop_front();
		}
		recentlyReceivedList.push_back(m);
		recentlyReceived.insert(m);
		if( m > lastFullyReceived || sz == 0 )
			lastFullyReceived = m;
	}
};

class CR { 
	friend class MR;
public:
	~CR() { ptrace( cout << ".warning: ~CR() not implemented" << endl; ) }
	CR(ProtocolConnection& _pc) : pc(_pc) { }
	MR* recv();
public:
	MR* getPendingMsg(F *fr, EndPoint& fromAddr);
	bool oldMessageId(int channel, MSGID m);
	void queueReceived(MR*);

	ProtocolConnection& pc;
	boost::condition receivedSome;
	vector<MR*> received;
	map<int,MR*> pendingMessages; /* partly received msgs */
	map<int,MsgTracker*> trackers; /* channel -> tracker */
};

/* -- sender side ------------------------------------------------*/

class CS { 
public:
	CS(ProtocolConnection& _pc);

	ProtocolConnection& pc;
	vector<MS*> pendingSend;
	void resetIt();

	double delayMax;
	double delay;
	void delayGotMissing() { 
		double delayOld = delay;
		if( delay == 0 )
			delay = 2.0;
		else
			delay = delay * 1.25;
		if( delay > delayMax ) delay = delayMax;
		if( delay != delayOld )
			cout << ".DELAY INCREASED TO " << delay << endl;
	}
	void delaySentMsg() { 
		if( delay != 0.0 ) {
			delay = delay * 0.5;
			cout << ".DELAY DECREASED TO " << delay << endl;
		}
	}
};

typedef map<EndPoint,ProtocolConnection*> EndPointToPC;
extern EndPointToPC pcMap;

/* -- overall Connection object ----------------------------------*/

#pragma warning( disable: 4355 )

class ProtocolConnection { 
public:
	ProtocolConnection(UDPConnection& c, EndPoint& e) : udpConnection(c), myEnd(e), cs(*this), cr(*this) { 
		init();
	}
	~ProtocolConnection() { 
		cout << ".warning: ~ProtocolConnection() not implemented (leaks mem)" << endl;
	}
	void shutdown();
	bool acceptAnyChannel() const;
	UDPConnection& udpConnection;
	/* note the channel for myEnd might be "any" --
  	   so you can't use channel for sending.  Use MS/MR.to 
	   for that.
	   */
	EndPoint myEnd;
	CR cr;
	CS cs;
	bool first;
	EndPoint to;
private:
	void init();
};

/* -- sender side ------------------------------------------------*/

class MS { 
public:
	MS(ProtocolConnection *_pc, EndPoint &_to, MSGID _msgid) : 
	  pc(*_pc), to(_to), msgid(_msgid), complainInterval(50) { }
	~MS() { 
		for( unsigned i = 0; i < fragments.size(); i++ )
			delete fragments[i];
	}

	/* init fragments, then call this */
	void send();

	vector<F*> fragments;

	/* request retrainsmissions. */
	bool complain(unsigned now);

	ProtocolConnection& pc;
	EndPoint to;
	const MSGID msgid;
	unsigned lastComplainTime;
	unsigned complainInterval;
};
