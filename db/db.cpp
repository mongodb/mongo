// db.cpp : Defines the entry point for the console application.
//

#include "stdafx.h"
#include "db.h"
#include "../grid/message.h"
#include "../util/mmap.h"
#include "../util/hashtab.h"
#include "../util/goodies.h"
#include "pdfile.h"
#include "jsobj.h"
#include "query.h"

extern const char *dbpath;
extern int curOp;

boost::mutex dbMutex;

struct MyStartupTests {
	MyStartupTests() {
		assert( sizeof(OID) == 12 );
	}
} mystartupdbcpp;

/* example for
	var zz = { x: 3, y: "foo", v: { z:5, arr: [1, 2] } }
	zz.v.arr.prop = "zoo";
*/

void quicktest() { 
	cout << "quicktest()\n";

	MemoryMappedFile mmf;
	char *m = (char *) mmf.map("/tmp/quicktest", 16384);
	//	cout << "mmf reads: " << m << endl;
	strcpy_s(m, 1000, "hello worldz");
}

void pdfileInit();

class DbMessage {
public:
	DbMessage(Message& _m) : m(_m) {
		theEnd = _m.data->_data + _m.data->dataLen();
		int *r = (int *) _m.data->_data;
		reserved = *r;
		r++;
		data = (const char *) r;
		nextjsobj = data;
	}

	const char * getns() { return data; }
	void getns(Namespace& ns) {
		ns = data;
	}

	int pullInt() {
		if( nextjsobj == data )
			nextjsobj += strlen(data) + 1; // skip namespace
		int i = *((int *)nextjsobj);
		nextjsobj += 4;
		return i;
	}
	long long pullInt64() {
		if( nextjsobj == data )
			nextjsobj += strlen(data) + 1; // skip namespace
		long long i = *((long long *)nextjsobj);
		nextjsobj += 8;
		return i;
	}

	OID* getOID() {
		return (OID *) (data + strlen(data) + 1); // skip namespace
	}

	void getQueryStuff(const char *&query, int& ntoreturn) {
		int *i = (int *) (data + strlen(data) + 1);
		ntoreturn = *i;
		i++;
		query = (const char *) i;
	}

	/* for insert and update msgs */
	bool moreJSObjs() { return nextjsobj != 0; }
	JSObj nextJsObj() {
		if( nextjsobj == data )
			nextjsobj += strlen(data) + 1; // skip namespace
		JSObj js(nextjsobj);
                assert( js.objsize() < ( theEnd - data ) );
		if( js.objsize() <= 0 )
			nextjsobj = null;
		else {
			nextjsobj += js.objsize();
			if( nextjsobj >= theEnd )
				nextjsobj = 0;
		}
		return js;
	}

private:
	Message& m;
	int reserved;
	const char *data;
	const char *nextjsobj;
	const char *theEnd;
};

/*Record* findByOID(const char *ns, OID *oid) {
	// temp implementation
	Cursor c = theDataFileMgr.findAll(ns);
	while( c.ok() ) {
		JSObj js = c.current();
		OID *i = js.getOID();
		if( i && *oid == *i )
			return r;
		c.advance();
	}
	return 0;
}*/

#pragma pack(push)
#pragma pack(1)
struct EmptyObject {
	EmptyObject() { len = 5; jstype = EOO; }
	int len;
	char jstype;
} emptyObject;
#pragma pack(pop)

void killCursors(int n, long long *ids);
void receivedKillCursors(Message& m) {
	int *x = (int *) m.data->_data;
	x++; // reserved
	int n = *x++;
	assert( n >= 1 && n <= 2000 );
	killCursors(n, (long long *) x);
}

void receivedUpdate(Message& m) {
	DbMessage d(m);
	const char *ns = d.getns();
	setClient(ns);
	int flags = d.pullInt();
	JSObj query = d.nextJsObj();
	assert( d.moreJSObjs() );
        assert( query.objsize() < m.data->dataLen() );
	JSObj toupdate = d.nextJsObj();
        assert( toupdate.objsize() < m.data->dataLen() );
        
        assert( query.objsize() + toupdate.objsize() < m.data->dataLen() );
	updateObjects(ns, toupdate, query, flags & 1);
	client = 0;
}

void receivedDelete(Message& m) {
	DbMessage d(m);
	const char *ns = d.getns();
	setClient(ns);
	int flags = d.pullInt();
	assert( d.moreJSObjs() );
	JSObj pattern = d.nextJsObj();
	deleteObjects(ns, pattern, flags & 1);
	client = 0;
}

void receivedQuery(MessagingPort& dbMsgPort, Message& m, stringstream& ss) {
	DbMessage d(m);
	const char *ns = d.getns();
	setClient(ns);
	int ntoskip = d.pullInt();
	int ntoreturn = d.pullInt();
	JSObj query = d.nextJsObj();
	auto_ptr< set<string> > fields;
	if( d.moreJSObjs() ) { 
		fields = auto_ptr< set<string> >(new set<string>());
		d.nextJsObj().getFieldNames(*fields);
	}
	QueryResult* msgdata;

	try { 
		msgdata = runQuery(ns, ntoskip, ntoreturn, query, fields, ss);
	}
	catch( AssertionException ) { 
		cout << " Caught Assertion in runQuery, continuing" << endl; 
		cout << "  ntoskip:" << ntoskip << " ntoreturn:" << ntoreturn << endl;
		cout << "  ns:" << ns << endl;
		cout << "  query:" << query.toString() << endl;
		msgdata = (QueryResult*) malloc(sizeof(QueryResult));
		QueryResult *qr = msgdata;
		qr->_data[0] = 0;
		qr->_data[1] = 0;
		qr->_data[2] = 0;
		qr->_data[3] = 0;
		qr->len = sizeof(QueryResult);
		qr->operation = opReply;
		qr->cursorId = 0;
		qr->startingFrom = 0;
		qr->nReturned = 0;
	}
	Message resp;
	resp.setData(msgdata, true); // transport will free
	dbMsgPort.reply(m, resp);
	client = 0;
}

void receivedGetMore(MessagingPort& dbMsgPort, Message& m) {
	DbMessage d(m);
	const char *ns = d.getns();
	setClient(ns);
	int ntoreturn = d.pullInt();
	long long cursorid = d.pullInt64();
	QueryResult* msgdata = getMore(ns, ntoreturn, cursorid);
	Message resp;
	resp.setData(msgdata, true);
	dbMsgPort.reply(m, resp);
	client = 0;
}

void receivedInsert(Message& m, stringstream& ss) {
//	cout << "GOT MSG id:" << m.data->id << endl;
	DbMessage d(m);
	while( d.moreJSObjs() ) {
		JSObj js = d.nextJsObj();
//		cout << "  temp dbinsert: got js object, size=" << js.objsize() << " ns:" << d.getns() << endl;
		const char *ns = d.getns();
		setClient(ns);
		ss << ns;
		theDataFileMgr.insert(ns, (void*) js.objdata(), js.objsize());
	}
	client = 0;
}

void testTheDb() {
	setClient("sys.unittest.pdfile");

	/* this is not validly formatted, if you query this namespace bad things will happen */
	theDataFileMgr.insert("sys.unittest.pdfile", (void *) "hello worldx", 13);
	theDataFileMgr.insert("sys.unittest.pdfile", (void *) "hello worldx", 13);

	JSObj j1((const char *) &js1);
	deleteObjects("sys.unittest.delete", j1, false);
	theDataFileMgr.insert("sys.unittest.delete", &js1, sizeof(js1));
	deleteObjects("sys.unittest.delete", j1, false);
	updateObjects("sys.unittest.delete", j1, j1, true);
	updateObjects("sys.unittest.delete", j1, j1, false);

	auto_ptr<Cursor> c = theDataFileMgr.findAll("sys.unittest.pdfile");
	while( c->ok() ) {
		Record* r = c->_current();
		c->advance();
	}
	cout << endl;

	client = 0;
}

int port = DBPort;

MessagingPort *grab = 0;
void t();

class OurListener : public Listener { 
public:
	OurListener(int p) : Listener(p) { }
	virtual void accepted(MessagingPort *mp) {
		assert( grab == 0 );
		grab = mp;
		boost::thread thr(t);
		while( grab )
			sleepmillis(1);
	}
};

void listen(int port) { 
	cout << "db version: 27feb08.1 latent cursor fixes" << endl;
	pdfileInit();
	testTheDb();
	cout << curTimeMillis() % 10000 << " waiting for connections...\n" << endl;
	OurListener l(port);
	l.listen();
}

int ctr = 0;

void t()
{
	try { 

	MessagingPort& dbMsgPort = *grab;
	grab = 0;

	Message m;
	while( 1 ) { 
		curOp = 0;
		m.reset();
		stringstream ss;

		if( !dbMsgPort.recv(m) ) {
			cout << "MessagingPort::recv(): returned false" << endl;
			dbMsgPort.shutdown();
			break;
		}

		char buf[64];
		time_t_to_String(time(0), buf);
		buf[20] = 0; // don't want the year
		ss << buf;
		//		ss << curTimeMillis() % 10000 << ' ';

		{
			lock lk(dbMutex);
			Timer t;

			curOp = m.data->operation;
			if( m.data->operation == dbMsg ) { 
				ss << "msg ";
				char *p = m.data->_data;
				int len = strlen(p);
				if( len > 400 ) 
					cout << curTimeMillis() % 10000 << 
					" long msg received, len:" << len << 
					" ends with: " << p + len - 10 << endl;
				bool end = strcmp("end", p) == 0;
				Message resp;
				resp.setData(opReply, "i am fine");
				dbMsgPort.reply(m, resp);
				if( end ) {
					cout << curTimeMillis() % 10000 << "   end msg" << endl;
					dbMsgPort.shutdown();
					sleepmillis(500);
					exit(1);
				}
			}
			else if( m.data->operation == dbQuery ) { 
				receivedQuery(dbMsgPort, m, ss);
			}
			else if( m.data->operation == dbInsert ) {
				try { 
					ss << "insert ";
					receivedInsert(m, ss);
				}
				catch( AssertionException ) { cout << "Caught Assertion, continuing" << endl; }
			}
			else if( m.data->operation == dbUpdate ) {
				try { 
					ss << "update ";
					receivedUpdate(m);
				}
				catch( AssertionException ) { cout << "Caught Assertion, continuing" << endl; }
			}
			else if( m.data->operation == dbDelete ) {
				try { 
					ss << "remove ";
					receivedDelete(m);
				}
				catch( AssertionException ) { cout << "Caught Assertion, continuing" << endl; }
			}
			else if( m.data->operation == dbGetMore ) {
				ss << "getmore ";
				receivedGetMore(dbMsgPort, m);
			}
			else if( m.data->operation == dbKillCursors ) { 
				ss << "killcursors ";
				receivedKillCursors(m);
			}
			else {
				cout << "    operation isn't supported: " << m.data->operation << endl;
				assert(false);
			}

			int ms = t.millis();
			bool log = ctr++ % 100 == 0;
			if( log || ms > 50 ) {
				ss << ' ' << t.millis() << "ms";
				cout << ss.str().c_str() << endl;
			}
		}
	}

	}
	catch( AssertionException ) { 
		cout << "Caught AssertionException, terminating" << endl;
		exit(-7);
	}
}

void msg(const char *m, int extras = 0) { 
	SockAddr db("127.0.0.1", DBPort);
//	SockAddr db("192.168.37.1", MessagingPort::DBPort);
//	SockAddr db("10.0.21.60", MessagingPort::DBPort);
//	SockAddr db("172.16.0.179", MessagingPort::DBPort);

	MessagingPort p;
	if( !p.connect(db) )
		return;

	for( int q = 0; q < 3; q++ ) {
		Message send;
		Message response;

		send.setData( dbMsg , m);
		int len = send.data->dataLen();

		for( int i = 0; i < extras; i++ )
			p.say(db, send);

		Timer t;
		bool ok = p.call(db, send, response);
		double tm = t.micros() + 1;
		cout << " ****ok. response.data:" << ok << " time:" << tm / 1000.0 << "ms " << 
			((double) len) * 8 / 1000000 / (tm/1000000) << "Mbps" << endl;
		if(  q+1 < 3 ) {
			cout << "\t\tSLEEP 8 then sending again" << endl;
			sleepsecs(8);
		}
	}

	p.shutdown();
}

int main(int argc, char* argv[], char *envp[] )
{
	srand(curTimeMillis());

	if( argc >= 2 ) {
	  if( strcmp(argv[1], "quicktest") == 0 ) {
		  quicktest();
		  return 0;
	  }
		if( strcmp(argv[1], "msg") == 0 ) {
			msg(argc >= 3 ? argv[2] : "ping");
			goingAway = true;
			return 0;
		}
		if( strcmp(argv[1], "msglots") == 0 ) {
			msg(argc >= 3 ? argv[2] : "ping", 1000);
			goingAway = true;
			return 0;
		}
		if( strcmp(argv[1], "dev") == 0 ) { 
			dbpath = "/home/dwight/db/";
			cout << "dev mode: expect db files in " << dbpath << endl;
			quicktest();
			port++;
			cout << "listening on port " << port << endl;
			listen(port);
			goingAway = true;
			return 0;
		}
		if( strcmp(argv[1], "run") == 0 ) {
			listen(port);
			goingAway = true;
			return 0;
		}
		if( strcmp(argv[1], "longmsg") == 0 ) {
			char buf[800000];
			memset(buf, 'a', 799999);
			buf[799999] = 0;
			buf[799998] = 'b';
			buf[0] = 'c';
			msg(buf);
			goingAway = true;
			return 0;
		}
	}

	cout << "usage:\n";
	cout << "  quicktest    just check basic assertions and exit" << endl;
	cout << "  msg [msg]    send a request to the db server" << endl;
	cout << "  msg end      shut down" << endl;
	cout << "  run          run db" << endl;
	cout << "  dev          run in dev mode (diff db loc, diff port #)" << endl;
	cout << "  longmsg      send a long test message to the db server" << endl;
	cout << "  msglots      send a bunch of test messages, and then wait for answer o nthe last one" << endl;
	goingAway = true;
	return 0;
}

//#if !defined(_WIN32)
//int main( int argc, char *argv[], char *envp[] ) {
//	return _tmain(argc, 0);
//}
//#endif
