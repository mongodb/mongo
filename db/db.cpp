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
	char *m = (char *) mmf.map("/tmp/abc", 16384);
	//	cout << "mmf reads: " << m << endl;
	strcpy_s(m, 1000, "hello worldz");
}

MessagingPort dbMsgPort(MessagingPort::ANYCHANNEL);
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

void receivedUpdate(Message& m) {
	DbMessage d(m);
	const char *ns = d.getns();
	int flags = d.pullInt();
	JSObj query = d.nextJsObj();
	assert( d.moreJSObjs() );
	JSObj toupdate = d.nextJsObj();
	updateObjects(ns, toupdate, query, flags & 1);
}

void receivedDelete(Message& m) {
	DbMessage d(m);
	const char *ns = d.getns();
	int flags = d.pullInt();
	assert( d.moreJSObjs() );
	JSObj pattern = d.nextJsObj();
	deleteObjects(ns, pattern, flags & 1);
}

void receivedQuery(Message& m) {
	DbMessage d(m);
	const char *ns = d.getns();
	int ntoreturn = d.pullInt();
	JSObj query = d.nextJsObj();
	auto_ptr< set<string> > fields;
	if( d.moreJSObjs() ) { 
		fields = auto_ptr< set<string> >(new set<string>());
		d.nextJsObj().getFieldNames(*fields);
	}
	QueryResult* msgdata = 
		runQuery(ns, ntoreturn, query, fields);
	Message resp;
	resp.setData(msgdata, true);
	dbMsgPort.reply(m, resp);
}

void receivedGetMore(Message& m) {
	DbMessage d(m);
	const char *ns = d.getns();
	int ntoreturn = d.pullInt();
	long long cursorid = d.pullInt64();
	QueryResult* msgdata = getMore(ns, ntoreturn, cursorid);
	Message resp;
	resp.setData(msgdata, true);
	dbMsgPort.reply(m, resp);
}

/*void getbyoid(Message& m) {
	DbMessage d(m);
	Record *r = findByOID(d.getns(), d.getOID());
	Message resp;
	if( r == 0 )
		resp.setData(opReply, (char *) &emptyObject, emptyObject.len);
	else
		resp.setData(opReply, r->data, r->netLength());
	dbMsgPort.reply(m, resp);
}*/

void receivedInsert(Message& m) {
	DbMessage d(m);
	while( d.moreJSObjs() ) {
		JSObj js = d.nextJsObj();
		cout << "  temp dbinsert: got js object, size=" << js.objsize() << " ns:" << d.getns() << endl;
		theDataFileMgr.insert(d.getns(), (void*) js.objdata(), js.objsize());
	}
}

void testTheDb() {
	/* this is not validly formatted, if you query this namespace bad things will happen */
	theDataFileMgr.insert("sys.unittest.pdfile", (void *) "hello worldx", 13);
	theDataFileMgr.insert("sys.unittest.pdfile", (void *) "hello worldx", 13);

	JSObj j1((const char *) &js1);
	deleteObjects("sys.unittest.delete", j1, false);
	theDataFileMgr.insert("sys.unittest.delete", &js1, sizeof(js1));
	deleteObjects("sys.unittest.delete", j1, false);
	updateObjects("sys.unittest.delete", j1, j1, true);
	updateObjects("sys.unittest.delete", j1, j1, false);

	cout << "findAll:\n";
	auto_ptr<Cursor> c = theDataFileMgr.findAll("sys.unittest.pdfile");
	while( c->ok() ) {
		Record* r = c->_current();
		cout << "  gotrec " << r->netLength() << ' ' << 
			r->data << '\n';
		c->advance();
	}
	cout << endl;
}

int port = MessagingPort::DBPort;

void run() { 
	dbMsgPort.init(port);

	pdfileInit();

	testTheDb();

	Message m;
	while( 1 ) { 
		m.reset();

		// temp:
		//		sleepsecs(1);

		cout << curTimeMillis() % 10000 << " waiting for msg..." << endl;
		if( !dbMsgPort.recv(m) ) {
			cout << "MessagingPort::recv() returned false" << endl;
			break;
		}

		cout << curTimeMillis() % 10000 << " db.cpp got msg" << endl;

		//cout << "  got msg" << endl;
		//cout << "  op:" << m.data->operation << " len:" << m.data->len << endl;

		if( m.data->operation == dbMsg ) { 
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
				break;
			}
		}
		else if( m.data->operation == dbQuery ) { 
			receivedQuery(m);
		}
		else if( m.data->operation == dbInsert ) {
			receivedInsert(m);
		}
		else if( m.data->operation == dbUpdate ) {
			receivedUpdate(m);
		}
		else if( m.data->operation == dbDelete ) {
			receivedDelete(m);
		}
		else if( m.data->operation == dbGetMore ) {
			receivedGetMore(m);
		}
		else {
			cout << "    operation isn't supported ?" << endl;
		}
	}
}

void msg(const char *m, int extras = 0) { 
	MessagingPort p;
	p.init(29999);

//	SockAddr db("192.168.37.1", MessagingPort::DBPort);
	SockAddr db("127.0.0.1", MessagingPort::DBPort);
//	SockAddr db("10.0.21.60", MessagingPort::DBPort);
//	SockAddr db("172.16.0.179", MessagingPort::DBPort);

for( int q = 0; q < 3; q++ ) {
	Message send;
	Message response;

	send.setData( dbMsg , m);

	for( int i = 0; i < extras; i++ )
		p.say(p.channel(), db, send);

	cout << curTimeMillis() % 10000 << " ****calling DB..." << endl;
	bool ok = p.call(db, send, response);
	cout << curTimeMillis() % 10000 << " ****ok. response.data:" << ok << " ****" << endl;
/*	cout << "  " << response.data->id << endl;
	cout << "  " << response.data->len << endl;
	cout << "  " << response.data->operation << endl;
	cout << "  " << response.data->responseTo << endl;*/
	cout << " data: " << response.data->_data << endl;

				cout << "SLEEP 8 then sending again ----------------------------------------" << endl;
				sleepsecs(8);
}

	p.shutdown();
}

void bar() { 
cout << "hello" << endl;
sleepsecs(6);
cout << "hello2" << endl;
}

int main(int argc, char* argv[], char *envp[] )
{
	srand(curTimeMillis());
	cout << curTimeMillis() % 10000 << endl;

	quicktest();

	if( argc >= 2 ) {
		if( strcmp(argv[1], "quicktest") == 0 )
			return 0;
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
			cout << "dev mode: expect db files in ~/db/" << endl;
			dbpath = "~/db/";
			port++;
			cout << "listening on port " << port << endl;
			run();
			goingAway = true;
			return 0;
		}
		if( strcmp(argv[1], "run") == 0 ) {
			run();
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
	cout << "  dev          run in dev mode (~/db/, diff port #)" << endl;
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
