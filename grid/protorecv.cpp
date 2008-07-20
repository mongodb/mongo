// protorecv.cpp

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

#include "stdafx.h"
#include "protocol.h"
#include "boost/thread.hpp"
#include "../util/goodies.h"
#include "../util/sock.h"
#include "protoimpl.h"
#include "../db/introspect.h"

boost::mutex biglock;
boost::mutex coutmutex;
boost::mutex threadStarterMutex;
boost::condition threadActivate; // creating a new thread, grabbing threadUseThisOne
ProtocolConnection *threadUseThisOne = 0;
void receiverThread();

map<SockAddr,ProtocolConnection*> firstPCForThisAddress;

/* todo: eventually support multiple listeners on diff ports.  not 
         bothering yet.
*/
ProtocolConnection *any = 0;

EndPointToPC pcMap;

class GeneralInspector : public SingleResultObjCursor { 
	Cursor* clone() { return new GeneralInspector(*this); }
	void fill() {
		b.append("version", "1.0.0.1");
		b.append("versionDesc", "none");
		b.append("nConnections", pcMap.size());
	}
public:
	GeneralInspector() { reg("intr.general"); }
} _geninspectorproto;

#include <signal.h>

/* our version of netstat */
void sighandler(int x) { 
	cout << "ProtocolConnections:" << endl;
	lock lk(biglock);
	EndPointToPC::iterator it = pcMap.begin();
	while( it != pcMap.end() ) { 
		cout << "  conn " << it->second->toString() << endl;
		it++;
	}
	cout << "any: ";
	if( any ) cout << any->toString();
	cout << "\ndone" << endl;
}

struct SetSignal { 
	SetSignal() { 
#if !defined(_WIN32)
		signal(SIGUSR2, sighandler);
#endif
	}
} setSignal;

string ProtocolConnection::toString() { 
	stringstream out;
	out << myEnd.toString() << " <> " <<
		farEnd.toString() << " rcvd.size:" << cr.received.size() <<
		" pndngMsgs.size:" << cr.pendingMessages.size() << 
		" pndngSnd.size:" << cs.pendingSend.size();
	return out.str();
} 

ProtocolConnection::~ProtocolConnection() { 
	cout << ".warning: ~ProtocolConnection() not implemented (leaks mem etc)" << endl;
	if( any == this )
		any = 0;
}

void ProtocolConnection::shutdown() { 
	ptrace( cout << ".shutdown()" << endl; )
	if( acceptAnyChannel() || first ) 
		return;
	ptrace( cout << ".   to:" << to.toString() << endl; )
	__sendRESET(this);
}

inline ProtocolConnection::ProtocolConnection(ProtocolConnection& par, EndPoint& _to) :
  udpConnection(par.udpConnection), myEnd(par.myEnd), cs(*this), cr(*this) 
{ 
	parent = &par;
	farEnd = _to;
	first = true;
	// todo: LOCK

	assert(pcMap.count(farEnd) == 0);
//	pcMap[myEnd] = this;
}

inline bool ProtocolConnection::acceptAnyChannel() const { 
	return myEnd.channel == MessagingPort::ANYCHANNEL; 
}

inline void ProtocolConnection::init() {
	first = true;
	lock lk(biglock);
	lock tslk(threadStarterMutex);

	if( acceptAnyChannel() ) {
		assert( any == 0 );
		any = this;
	}
	else {
		pcMap[farEnd] = this;
	}

	if( firstPCForThisAddress.count(myEnd.sa) == 0 ) { 
		firstPCForThisAddress[myEnd.sa] = this;
		// need a receiver thread.  one per port # we listen on. shared by channels.
		boost::thread receiver(receiverThread);
		threadUseThisOne = this;
		threadActivate.notify_one();
		return;
	}
}

ProtocolConnection::ProtocolConnection(UDPConnection& c, EndPoint& e, SockAddr *_farEnd) : 
  udpConnection(c), myEnd(e), cs(*this), cr(*this) 
{ 
	parent = this;
	if( _farEnd ) { 
		farEnd.channel = myEnd.channel;
		farEnd.sa = *_farEnd;
	}
	init();
}

/* find message for fragment */
MR* CR::getPendingMsg(F *fr) {
	MR *m;
	map<int,MR*>::iterator i = pendingMessages.find(fr->__msgid());
	if( i == pendingMessages.end() ) {
		if( pendingMessages.size() > 20 ) {		
			cout << ".warning: pendingMessages.size()>20, ignoring msg until we dequeue" << endl;
			return 0;
		}
		m = new MR(&pc, fr->__msgid(), pc.farEnd);
		pendingMessages[fr->__msgid()] = m;
	}
	else
		m = i->second;
	return m;
/*
	MR*& m = pendingMessages[fr->__msgid()];
	if( m == 0 )		
		m = new MR(&pc, fr->__msgid(), fromAddr);
	return m;
*/
}

void MR::removeFromReceivingList() {
	pc.cr.pendingMessages.erase(msgid);
}

MR::MR(ProtocolConnection *_pc, MSGID _msgid, EndPoint& _from) : 
        pc(*_pc), msgid(_msgid), from(_from), nReceived(0)
{
	messageLenExpected = nExpected = -1;
	f.push_back(0);
}

void MR::reportMissings(bool reportAll) { 
	vector<short> missing;
	unsigned t = curTimeMillis();
	for( unsigned i = 0; i < f.size(); i++ ) { 
		if( f[i] == 0 ) {
			int diff = tdiff(reportTimes[i],t);
			assert( diff >= 0 || reportTimes[i] == 0 );
			if( diff > 100 || reportTimes[i]==0 || 
				(reportAll && diff > 1) ) {
				reportTimes[i] = t;
				assert( i < 25000 );
				missing.push_back(i);
			}
		}
	}
	if( !missing.empty() )
		__sendMISSING(&pc, from, msgid, missing);
}

inline bool MR::complete() { 
	if( nReceived == nExpected ) {
		assert( nExpected == (int) f.size() );
		assert( f[0] != 0 );
		return true;
	}
	return false;
/*
	if( nExpected > (int) f.size() || nExpected == -1 )
		return false;
	for( unsigned i = 0; i < f.size(); i++ )
		if( f[i] == 0 )
			return false;
	return true;
*/
}

/* true=msg complete */
bool MR::got(F *frag, EndPoint& fromAddr) {
	MSGID msgid = frag->__msgid();
	int i = frag->__num();
	if( i == 544 && !frag->__isREQUESTACK() ) { 
		cout << "************ GOT LAST FRAGMENT #544" << endl;
	}
	if( nExpected < 0 && i == 0 ) {
		messageLenExpected = frag->__firstFragMsgLen();
		if( messageLenExpected == frag->__len()-FragHeader )
			nExpected = 1;
		else {
			int mss = frag->__len()-FragHeader;
			assert( messageLenExpected > mss );
			nExpected = (messageLenExpected + mss - 1) / mss;
			ptrace( cout << ".got first frag, expect:" << nExpected << "packets, expectedLen:" << messageLenExpected << endl; )
		}
	}
	if( i >= (int) f.size() )
		f.resize(i+1, 0);
	if( frag->__isREQUESTACK() ) { 
		ptrace( cout << "...got(): processing a REQUESTACK" << endl; )
		/* we're simply seeing if we got the data, and then acking. */
		delete frag;
		frag = 0;
		reportTimes.resize(f.size(), 0);
		if( complete() ) {
			__sendACK(&pc, msgid);
			return true;
		} 
		reportMissings(true);
		return false;
	}
	else if( f[i] != 0 ) { 
		ptrace( cout << "\t.dup packet i:" << i << ' ' << from.toString() << endl; )
		delete frag;
		return false;
	}
	if( frag ) { 
		f[i] = frag;
		nReceived++;
	}
	reportTimes.resize(f.size(), 0);

	if( !complete() )
		return false;
	__sendACK(&pc, msgid);
	return true;

/*
	if( f[0] == 0 || f[i] == 0 ) {
//		reportMissings(frag == 0);
		return false;
	}
	if( i+1 < nExpected ) {
//cout << "TEMP COMMENT" << endl;
//		if( i > 0 && f[i-1] == 0 )
//			reportMissings(frag == 0);
		return false;
	}
	// last fragment received
	if( !complete() ) {
//		reportMissings(frag == 0);
		return false;
	}
	__sendACK(&pc, fromAddr, msgid);
	return frag != 0;
*/
}

MR* CR::recv() { 
	MR *x;
	{
		lock lk(biglock);
		while( received.empty() )
			receivedSome.wait(lk);
		x = received.back();
		received.pop_back();
	}
	return x;
}

// this is how we tell protosend.cpp we got these
void gotACK(F*, ProtocolConnection *);
void gotMISSING(F*, ProtocolConnection *);

void receiverThread() {
	ProtocolConnection *startingConn; // this thread manages many; this is just initiator or the parent for acceptany
	UDPConnection *uc;
	{
		lock lk(threadStarterMutex);
		while( 1 ) {
			if( threadUseThisOne != 0 ) {
				uc = &threadUseThisOne->udpConnection;
				startingConn = threadUseThisOne;
				threadUseThisOne = 0;
				break;
			}
			threadActivate.wait(lk);
		}
	}

	cout << "\n.Activating a new receiverThread\n   " << startingConn->toString() << '\n' << endl;

	EndPoint fromAddr;
	while( 1 ) {
		F *f = __recv(*uc, fromAddr.sa);
		lock l(biglock);
		fromAddr.channel = f->__channel();
		ptrace( cout << "..__recv() from:" << fromAddr.toString() << " frag:" << f->__num() << endl; )
		assert( fromAddr.channel >= 0 );
		EndPointToPC::iterator it = pcMap.find(fromAddr);
		ProtocolConnection *mypc;
		if( it == pcMap.end() ) {
			if( !startingConn->acceptAnyChannel() ) {
				cout << ".WARNING: got packet from an unknown endpoint:" << fromAddr.toString() << endl;
			    cout << ".  this may be ok if you just restarted" << endl;
				delete f;
				continue;
			}
			cout << ".New connection accepted from " << fromAddr.toString() << endl;
			mypc = new ProtocolConnection(*startingConn, fromAddr);
			pcMap[fromAddr] = mypc;
		}
		else
			mypc = it->second;

		assert( fromAddr.channel == mypc->farEnd.channel );
		MsgTracker& track = mypc->cr.oldMsgTracker;

		if( f->op != F::NORMAL ) {
			if( f->__isACK() ) { 
				gotACK(f, mypc);
				delete f;
				continue;
			}
			if( f->__isMISSING() ) { 
				gotMISSING(f, mypc);
				delete f;
				continue;
			}
			if( f->op == F::RESET ) { 
				ptrace( cout << ".got RESET" << endl; )
				track.reset();
				mypc->cs.resetIt();
				delete f;
				continue;
			}
		}

		if( track.recentlyReceived.count(f->__msgid()) ) { 
			// already done with it.  ignore, other than acking.
			if( f->__isREQUESTACK() )
				__sendACK(mypc, f->__msgid());
			else { 
				ptrace( cout << ".ignoring packet about msg already received msg:" << f->__msgid() << " op:" << f->op << endl; )
			}
			delete f;
			continue;
		}

		if( f->__msgid() <= track.lastFullyReceived && !track.recentlyReceivedList.empty() ) {
			// reconnect on an old channel?
			ptrace( cout << ".warning: strange msgid:" << f->__msgid() << " received, last:" << track->lastFullyReceived << " conn:" << fromAddr.toString() << endl; )
		}

		MR *m = mypc->cr.getPendingMsg(f); /* todo: optimize for single fragment case? */
		if( m == 0 ) { 
			ptrace( cout << "..getPendingMsg() returns 0" << endl; )
			delete f;
			continue;
		}
		if( m->got(f, fromAddr) ) {
			track.got(m->msgid);
			m->removeFromReceivingList();
			{
				mypc->parent->cr.received.push_back(m);
				mypc->parent->cr.receivedSome.notify_one();
			}
		}
	}
}
