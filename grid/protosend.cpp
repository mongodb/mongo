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
boost::mutex mutexs;
boost::condition msgSent;

bool erasePending(ProtocolConnection *pc, int id);

inline bool MS::complain(unsigned now) {
	if( tdiff(lastComplainTime, now) < (int) complainInterval )
		return false;
	if( complainInterval > 4000 ) { 
		etrace( cout << ".no ack of send after " << complainInterval/1000 << " seconds " << to.toString() << endl; )
		if( complainInterval > 35000 ) { 
			etrace( cout << ".GIVING UP on sending message" << endl; )
			erasePending(&pc, msgid);
			return true;
		}
	}
	complainInterval *= 2;
	lastComplainTime = now;
	__sendREQUESTACK(&pc, to, msgid, fragments.size()-1);
	return false; 
}

// where's my ack?
void senderComplainThread() { 
	while( 1 ) { 
		sleepmillis(50);
		{
			lock lk(mutexs);
			unsigned now = curTimeMillis();
			for( EndPointToPC::iterator i = pcMap.begin(); i != pcMap.end(); i++ ) {
				ProtocolConnection *pc = i->second;
				for( vector<MS*>::iterator j = pc->cs.pendingSend.begin(); j < pc->cs.pendingSend.end(); j++ )
					if( (*j)->complain(now) ) 
						break;
			}
		}
	}
}

boost::thread sender(senderComplainThread);

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
void gotMISSING(F* fr, ProtocolConnection *pc, EndPoint& from) {
	lock lk(mutexs);

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
	ptrace( cout << "\t.warning: gotMISSING for an unknown msg id:" << fr->__msgid() << ' ' << from.toString() << endl; )
}

bool erasePending(ProtocolConnection *pc, int id) {
	vector<MS*>::iterator it;
	for( it = pc->cs.pendingSend.begin(); it < pc->cs.pendingSend.end(); it++ ) {
		MS *m = *it;
		if( m->msgid == id ) { 
			pc->cs.pendingSend.erase(it);
			ptrace( cout << "..gotACK/erase: found pendingSend msg:" << id << endl; )
			delete m;
			msgSent.notify_one();
			return true;
		}
	}
	return false;
}

// received an ACK message. so we can discard our saved copy we were trying to send, we
// are done with it.
void gotACK(F* fr, ProtocolConnection *pc, EndPoint& from) {
	lock lk(mutexs);
	if( erasePending(pc, fr->__msgid()) )
		return;
	ptrace( cout << ".warning: got ack for an unknown msg id:" << fr->__msgid()	<< ' ' << from.toString() << endl; )
}

void MS::send() {
	/* flow control */
	cout << "send() to:" << to.toString() << endl;
	ptrace( cout << "..MS::send() pending=" << pc.cs.pendingSend.size() << endl; )
	lock lk(mutexs);

	if( pc.first ) { 
		pc.first = false;
		if( pc.myEnd.channel >= 0 )
			__sendRESET(&pc, to);
		pc.to = to;
#if defined(_WIN32)
		pc.cs.delayMax = 1.0;
#else
//		cout << "SADDR:" << pc.to.sa.sa.sin_addr.s_addr << endl;
		if( pc.to.sa.sa.sin_addr.s_addr == 0x100007f ) {
//			cout << "TEMP: LOCALHOST ************************************* " << endl;
			pc.cs.delayMax = 1.0;
		}
#endif
	}

	while( pc.cs.pendingSend.size() >= 1 ) {
		cout << ".waiting for queued sends to complete " << pc.cs.pendingSend.size() << endl;
		while( pc.cs.pendingSend.size() >= 1 )
			msgSent.wait(lk);
		cout << ".waitend" << endl;
	}
	lastComplainTime = curTimeMillis();
	pc.cs.pendingSend.push_back(this);
	/* todo: pace */
	for( unsigned i = 0; i < fragments.size(); i++ ) {
		__sendFrag(&pc, to, fragments[i]);
		if( i % 64 == 0 && pc.cs.delay >= 1.0 ) {
			ptrace( cout << "SLEEP" << endl; )
			sleepmillis((int) pc.cs.delay);
		}
	}

	pc.cs.delaySentMsg();
}

CS::CS(ProtocolConnection& _pc) : 
    pc(_pc), delay(0), delayMax(10)
{ 
}

void CS::resetIt() { 
	lock lk(mutexs);
	for( vector<MS*>::iterator i = pendingSend.begin(); i < pendingSend.end(); i++ )
		delete (*i);
	pendingSend.clear();
}
