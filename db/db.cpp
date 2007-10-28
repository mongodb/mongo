// db.cpp : Defines the entry point for the console application.
//

#include "stdafx.h"
#include "db.h"
#include "../grid/message.h"
#include "../util/mmap.h"
#include "../util/hashtab.h"
#include "pdfile.h"
#include "jsobj.h"
#include "query.h"

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
	cout << "quicktest\n";

	MemoryMappedFile mmf;
	char *m = (char *) mmf.map("/tmp/abc", 16384);
	//	cout << "mmf reads: " << m << endl;
	strcpy_s(m, 1000, "hello worldz");
}

MessagingPort dbMsgPort;
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
		if( js.size <= 4 )
			nextjsobj = null;
		else {
			nextjsobj += js.size;
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

Record* findByOID(const char *ns, OID *oid) {
	// temp implementation
	Cursor c = theDataFileMgr.findAll(ns);
	while( c.ok() ) {
		Record *r = c.current();
		JSObj js(r);
		OID *i = js.getOID();
		if( i && *oid == *i )
			return r;
		c.advance();
	}
	return 0;
}

void updateByOID(const char *ns, char *objdata, int objsize, OID *oid) {
	Record *r = findByOID(ns, oid);
	if( r == 0 ) { 
		cout << "updateByOID: no such record " << ns << endl;
		return;
	}
	if( objsize > r->netLength() ) {
		cout << "ERROR: updateByOID: growing records not implemented yet." << endl;
		return;
	}
	/* note: need to be smarter if it gets a lot smaller? */
	/* this really dumb for now as it gets smaller but doesn't allow regrowth 
	to the original size! */
	memcpy(r->data, objdata, objsize);
	r->setNewLength(objsize);
}

#pragma pack(push)
#pragma pack(1)
struct EmptyObject {
	EmptyObject() { len = 5; jstype = EOO; }
	int len;
	char jstype;
} emptyObject;
#pragma pack(pop)

void query(Message& m) {
	DbMessage d(m);
	const char *query;
	int ntoreturn;
	d.getQueryStuff(query, ntoreturn);

	QueryResult* msgdata = runQuery(d.getns(), query, ntoreturn);
	Message resp;
	resp.setData(msgdata, true);
	dbMsgPort.reply(m, resp);
}

void getbyoid(Message& m) {
	DbMessage d(m);
	Record *r = findByOID(d.getns(), d.getOID());
	Message resp;
	if( r == 0 )
		resp.setData(opReply, (char *) &emptyObject, emptyObject.len);
	else
		resp.setData(opReply, r->data, r->netLength());
	dbMsgPort.reply(m, resp);
}

void dbinsert(Message& m) {
	DbMessage d(m);
	while( d.moreJSObjs() ) {
		JSObj js = d.nextJsObj();
		cout << "  temp dbinsert: got js object, size=" << js.objsize() << " ns:" << d.getns() << endl;
		if( m.data->operation == dbInsert ) {
			theDataFileMgr.insert(d.getns(), (void*) js.objdata(), js.objsize());
		} else {
			// update
			OID *oid = js.getOID();
			if( oid == null )
				cout << "error: no oid on update -- that isn't coded yet" << endl;
			else
				updateByOID(d.getns(), (char *) js.objdata(), js.objsize(), oid);
		}
	}
}

void run() { 
	dbMsgPort.init(MessagingPort::DBPort);

	pdfileInit();

	theDataFileMgr.insert("sys.unittest.pdfile", (void *) "hello world", 12);
	cout << "findAll:\n";
	Cursor c = theDataFileMgr.findAll("sys.unittest.pdfile");
	while( c.ok() ) {
		Record* r = c.current();
		cout << "  gotrec " << r->netLength() << ' ' << 
			r->data << '\n';
		c.advance();
	}
	cout << endl;

	Message m;
	while( 1 ) { 
		cout << "waiting for msg..." << endl;
		m.reset();
		if( !dbMsgPort.recv(m) ) {
			cout << "recv() returned false" << endl;
			break;
		}
		cout << "  got msg" << endl;
		cout << "  op:" << m.data->operation << " len:" << m.data->len << endl;

		if( m.data->operation == dbMsg ) { 
			bool end = strcmp("end", m.data->_data) == 0;
			Message resp;
			resp.setData(opReply, "i am fine");
			dbMsgPort.reply(m, resp);
			if( end ) {
				cout << "    end msg" << endl;
				break;
			}
		}
		else if( m.data->operation == dbUpdate || dbInsert ) {
			dbinsert(m);
		}
		else if( m.data->operation == dbGetByOID ) {
			getbyoid(m);
		}
		else if( m.data->operation == dbQuery ) { 
			query(m);
		}
		else if( m.data->operation == dbGetMore ) {
			cout << "dbGetMore: not implemented!" << endl;
		}
		else {
			cout << "    operation isn't supported ?" << endl;
		}
	}
}

void msg(const char *m) { 
	MessagingPort p;
	p.init(29999);

	SockAddr db("127.0.0.1", MessagingPort::DBPort);

	Message send;
	Message response;

	send.setData(1000, m);

	cout << "contacting DB..." << endl;
	bool ok = p.call(db, send, response);
	cout << "ok: " << ok << endl;
	cout << "  " << response.data->id << endl;
	cout << "  " << response.data->len << endl;
	cout << "  " << response.data->operation << endl;
	cout << "  " << response.data->reserved << endl;
	cout << "  " << response.data->responseTo << endl;
	cout << "  " << response.data->_data << endl;

}

int main(int argc, char* argv[], char *envp[] )
{
	quicktest();

	if( argc >= 2 ) {
		if( strcmp(argv[1], "quicktest") == 0 )
			return 0;
		if( strcmp(argv[1], "msg") == 0 ) {
			msg(argc >= 3 ? argv[2] : "ping");
			return 0;
		}
		if( strcmp(argv[1], "run") == 0 ) {
			run();
			return 0;
		}
	}

	cout << "usage:\n";
	cout << "  quicktest    just check basic assertions and exit" << endl;
	cout << "  msg [msg]    send a request to the db server" << endl;
	cout << "  msg end      shut down" << endl;
	cout << "  run          run db" << endl;
	return 0;
}

//#if !defined(_WIN32)
//int main( int argc, char *argv[], char *envp[] ) {
//	return _tmain(argc, 0);
//}
//#endif
