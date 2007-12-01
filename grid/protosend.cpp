// protosend.cpp

/* todo: redo locking! */

#include "stdafx.h"
#include "protocol.h"
#include "boost/thread.hpp"
#include "../util/goodies.h"
#include "../util/sock.h"
#include "protoimpl.h"
using namespace boost;
typedef boost::mutex::scoped_lock lock;

/* todo: granularity? */
extern boost::mutex biglock;

void senderComplainThread();

boost::thread sender(senderComplainThread);

bool erasePending(ProtocolConnection *pc, int id);

inline bool MS::complain(unsigned now) {
	if( tdiff(lastComplainTime, now) < (int) complainInterval )
		return false;
	if( complainInterval > 4000 ) { 
		etrace( cout << ".no ack of send after " << complainInterval/1000 << " seconds " << to.toString() << endl; )
		if( complainInterval > 35000 ) { 
			etrace( cout << ".GIVING UP on sending message" << endl; )
			erasePending(pc, msgid);
			return true;
		}
	}
	complainInterval *= 2;
	lastComplainTime = now;
	__sendREQUESTACK(pc, to, msgid, fragments.size()-1);
	return false; 
}

// where's my ack?
void senderComplainThread() { 
	sleepmillis(200);

	while( 1 ) { 
		sleepmillis(50);
		{
			lock lk(biglock);
			unsigned now = curTimeMillis();
			/* todo: more efficient data structure here. */
			for( EndPointToPC::iterator i = pcMap.begin(); i != pcMap.end(); i++ ) {
				ProtocolConnection *pc = i->second;
				for( vector<MS*>::iterator j = pc->cs.pendingSend.begin(); j < pc->cs.pendingSend.end(); j++ )
					if( (*j)->complain(now) ) 
						break;
			}
		}
	}
}

inline MS* _slocked_getMSForFrag(F* fr, ProtocolConnection *pc) { 
	int id = fr->__msgid();
	vector<MS*>::iterator it;
	for( it = pc->cs.pendingSend.begin(); it<pc->cs.pendingSend.end(); it++ ) {
		MS *m = *it;
		if( m->msgid == id )
			return m;
	}
	return 0;
}

// received a MISSING message from the other end.  retransmit.
void gotMISSING(F* fr, ProtocolConnection *pc) {
	pc->cs.delayGotMissing();

	ptrace( cout << ".gotMISSING() msgid:" << fr->__msgid() << endl; )
	MS *m = _slocked_getMSForFrag(fr, pc);
	if( m ) {
		{ 
			int df = tdiff(m->lastComplainTime, curTimeMillis()) * 2;
			if( df < 10 )
				df = 10;
			if( df > 2000 )
				df = 2000;
			m->complainInterval = (unsigned) df;
			cout << "TEMP: set complainInterval to " << m->complainInterval << endl;
		}

		int n;
		short* s = fr->__getMissing(n);
		assert( n > 0 && n < 5000 );
		for( int i = 0; i < n; i++ ) {
			//				ptrace( cout << "..  resending frag #" << s[i] << ' ' << m->to.toString() << endl; )
			__sendFrag(pc, m->to, m->fragments[s[i]], true);
			if( i % 64 == 0 && pc->cs.delay >= 1.0 ) {
				ptrace( cout << "SLEEP" << endl; )
				sleepmillis((int) pc->cs.delay);
			}
		}
		return;
	}
	ptrace( cout << "\t.warning: gotMISSING for an unknown msg id:" << fr->__msgid() << ' ' << pc->farEnd.toString() << endl; )
}

/* done sending a msg, clean up and notify sender to unblock */
bool erasePending(ProtocolConnection *pc, int id) {
	vector<MS*>::iterator it;
	CS& cs = pc->cs;
	for( it = cs.pendingSend.begin(); it < cs.pendingSend.end(); it++ ) {
		MS *m = *it;
		if( m->msgid == id ) { 
			cs.pendingSend.erase(it);
			ptrace( cout << "..gotACK/erase: found pendingSend msg:" << id << endl; )
			delete m;
			cs.msgSent.notify_one();
			return true;
		}
	}
	return false;
}

// received an ACK message. so we can discard our saved copy we were trying to send, we
// are done with it.
void gotACK(F* fr, ProtocolConnection *pc) {
	if( erasePending(pc, fr->__msgid()) )
		return;
	ptrace( cout << ".warning: got ack for an unknown msg id:" << fr->__msgid()	<< ' ' << pc->farEnd.toString() << endl; )
}

void MS::send() {
	/* flow control */
//	cout << "send() to:" << to.toString() << endl;
	ptrace( cout << "..MS::send() pending=" << pc.cs.pendingSend.size() << endl; )

	lock lk(biglock);

	if( pc->acceptAnyChannel() ) { 
		EndPointToPC::iterator it = pcMap.find(to);
		if( it == pcMap.end() ) {
			cout << ".ERROR: can't find ProtocolConnection object for " << to.toString() << endl;
			assert(false);
			return;
		}
		/* switch to the child protocolconnection */
		pc = it->second;
	}
	else {
		assert(pc->myEnd.channel == to.channel);
	}

	if( pc->first ) { 
		pc->first = false;
		if( pc->myEnd.channel >= 0 )
			__sendRESET(pc);
		assert( pc->farEnd.channel == to.channel);
		if( pc->farEnd.sa.isLocalHost() )
			pc->cs.delayMax = 1.0;
	}

	// not locked here, probably ok to call size()
	if( pc->cs.pendingSend.size() >= 1 ) {
		cout << ".waiting for queued sends to complete " << pc->cs.pendingSend.size() << endl;
		while( pc->cs.pendingSend.size() >= 1 )
			pc->cs.msgSent.wait(lk);
		cout << ".waitend" << endl;
	}

	lastComplainTime = curTimeMillis();
	pc->cs.pendingSend.push_back(this);
	/* todo: pace */
	for( unsigned i = 0; i < fragments.size(); i++ ) {
		__sendFrag(pc, to, fragments[i]);
		if( i % 64 == 0 && pc->cs.delay >= 1.0 ) {
			ptrace( cout << ".sleep " << pc->cs.delay << endl; )
			sleepmillis((int) pc->cs.delay);
		}
	}

	pc->cs.delaySentMsg();
}

CS::CS(ProtocolConnection& _pc) : 
    pc(_pc), delay(0), delayMax(10)
{ 
}

void CS::resetIt() { 
	for( vector<MS*>::iterator i = pendingSend.begin(); i < pendingSend.end(); i++ )
		delete (*i);
	pendingSend.clear();
}
