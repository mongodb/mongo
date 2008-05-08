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
#include "javajs.h"
#include "query.h"
#include "introspect.h"

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

void killCursors(int n, long long *ids);
void receivedKillCursors(Message& m) {
	int *x = (int *) m.data->_data;
	x++; // reserved
	int n = *x++;
	assert( n >= 1 );
	if( n > 2000 ) { 
		problem() << "Assertion failure, receivedKillCursors, n=" << n << endl;
		cout << "Assertion failure, receivedKillCursors, n=" << n << endl;
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
	assert( d.moreJSObjs() );
	JSObj pattern = d.nextJsObj();
	deleteObjects(ns, pattern, flags & 1);
}

void receivedQuery(AbstractMessagingPort& dbMsgPort, Message& m, stringstream& ss) {
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
		ss << " exception ";
		problem() << " Caught Assertion in runQuery " << ns << endl; 
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
	if( client ) { 
		if( client->profile )
			ss << " bytes:" << resp.data->dataLen();
	}
	else { 
		cout << "ERROR: client is null; ns=" << ns << endl;
	}
	dbMsgPort.reply(m, resp);
}

void receivedGetMore(AbstractMessagingPort& dbMsgPort, Message& m, stringstream& ss) {
	DbMessage d(m);
	const char *ns = d.getns();
	ss << ns;
	setClient(ns);
	int ntoreturn = d.pullInt();
	long long cursorid = d.pullInt64();
	ss << " cid:" << cursorid;
	ss << " ntoreturn:" << ntoreturn;
	QueryResult* msgdata = getMore(ns, ntoreturn, cursorid);
	Message resp;
	resp.setData(msgdata, true);
	ss << " bytes:" << resp.data->dataLen();
	ss << " nreturned:" << msgdata->nReturned;
	dbMsgPort.reply(m, resp);
}

void receivedInsert(Message& m, stringstream& ss) {
//	cout << "GOT MSG id:" << m.data->id << endl;
	DbMessage d(m);
	while( d.moreJSObjs() ) {
		JSObj js = d.nextJsObj();
//		cout << "  temp dbinsert: got js object, size=" << js.objsize() << " ns:" << d.getns() << endl;
		const char *ns = d.getns();
		assert(*ns);
		setClient(ns);
		ss << ns;
		theDataFileMgr.insert(ns, (void*) js.objdata(), js.objsize());
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

void listen(int port) { 
	const char *Version = "db version: 109 30apr2008 sai hack,keylen";
	problem() << Version << endl;
	cout << Version << endl;
	pdfileInit();
	testTheDb();
	cout << curTimeMillis() % 10000 << " waiting for connections on port " << port << " ...\n" << endl;
	OurListener l(port);
	l.listen();
}

int ctr = 0;
extern int callDepth;

class JniMessagingPort : public AbstractMessagingPort { 
public:
	JniMessagingPort(Message& _container) : container(_container) { }
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
				receivedQuery(jmp, m, ss);
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
				log = true;
				ss << "getmore ";
				receivedGetMore(jmp, m, ss);
			}
			else if( m.data->operation == dbKillCursors ) { 
				try {
					log = true;
					ss << "killcursors ";
					receivedKillCursors(m);
				}
				catch( AssertionException ) { 
					problem() << "Caught Assertion in kill cursors, continuing" << endl; 
					cout << "Caught Assertion in kill cursors, continuing" << endl; 
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
		cout << "Caught AssertionException in jniCall()" << endl;
	}

	curOp = curOpOld;
	callDepth--;

	if( client != clientOld ) { 
		client = clientOld;
		wassert(false);
	}
}

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
			client = 0;
			curOp = 0;

			bool log = false;
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
					problem() << "exiting end msg" << endl;
					exit(EXIT_SUCCESS);
				}
			}
			else if( m.data->operation == dbQuery ) { 
#if defined(_WIN32)
				log = true;
#endif
				receivedQuery(dbMsgPort, m, ss);
			}
			else if( m.data->operation == dbInsert ) {
				try { 
					ss << "insert ";
					receivedInsert(m, ss);
				}
				catch( AssertionException ) { 
					problem() << "Caught Assertion insert, continuing" << endl; 
					cout << "Caught Assertion, continuing" << endl; 
					ss << " exception ";
				}
			}
			else if( m.data->operation == dbUpdate ) {
				try { 
					ss << "update ";
					receivedUpdate(m, ss);
				}
				catch( AssertionException ) { 
					problem() << "Caught Assertion update, continuing" << endl; 
					cout << "Caught Assertion update, continuing" << endl; 
					ss << " exception ";
				}
			}
			else if( m.data->operation == dbDelete ) {
				try { 
					ss << "remove ";
					receivedDelete(m);
				}
				catch( AssertionException ) { 
					problem() << "Caught Assertion receviedDelete, continuing" << endl; 
					cout << "Caught Assertion receviedDelete, continuing" << endl; 
					ss << " exception ";
				}
			}
			else if( m.data->operation == dbGetMore ) {
				log = true;
				ss << "getmore ";
				receivedGetMore(dbMsgPort, m, ss);
			}
			else if( m.data->operation == dbKillCursors ) { 
				try {
					log = true;
					ss << "killcursors ";
					receivedKillCursors(m);
				}
				catch( AssertionException ) { 
					cout << "Caught Assertion in kill cursors, continuing" << endl; 
					problem() << "Caught Assertion in kill cursors, continuing" << endl; 
					ss << " exception ";
				}
			}
			else {
				cout << "    operation isn't supported: " << m.data->operation << endl;
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

	}
	catch( AssertionException ) { 
		cout << "Caught AssertionException, terminating" << endl;
		problem() << "Caught AssertionException, terminating" << endl;
		exit(-7);
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

void msg(const char *m, int extras = 0) { 
    msg(m, "127.0.0.1", DBPort, extras);
}

#if !defined(_WIN32)

#include <signal.h>

void pipeSigHandler( int signal ) {
  psignal( signal, "Signal Received : ");
}

void segvhandler(int x) { 
	cout << "got SIGSEGV " << x << ", terminating :-(" << endl;
	problem() << "got SIGSEGV " << x << ", terminating :-(" << endl;
	exit(-9);
}

void mysighandler(int x) { 
	// [dm] not working.  why? 
	// [gmj] because the VM catches sig 3 to do a thread dump
	cout << "got kill or ctrl c signal " << x << ", terminating" << endl;
	problem() << "got kill or ctrl c signal " << x << ", terminating" << endl;
	exit(0);
}

void setupSignals() {
	cout << "SETUPSIGNALS " << signal(SIGINT, mysighandler) << endl;
//	assert( signal(SIGINT, mysighandler) != SIG_ERR );
	assert( signal(SIGTERM, mysighandler) != SIG_ERR );
	assert( signal(SIGQUIT, mysighandler) != SIG_ERR );
	assert( signal(SIGSEGV, segvhandler) != SIG_ERR );
}

#else
void setupSignals() {}
#endif

void initAndListen(int listenPort, const char *dbPath) { 

	setupSignals();

    /*
     * ensure that the dbpath ends w/ '/' as that's key in preventing things like 
     *    /data/dbadmin.ns
     */
    
    if (dbPath && dbPath[strlen(dbPath)-1] != '/') {
    	char *t = (char *) malloc(strlen(dbPath) + 2);
    	
    	strcpy(t, dbPath);
    	strcat(t, "/");
    	dbPath = t;
    }

    dbpath = dbPath;

#if !defined(_WIN32)
    pid_t pid = 0;
    pid = getpid();
#else
	int pid=0;
#endif
    
    cout << "10Gen DB : starting : pid = " << pid << " port = " << port << " dbpath = " << dbpath << endl;
    problem() << "10Gen DB : starting : pid = " << pid << " port = " << port << " dbpath = " << dbpath << endl;

    JavaJS = new JavaJSImpl();
    javajstest();

    listen(listenPort);    
}

ofstream problems("dbproblems.log", ios_base::app | ios_base::out);

int main(int argc, char* argv[], char *envp[] )
{
#if !defined(_WIN32)
    signal(SIGPIPE, pipeSigHandler);
#endif
	srand(curTimeMillis());

	if( argc >= 2 ) {
	  if( strcmp(argv[1], "quicktest") == 0 ) {
		  quicktest();
		  return 0;
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
		    
		    initAndListen(port, dbpath);
		    
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

        /*
         *  *** POST STANDARD SWITCH METHOD - if we don't satisfy, we switch to a 
         *     slightly different mode where "run" is assumed and we can set values
         */
		
        for (int i = 1; i < argc; i++)  {
    
            char *s = argv[i];
            
            if (s && strcmp(s, "--port") == 0) { 
                port = atoi(argv[++i]);
            }
            else if (s && strcmp(s, "--dbpath") == 0) { 
            	dbpath = argv[++i];
            }
        }
        
        initAndListen(port, dbpath);
        
        goingAway = true;
        return 0;
	}

	cout << "usage:\n";
	cout << "  quicktest         just check basic assertions and exit" << endl;
	cout << "  msg [msg] [port]  send a request to the db server listening on port (or default)" << endl;
	cout << "  msg end [port]    shut down db server listening on port (or default)" << endl;
	cout << "  run               run db" << endl;
	cout << "  dev               run in dev mode (diff db loc, diff port #)" << endl;
	cout << "  longmsg           send a long test message to the db server" << endl;
	cout << "  msglots           send a bunch of test messages, and then wait for answer on the last one" << endl;
	cout << endl << "Alternate Usage :" << endl;
	cout << " --port <portno>  --dbpath <root>" << endl << endl;
	
	goingAway = true;
	return 0;
}

//#if !defined(_WIN32)
//int main( int argc, char *argv[], char *envp[] ) {
//	return _tmain(argc, 0);
//}
//#endif
