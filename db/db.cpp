// db.cpp : Defines the entry point for the console application.
//

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
#include "db.h"
#include "../grid/message.h"
#include "../util/mmap.h"
#include "../util/hashtab.h"
#include "../util/goodies.h"
#include "pdfile.h"
#include "jsobj.h"
#include "javajs.h"
#include "query.h"
#include "introspect.h"
#include "repl.h"

bool slave = false;
bool master = false; // true means keep an op log
bool useJNI = true;
extern const char *dbpath;
extern int curOp;

/* only off if --nocursors which is for debugging. */
bool useCursors = true;

boost::mutex dbMutex;
int dbLocked = 0;

void closeAllSockets();
void startReplication();

struct MyStartupTests {
	MyStartupTests() {
		assert( sizeof(OID) == 12 );
	}
} mystartupdbcpp;

/* 0 = off; 1 = writes, 2 = reads, 3 = both 
   7 = log a few reads, and all writes.
*/
int opLogging = 0;

struct OpLog { 
	ofstream *f;
	OpLog() : f(0) { }
	void init() { 
		stringstream ss;
		ss << "oplog." << hex << time(0);
		string name = ss.str();
		f = new ofstream(name.c_str(), ios::out | ios::binary);
		if ( ! f->good() ){
		  problem() << "couldn't open log stream" << endl;
		  throw 1717;
		}
	}
	void readop(char *data, int len) { 
		bool log = (opLogging & 4) == 0;
		OCCASIONALLY log = true;
		if( log ) 
			f->write(data,len);
	}
} _oplog;
void flushOpLog() { _oplog.f->flush(); }
#define oplog (*(_oplog.f))
#define OPWRITE if( opLogging & 1 ) oplog.write((char *) m.data, m.data->len);
#define OPREAD if( opLogging & 2 ) _oplog.readop((char *) m.data, m.data->len);

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

void killCursors(int n, long long *ids);
void receivedKillCursors(Message& m) {
	int *x = (int *) m.data->_data;
	x++; // reserved
	int n = *x++;
	assert( n >= 1 );
	if( n > 2000 ) { 
		problem() << "Assertion failure, receivedKillCursors, n=" << n << endl;
		assert( n < 30000 );
	}
	killCursors(n, (long long *) x);
}

void receivedUpdate(Message& m, stringstream& ss) {
	DbMessage d(m);
	const char *ns = d.getns();
	assert(*ns);
	setClient(ns);
	if( client->profile )
		ss << ns << ' ';
	int flags = d.pullInt();
	JSObj query = d.nextJsObj();

	assert( d.moreJSObjs() );
	assert( query.objsize() < m.data->dataLen() );
	JSObj toupdate = d.nextJsObj();

	assert( toupdate.objsize() < m.data->dataLen() );
	assert( query.objsize() + toupdate.objsize() < m.data->dataLen() );
	updateObjects(ns, toupdate, query, flags & 1, ss);
}

void receivedDelete(Message& m) {
	DbMessage d(m);
	const char *ns = d.getns();
	assert(*ns);
	setClient(ns);
	int flags = d.pullInt();
	bool justOne = flags & 1;
	assert( d.moreJSObjs() );
	JSObj pattern = d.nextJsObj();
	deleteObjects(ns, pattern, justOne);
	logOp("d", ns, pattern, 0, &justOne);
}

/* we defer response until we unlock.  don't want a blocked socket to 
   keep things locked.
*/
struct DbResponse { 
	Message *response;
	MSGID responseTo;
	DbResponse(Message *r, MSGID rt) : response(r), responseTo(rt) {
	}
	DbResponse() { response = 0; }
	~DbResponse() { delete response; }
};

void receivedQuery(DbResponse& dbresponse, /*AbstractMessagingPort& dbMsgPort, */Message& m, stringstream& ss, bool logit) {
	MSGID responseTo = m.data->id;

	DbMessage d(m);
	const char *ns = d.getns();

	if( opLogging && logit ) { 
		if( strstr(ns, "$cmd") ) { 
			/* $cmd queries are "commands" and usually best treated as write operations */
			OPWRITE;
		}
		else { 
			OPREAD;
		}
	}

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
		msgdata = runQuery(m, ns, ntoskip, ntoreturn, query, fields, ss, m.data->dataAsInt());
	}
	catch( AssertionException ) { 
		ss << " exception ";
		problem() << " Caught Assertion in runQuery ns:" << ns << endl; 
		log() << "  ntoskip:" << ntoskip << " ntoreturn:" << ntoreturn << '\n';
		log() << "  query:" << query.toString() << '\n';
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
	Message *resp = new Message();
	resp->setData(msgdata, true); // transport will free
	dbresponse.response = resp;
	dbresponse.responseTo = responseTo;
	if( client ) { 
		if( client->profile )
			ss << " bytes:" << resp->data->dataLen();
	}
	else { 
		if( strstr(ns, "$cmd") == 0 ) // (this condition is normal for $cmd dropDatabase)
			log() << "ERROR: receiveQuery: client is null; ns=" << ns << endl;
	}
	//	dbMsgPort.reply(m, resp, responseTo);
}

QueryResult* emptyMoreResult(long long);

void receivedGetMore(DbResponse& dbresponse, /*AbstractMessagingPort& dbMsgPort, */Message& m, stringstream& ss) {
	DbMessage d(m);
	const char *ns = d.getns();
	ss << ns;
	setClient(ns);
	int ntoreturn = d.pullInt();
	long long cursorid = d.pullInt64();
	ss << " cid:" << cursorid;
	ss << " ntoreturn:" << ntoreturn;
	QueryResult* msgdata;
	try { 
		msgdata = getMore(ns, ntoreturn, cursorid);
	}
	catch( AssertionException ) { 
		ss << " exception ";
		msgdata = emptyMoreResult(cursorid);
	}
	Message *resp = new Message();
	resp->setData(msgdata, true);
	ss << " bytes:" << resp->data->dataLen();
	ss << " nreturned:" << msgdata->nReturned;
	dbresponse.response = resp;
	dbresponse.responseTo = m.data->id;
	//dbMsgPort.reply(m, resp);
}

void receivedInsert(Message& m, stringstream& ss) {
//	cout << "GOT MSG id:" << m.data->id << endl;
	DbMessage d(m);
	while( d.moreJSObjs() ) {
		JSObj js = d.nextJsObj();
		const char *ns = d.getns();
		assert(*ns);
		setClient(ns);
		ss << ns;
		theDataFileMgr.insert(ns, (void*) js.objdata(), js.objsize());
		logOp("i", ns, js);
	}
}

void testTheDb() {
	stringstream ss;

	setClient("sys.unittest.pdfile");

	/* this is not validly formatted, if you query this namespace bad things will happen */
	theDataFileMgr.insert("sys.unittest.pdfile", (void *) "hello worldx", 13);
	theDataFileMgr.insert("sys.unittest.pdfile", (void *) "hello worldx", 13);

	JSObj j1((const char *) &js1);
	deleteObjects("sys.unittest.delete", j1, false);
	theDataFileMgr.insert("sys.unittest.delete", &js1, sizeof(js1));
	deleteObjects("sys.unittest.delete", j1, false);
	updateObjects("sys.unittest.delete", j1, j1, true,ss);
	updateObjects("sys.unittest.delete", j1, j1, false,ss);

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
void connThread();

class OurListener : public Listener { 
public:
	OurListener(int p) : Listener(p) { }
	virtual void accepted(MessagingPort *mp) {
		assert( grab == 0 );
		grab = mp;
		boost::thread thr(connThread);
		while( grab )
			sleepmillis(1);
	}
};

/* versions
   114 bad memory bug fixed
   115 replay, opLogging
*/
void listen(int port) { 
	const char *Version = "db version: 121";
	problem() << Version << endl;
	pdfileInit();
	//testTheDb();
	log() << "waiting for connections on port " << port << "..." << endl;
	OurListener l(port);
	startReplication();
	l.listen();
}

int ctr = 0;
extern int callDepth;

class JniMessagingPort : public AbstractMessagingPort { 
public:
	JniMessagingPort(Message& _container) : container(_container) { }
	void reply(Message& received, Message& response, MSGID) {
		container = response;
	}
	void reply(Message& received, Message& response) { 
		container = response;
	}
	Message & container;
};

/* a call from java/js to the database locally. 

     m - inbound message
     out - outbound message, if there is any, will be set here.
           if there is one, out.data will be non-null on return.
		   The out.data buffer will automatically clean up when out
		   goes out of scope (out.freeIt==true)

   note we should already be in the mutex lock from connThread() at this point.
*/
void jniCallback(Message& m, Message& out)
{
	Client *clientOld = client;

	JniMessagingPort jmp(out);
	callDepth++;
	int curOpOld = curOp;

	try { 

		stringstream ss;
		char buf[64];
		time_t_to_String(time(0), buf);
		buf[20] = 0; // don't want the year
		ss << buf << " dbjs ";

		{
			Timer t;

			bool log = false;
			curOp = m.data->operation;

			if( m.data->operation == dbQuery ) { 
				// on a query, the Message must have m.freeIt true so that the buffer data can be 
				// retained by cursors.  As freeIt is false, we make a copy here.
				assert( m.data->len > 0 && m.data->len < 32000000 );
				Message copy(malloc(m.data->len), true);
				memcpy(copy.data, m.data, m.data->len);
				DbResponse dbr;
				receivedQuery(dbr, copy, ss, false);
				jmp.reply(m, *dbr.response, dbr.responseTo);
			}
			else if( m.data->operation == dbInsert ) {
				ss << "insert ";
				receivedInsert(m, ss);
			}
			else if( m.data->operation == dbUpdate ) {
				ss << "update ";
				receivedUpdate(m, ss);
			}
			else if( m.data->operation == dbDelete ) {
				ss << "remove ";
				receivedDelete(m);
			}
			else if( m.data->operation == dbGetMore ) {
				DEV log = true;
				ss << "getmore ";
				DbResponse dbr;
				receivedGetMore(dbr, m, ss);
				jmp.reply(m, *dbr.response, dbr.responseTo);
			}
			else if( m.data->operation == dbKillCursors ) { 
				try {
					log = true;
					ss << "killcursors ";
					receivedKillCursors(m);
				}
				catch( AssertionException ) { 
					problem() << "Caught Assertion in kill cursors, continuing" << endl; 
					ss << " exception ";
				}
			}
			else {
				cout << "    jnicall: operation isn't supported: " << m.data->operation << endl;
				assert(false);
			}

			int ms = t.millis();
			log = log || ctr++ % 128 == 0;
			if( log || ms > 100 ) {
				ss << ' ' << t.millis() << "ms";
				cout << ss.str().c_str() << endl;
			}
			if( client && client->profile >= 1 ) { 
				if( client->profile >= 2 || ms >= 100 ) { 
					// profile it
					profile(ss.str().c_str()+20/*skip ts*/, ms);
				}
			}
		}

	}
	catch( AssertionException ) { 
		problem() << "Caught AssertionException in jniCall()" << endl;
	}

	curOp = curOpOld;
	callDepth--;

	if( client != clientOld ) { 
		client = clientOld;
		wassert(false);
	}
}

/* we create one thread for each connection from an app server client.  
   app server will open a pool of threads.
*/
void connThread()
{
	try { 

	MessagingPort& dbMsgPort = *grab;
	grab = 0;

	Message m;
	while( 1 ) { 
		m.reset();
		stringstream ss;

		if( !dbMsgPort.recv(m) ) {
			log() << "end connection " << dbMsgPort.farEnd.toString() << endl;
			dbMsgPort.shutdown();
			break;
		}

		char buf[64];
		time_t_to_String(time(0), buf);
		buf[20] = 0; // don't want the year
		ss << buf;
		//		ss << curTimeMillis() % 10000 << ' ';

		DbResponse dbresponse;
		{
			dblock lk;
			Timer t;
			client = 0;
			curOp = 0;

			int ms;
			bool log = false;
			curOp = m.data->operation;

#if 0
				/* use this if you only want to process operations for a particular namespace.  
				maybe add to cmd line parms or something fancier.
				*/
				DbMessage ddd(m);
				if( strncmp(ddd.getns(), "clusterstock", 12) != 0 ) { 
					static int q;
					if( ++q < 20 ) 
						cout << "TEMP skip " << ddd.getns() << endl;
					goto skip;
				}
#endif

			if( m.data->operation == dbMsg ) { 
				ss << "msg ";
				char *p = m.data->_data;
				int len = strlen(p);
				if( len > 400 ) 
					cout << curTimeMillis() % 10000 << 
					" long msg received, len:" << len << 
					" ends with: " << p + len - 10 << endl;
				bool end = strcmp("end", p) == 0;
				Message *resp = new Message();
				resp->setData(opReply, "i am fine");
				dbresponse.response = resp;
				dbresponse.responseTo = m.data->id;
				//dbMsgPort.reply(m, resp);
				if( end ) {
					cout << curTimeMillis() % 10000 << "   end msg " << dbMsgPort.farEnd.toString() << endl;
					if( dbMsgPort.farEnd.isLocalHost() ) { 
						dbMsgPort.shutdown();
						sleepmillis(50);
						problem() << "exiting end msg" << endl;
						exit(EXIT_SUCCESS);
					}
					else { 
						cout << "  (not from localhost, ignoring end msg)" << endl;
					}
				}
			}
			else if( m.data->operation == dbQuery ) { 
				receivedQuery(dbresponse, m, ss, true);
			}
			else if( m.data->operation == dbInsert ) {
				OPWRITE;
				try { 
					ss << "insert ";
					receivedInsert(m, ss);
				}
				catch( AssertionException ) { 
					problem() << " Caught Assertion insert, continuing" << endl; 
					ss << " exception ";
				}
			}
			else if( m.data->operation == dbUpdate ) {
				OPWRITE;
				try { 
					ss << "update ";
					receivedUpdate(m, ss);
				}
				catch( AssertionException ) { 
					problem() << " Caught Assertion update, continuing" << endl; 
					ss << " exception ";
				}
			}
			else if( m.data->operation == dbDelete ) {
				OPWRITE;
				try { 
					ss << "remove ";
					receivedDelete(m);
				}
				catch( AssertionException ) { 
					problem() << " Caught Assertion receivedDelete, continuing" << endl; 
					ss << " exception ";
				}
			}
			else if( m.data->operation == dbGetMore ) {
				OPREAD;
				DEV log = true;
				ss << "getmore ";
				receivedGetMore(dbresponse, m, ss);
			}
			else if( m.data->operation == dbKillCursors ) { 
				OPREAD;
				try {
					log = true;
					ss << "killcursors ";
					receivedKillCursors(m);
				}
				catch( AssertionException ) { 
					problem() << " Caught Assertion in kill cursors, continuing" << endl; 
					ss << " exception ";
				}
			}
			else {
				cout << "    operation isn't supported: " << m.data->operation << endl;
				assert(false);
			}

			ms = t.millis();
			log = log || ctr++ % 512 == 0;
			DEV log = true;
			if( log || ms > 100 ) {
				ss << ' ' << t.millis() << "ms";
				cout << ss.str().c_str() << endl;
			}
//skip:
			if( client && client->profile >= 1 ) { 
				if( client->profile >= 2 || ms >= 100 ) { 
					// profile it
					profile(ss.str().c_str()+20/*skip ts*/, ms);
				}
			}

		} /* end lock */
		if( dbresponse.response ) 
			dbMsgPort.reply(m, *dbresponse.response, dbresponse.responseTo);
	}

	}
	catch( AssertionException ) { 
		problem() << "Uncaught AssertionException, terminating" << endl;
		exit(15);
	}
}


void msg(const char *m, const char *address, int port, int extras = 0) {

    SockAddr db(address, port);

//	SockAddr db("127.0.0.1", DBPort);
//	SockAddr db("192.168.37.1", MessagingPort::DBPort);
//	SockAddr db("10.0.21.60", MessagingPort::DBPort);
//	SockAddr db("172.16.0.179", MessagingPort::DBPort);

	MessagingPort p;
	if( !p.connect(db) )
		return;

	const int Loops = 1;
	for( int q = 0; q < Loops; q++ ) {
		Message send;
		Message response;

		send.setData( dbMsg , m);
		int len = send.data->dataLen();

		for( int i = 0; i < extras; i++ )
			p.say(/*db, */send);

		Timer t;
		bool ok = p.call(send, response);
		double tm = t.micros() + 1;
		cout << " ****ok. response.data:" << ok << " time:" << tm / 1000.0 << "ms " << 
			((double) len) * 8 / 1000000 / (tm/1000000) << "Mbps" << endl;
		if(  q+1 < Loops ) {
			cout << "\t\tSLEEP 8 then sending again as a test" << endl;
			sleepsecs(8);
		}
	}
	sleepsecs(1);

	p.shutdown();
}

void msg(const char *m, int extras = 0) { 
    msg(m, "127.0.0.1", DBPort, extras);
}

#if !defined(_WIN32)

#include <signal.h>

void pipeSigHandler( int signal ) {
  psignal( signal, "Signal Received : ");
}

int segvs = 0;
void segvhandler(int x) {
	if( ++segvs > 1 ) {
		signal(x, SIG_DFL);
		if( segvs == 2 ) {
			cout << "\n\n\n got 2nd SIGSEGV" << endl;
			sayDbContext();
		}
		return;
	}
	problem() << "got SIGSEGV " << x << ", terminating :-(" << endl;
	sayDbContext();
//	closeAllSockets();
//	MemoryMappedFile::closeAllFiles();
//	flushOpLog();
	dbexit(14);
}

void mysighandler(int x) { 
   signal(x, SIG_IGN); 
   cout << "got kill or ctrl c signal " << x << ", will terminate after current cmd ends" << endl;
   problem() << "got kill or ctrl c signal " << x << ", will terminate after current cmd ends" << endl;
   {
	   dblock lk;
	   problem() << "  now exiting" << endl;
	   exit(12);
   }
}

void setupSignals() {
	assert( signal(SIGINT, mysighandler) != SIG_ERR );
	assert( signal(SIGTERM, mysighandler) != SIG_ERR );
}

#else
void setupSignals() {}
#endif

void initAndListen(int listenPort, const char *dbPath, const char *appserverLoc = null) { 
  if( opLogging ) 
		log() << "opLogging = " << opLogging << endl;
    _oplog.init();

#if !defined(_WIN32)
	assert( signal(SIGSEGV, segvhandler) != SIG_ERR );
#endif

    /*
     * ensure that the dbpath ends with a path delim if not supplied
     * @TODO - the following is embarassing - not sure of there's a clean way to
     * find the platform delim
     */

	char endchar = '/';
	char *endstr = "/";

#if defined(_WIN32)
    endchar = '\\';
    endstr = "\\";
#endif
    
    if (dbPath && dbPath[strlen(dbPath)-1] != endchar) {
    	char *t = (char *) malloc(strlen(dbPath) + 2);

        strcpy(t, dbPath);
        strcat(t, endstr);
        dbPath = t;
    }

    dbpath = dbPath;

#if !defined(_WIN32)
    pid_t pid = 0;
    pid = getpid();
#else
	int pid=0;
#endif
    
    log() << "Mongo DB : starting : pid = " << pid << " port = " << port << " dbpath = " << dbpath 
            <<  " master = " << master << " slave = " << slave << endl;

    if( useJNI ) {
      JavaJS = new JavaJSImpl(appserverLoc);
      javajstest();
    }

	setupSignals();

    listen(listenPort);    
}

//ofstream problems("dbproblems.log", ios_base::app | ios_base::out);
int test2();
void testClient();

int main(int argc, char* argv[], char *envp[] )
{
	DEV cout << "warning: DEV mode enabled\n";

#if !defined(_WIN32)
    signal(SIGPIPE, pipeSigHandler);
#endif
	srand(curTimeMillis());

	if( argc >= 2 ) {
		if( strcmp(argv[1], "quicktest") == 0 ) {
			quicktest();
			return 0;
		}
		if( strcmp(argv[1], "test2") == 0 ) {
			return test2();
		}
		if( strcmp(argv[1], "msg") == 0 ) {

		    // msg(argc >= 3 ? argv[2] : "ping");

		    const char *m = "ping";
		    int thePort = DBPort;
		    
		    if (argc >= 3) { 
		        m = argv[2];
		        
		        if (argc > 3) { 
		            thePort = atoi(argv[3]);
		        }
		    }
		    
		    msg(m, "127.0.0.1", thePort);
		    
			return 0;
		}
		if( strcmp(argv[1], "msglots") == 0 ) {
			msg(argc >= 3 ? argv[2] : "ping", 1000);
			return 0;
		}
		if( strcmp( argv[1], "testclient") == 0 ) { 
			testClient();
			return 0;
		}
		if( strcmp(argv[1], "zzz") == 0 ) {
			msg(argc >= 3 ? argv[2] : "ping", 1000);
			return 0;
		}
		if( strcmp(argv[1], "run") == 0 ) {
			initAndListen(port, dbpath);
			return 0;
		}
		if( strcmp(argv[1], "longmsg") == 0 ) {
			char buf[800000];
			memset(buf, 'a', 799999);
			buf[799999] = 0;
			buf[799998] = 'b';
			buf[0] = 'c';
			msg(buf);
			return 0;
		}

        /*
         *  *** POST STANDARD SWITCH METHOD - if we don't satisfy, we switch to a 
         *     slightly different mode where "run" is assumed and we can set values
         */
		
        char *appsrvPath = null;
		
        for (int i = 1; i < argc; i++)  {
    
			if( argv[i] == 0 ) continue;
			string s = argv[i];

			if( s == "--port" )
                port = atoi(argv[++i]);
			else if( s == "--nojni" )
				useJNI = false;
			else if( s == "--master" )
				master = true;
			else if( s == "--slave" )
				slave = true;
			else if( s == "--dbpath" )
            	dbpath = argv[++i];
            else if( s == "--appsrvpath" )
                appsrvPath = argv[++i];
			else if( s == "--nocursors" ) 
				useCursors = false;
			else if( strncmp(s.c_str(), "--oplog", 7) == 0 ) { 
				int x = s[7] - '0';
				if( x < 0 || x > 7 ) { 
					cout << "can't interpret --oplog setting" << endl;
					exit(13);
				}
				opLogging = x;
			}
        }
        
        initAndListen(port, dbpath, appsrvPath);
        
		exit(0);
	}

	cout << "Mongo db usage:\n";
	cout << "  run               run db" << endl;
	cout << "  msg end [port]    shut down db server listening on port (or default)" << endl;
	cout << "  msg [msg] [port]  send a request to the db server listening on port (or default)" << endl;
	cout << "  msglots           send a bunch of test messages, and then wait for answer on the last one" << endl;
	cout << "  longmsg           send a long test message to the db server" << endl;
	cout << "  quicktest         just check basic assertions and exit" << endl;
	cout << "  test2             run test2() - see code" << endl;
	cout << endl << "Alternate Usage :" << endl;
	cout << " --master --slave" << endl;
	cout << " --port <portno>  --dbpath <root> --appsrvpath <root of appsrv>" << endl;
	cout << " --nocursors  --nojni" << endl;
	cout << " --oplog<n> 0=off 1=W 2=R 3=both 7=W+some reads" << endl;
	cout << endl;
	
	return 0;
}

//#if !defined(_WIN32)
//int main( int argc, char *argv[], char *envp[] ) {
//	return _tmain(argc, 0);
//}
//#endif

#undef exit
void dbexit(int rc, const char *why) { 
	cout << "  dbexit: " << why << "; flushing op log and files" << endl;
	flushOpLog();

	/* must do this before unmapping mem or you may get a seg fault */
	closeAllSockets();

	MemoryMappedFile::closeAllFiles();
	cout << "  dbexit: really exiting now" << endl;
	exit(rc);
}
