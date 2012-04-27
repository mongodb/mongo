
/** @file perftests.cpp.cpp : unit tests relating to performance

          The idea herein is tests that run fast and can be part of the normal CI suite.  So no tests herein that take
          a long time to run.  Obviously we need those too, but they will be separate.

          These tests use DBDirectClient; they are a bit white-boxish.
*/

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

#include "pch.h"
#include <fstream>
#include "../db/db.h"
#include "../db/instance.h"
#include "../db/json.h"
#include "../db/lasterror.h"
#include "../db/taskqueue.h"
#include "../util/timer.h"
#include "dbtests.h"
#include "../db/dur_stats.h"
#include "../util/checksum.h"
#include "../util/version.h"
#include "../db/key.h"
#include "../util/compress.h"
#include "../util/concurrency/qlock.h"
#include <boost/filesystem/operations.hpp>

using namespace bson;

namespace mongo {
    namespace dbtests {
        extern unsigned perfHist;
    }
}

namespace PerfTests {

    using mongo::dbtests::perfHist;

    const bool profiling = false;

    typedef DBDirectClient DBClientType;
    //typedef DBClientConnection DBClientType;

    class ClientBase {
    public:
        // NOTE: Not bothering to backup the old error record.
        ClientBase() {
            //_client.connect("localhost");
            mongo::lastError.reset( new LastError() );
        }
        virtual ~ClientBase() {
            //mongo::lastError.release();
        }
    protected:
        static void insert( const char *ns, BSONObj o ) {
            _client.insert( ns, o );
        }
        static void update( const char *ns, BSONObj q, BSONObj o, bool upsert = 0 ) {
            _client.update( ns, Query( q ), o, upsert );
        }
        static bool error() {
            return !_client.getPrevError().getField( "err" ).isNull();
        }
        DBClientBase &client() const { return _client; }
    private:
        static DBClientType _client;
    };
    DBClientType ClientBase::_client;

    // todo: use a couple threads. not a very good test yet.
#if 0
    class TaskQueueTest {
        static int tot;
        struct V {
            int val;
            static void go(const V &v) { tot += v.val; }
        };
    public:
        void run() {
            tot = 0;
            TaskQueue<V> d;
            int x = 0;
            for( int i = 0; i < 100; i++ ) {
                if( i % 30 == 0 )
                    d.invoke();

                x += i;
                writelock lk;
                V v;
                v.val = i;
                d.defer(v);
            }
            d.invoke();
            verify( x == tot );
        }
    };
    int TaskQueueTest::tot;
#endif

    /* if you want recording of the timings, place the password for the perf database
        in ./../settings.py:
            pstatspassword="<pwd>"
    */
    static boost::shared_ptr<DBClientConnection> conn;
    static string _perfhostname;
    void pstatsConnect() {
        // no writing to perf db if _DEBUG
        DEV return;

        const char *fn = "../../settings.py";
        if( !boost::filesystem::exists(fn) ) {
            if( boost::filesystem::exists("settings.py") )
                fn = "settings.py";
            else {
                cout << "no ../../settings.py or ./settings.py file found. will not write perf stats to pstats db." << endl;
                cout << "it is recommended this be enabled even on dev boxes" << endl;
                return;
            }
        }

        try {
            if( conn == 0 ) {
                MemoryMappedFile f;
                const char *p = (const char *) f.mapWithOptions(fn, MongoFile::READONLY);
                string pwd;

                {
                    const char *q = str::after(p, "pstatspassword=\"");
                    if( *q == 0 ) {
                        cout << "info perftests.cpp: no pstatspassword= in settings.py" << endl;
                        return;
                    }
                    else {
                        pwd = str::before(q, '\"');
                    }
                }

                boost::shared_ptr<DBClientConnection> c(new DBClientConnection(false, 0, 60));
                string err;
                if( c->connect("perfdb.10gen.cc", err) ) {
                    if( !c->auth("perf", "perf", pwd, err) ) {
                        cout << "info: authentication with stats db failed: " << err << endl;
                        verify(false);
                    }
                    conn = c;

                    // override the hostname with the buildbot hostname, if present
                    ifstream hostf( "../../info/host" );
                    if ( hostf.good() ) {
                        char buf[1024];
                        hostf.getline(buf, sizeof(buf));
                        _perfhostname = buf;
                    }
                    else {
                        _perfhostname = getHostName();
                    }
                }
                else {
                    cout << err << " (to log perfstats)" << endl;
                }
            }
        }
        catch(...) { 
            cout << "pstatsConnect() didn't work; ignoring" << endl;
        }
    }


    class B : public ClientBase {
        string _ns;
    protected:
        const char *ns() { return _ns.c_str(); }

        // anything you want to do before being timed
        virtual void prep() { }

        virtual void timed() = 0;

        // optional 2nd test phase to be timed separately
        // return name of it
        virtual string timed2(DBClientBase&) { return ""; }

        virtual void post() { }

        virtual string name() = 0;

        // how long to run test.  0 is a sentinel which means just run the timed() method once and time it.
        virtual int howLongMillis() { return profiling ? 30000 : 5000; }

        /* override if your test output doesn't need that */
        virtual bool showDurStats() { return true; }

    public:
        virtual unsigned batchSize() { return 50; }

        void say(unsigned long long n, int ms, string s) {
            unsigned long long rps = n*1000/ms;
            cout << "stats " << setw(42) << left << s << ' ' << right << setw(9) << rps << ' ' << right << setw(5) << ms << "ms ";
            if( showDurStats() )
                cout << dur::stats.curr->_asCSV();
            cout << endl;

            if( conn && !conn->isFailed() ) {
                const char *ns = "perf.pstats";
                if( perfHist ) {
                    static bool needver = true;
                    try {
                        // try to report rps from last time */
                        Query q;
                        {
                            BSONObjBuilder b;
                            b.append("host",_perfhostname).append("test",s).append("dur",cmdLine.dur);
                            DEV { b.append("info.DEBUG",true); }
                            else b.appendNull("info.DEBUG");
                            if( sizeof(int*) == 4 )
                                b.append("info.bits", 32);
                            else
                                b.appendNull("info.bits");
                            q = Query(b.obj()).sort("when",-1);
                        }
                        BSONObj fields = BSON( "rps" << 1 << "info" << 1 );
                        vector<BSONObj> v;
                        conn->findN(v, ns, q, perfHist, 0, &fields);
                        for( vector<BSONObj>::iterator i = v.begin(); i != v.end(); i++ ) {
                            BSONObj o = *i;
                            double lastrps = o["rps"].Number();
                            if( 0 && lastrps ) {
                                cout << "stats " << setw(42) << right << "new/old:" << ' ' << setw(9);
                                cout << fixed << setprecision(2) << rps / lastrps;
                                if( needver ) {
                                    cout << "         " << o.getFieldDotted("info.git").toString();
                                }
                                cout << '\n';
                            }
                        }
                    } catch(...) { }
                    cout.flush();
                    needver = false;
                }
                {
                    bob b;
                    b.append("host", _perfhostname);
                    b.appendTimeT("when", time(0));
                    b.append("test", s);
                    b.append("rps", (int) rps);
                    b.append("millis", ms);
                    b.appendBool("dur", cmdLine.dur);
                    if( showDurStats() && cmdLine.dur )
                        b.append("durStats", dur::stats.curr->_asObj());
                    {
                        bob inf;
                        inf.append("version", versionString);
                        if( sizeof(int*) == 4 ) inf.append("bits", 32);
                        DEV inf.append("DEBUG", true);
#if defined(_WIN32)
                        inf.append("os", "win");
#endif
                        inf.append("git", gitVersion());
                        inf.append("boost", BOOST_VERSION);
                        b.append("info", inf.obj());
                    }
                    BSONObj o = b.obj();
                    //cout << "inserting " << o.toString() << endl;
                    try {
                        conn->insert(ns, o);
                    }
                    catch ( std::exception& e ) {
                        warning() << "couldn't save perf results: " << e.what() << endl;
                    }
                }
            }
        }

        /** if true runs timed2() again with several threads (8 at time of this writing 
            shouldn't be just timed2, that is legacy and can be cleaned up one day
        */
        virtual bool testThreaded() { return false; }

        unsigned long long n;

        int howLong() { 
            int hlm = howLongMillis();
            DEV {
                // don't run very long with _DEBUG - not very meaningful anyway on that build
                hlm = min(hlm, 500);
            }
            return hlm;
        }

        void run() {
            _ns = string("perftest.") + name();
            client().dropCollection(ns());
            prep();
            int hlm = howLong();
            dur::stats._intervalMicros = 0; // no auto rotate
            dur::stats.curr->reset();
            mongo::Timer t;
            n = 0;
            const unsigned Batch = batchSize();

            if( hlm == 0 ) {
                // means just do once
                timed();
            }
            else {
                do {
                    unsigned i;
                    for( i = 0; i < Batch; i++ )
                        timed();
                    n += i;
                } while( t.micros() < (unsigned) hlm * 1000 );
            }

            client().getLastError(); // block until all ops are finished
            int ms = t.millis();

            say(n, ms, name());

            post();

            string test2name = timed2(client());
            {
                if( test2name.size() != 0 ) {
                    dur::stats.curr->reset();
                    mongo::Timer t;
                    unsigned long long n = 0;
                    while( 1 ) {
                        unsigned i;
                        for( i = 0; i < Batch; i++ )
                            timed2(client());
                        n += i;
                        if( t.millis() > hlm )
                            break;
                    }
                    int ms = t.millis();
                    say(n, ms, test2name);
                }
            }

            if( testThreaded() ) {
                const int nThreads = 8;
                //cout << "testThreaded nThreads:" << nThreads << endl;
                mongo::Timer t;
                launchThreads(nThreads);
                say(n, t.millis(), test2name+"-threaded");
            }
        }

        bool stop;

        void thread() {
#if defined(_WIN32)
            static int z;
            srand( ++z ^ (unsigned) time(0));
#endif
            DBClientType c;
            Client::initThreadIfNotAlready("perftestthr");
            while( 1 ) {
                for( int i = 0; i < 8; i++ )
                    timed2(c);
                if( stop ) 
                    break;
            }
            cc().shutdown();
        }

        void launchThreads(int remaining) {
            stop = false;
            if (!remaining) {
                int hlm = howLong();
                sleepmillis(hlm);
                stop = true;
                return;
            }
            boost::thread athread(boost::bind(&B::thread, this));
            launchThreads(remaining - 1);
            athread.join();
        }
    };

    unsigned dontOptimizeOutHopefully = 1;

    class NonDurTest : public B {
    public:
        virtual int howLongMillis() { return 3000; }
        virtual bool showDurStats() { return false; }
    };

    class BSONIter : public NonDurTest {
    public:
        int n;
        bo b, sub;
        string name() { return "BSONIter"; }
        BSONIter() {
            n = 0;
            bo sub = bob().appendTimeT("t", time(0)).appendBool("abool", true).appendBinData("somebin", 3, BinDataGeneral, "abc").appendNull("anullone").obj();
            b = BSON( "_id" << OID() << "x" << 3 << "yaaaaaa" << 3.00009 << "zz" << 1 << "q" << false << "obj" << sub << "zzzzzzz" << "a string a string" );
        }
        void timed() {
            for( bo::iterator i = b.begin(); i.more(); )
                if( i.next().fieldName() )
                    n++;
            for( bo::iterator i = sub.begin(); i.more(); )
                if( i.next().fieldName() )
                    n++;
        }
    };

    class BSONGetFields1 : public NonDurTest {
    public:
        int n;
        bo b, sub;
        string name() { return "BSONGetFields1By1"; }
        BSONGetFields1() {
            n = 0;
            bo sub = bob().appendTimeT("t", time(0)).appendBool("abool", true).appendBinData("somebin", 3, BinDataGeneral, "abc").appendNull("anullone").obj();
            b = BSON( "_id" << OID() << "x" << 3 << "yaaaaaa" << 3.00009 << "zz" << 1 << "q" << false << "obj" << sub << "zzzzzzz" << "a string a string" );
        }
        void timed() {
            if( b["x"].eoo() )
                n++;
            if( b["q"].eoo() )
                n++;
            if( b["zzz"].eoo() )
                n++;
        }
    };

    class BSONGetFields2 : public BSONGetFields1 {
    public:
        string name() { return "BSONGetFields"; }
        void timed() {
            static const char *names[] = { "x", "q", "zzz" };
            BSONElement elements[3];
            b.getFields(3, names, elements);
            if( elements[0].eoo() )
                n++;
            if( elements[1].eoo() )
                n++;
            if( elements[2].eoo() )
                n++;
        }
    };

    class KeyTest : public B {
    public:
        KeyV1Owned a,b,c;
        string name() { return "Key-woequal"; }
        virtual int howLongMillis() { return 3000; }
        KeyTest() :
          a(BSON("a"<<1<<"b"<<3.0<<"c"<<"qqq")),
          b(BSON("a"<<1<<"b"<<3.0<<"c"<<"qqq")),
          c(BSON("a"<<1<<"b"<<3.0<<"c"<<"qqqb"))
          {}
        virtual bool showDurStats() { return false; }
        void timed() {
            verify( a.woEqual(b) );
            verify( !a.woEqual(c) );
        }
    };

    unsigned long long aaa;

    class Timer : public B {
    public:
        string name() { return "Timer"; }
        virtual int howLongMillis() { return 1000; }
        virtual bool showDurStats() { return false; }
        void timed() {
            mongo::Timer t;
            aaa += t.millis();
        }
    };

    class Sleep0Ms : public B {
    public:
        string name() { return "Sleep0Ms"; }
        virtual int howLongMillis() { return 400; }
        virtual bool showDurStats() { return false; }
        void timed() {
            sleepmillis(0);
            aaa++;
        }
    };

#if defined(__USE_XOPEN2K)
    class Yield : public B {
    public:
        string name() { return "Yield"; }
        virtual int howLongMillis() { return 400; }
        virtual bool showDurStats() { return false; }
        void timed() {
            pthread_yield();
            aaa++;
        }
    };
#endif

    RWLock lk("testrw");
    SimpleMutex m("simptst");
    mongo::mutex mtest("mtest");
    SpinLock s;
    boost::condition c;

    class NotifyOne : public B {
    public:
        string name() { return "notify_one"; }
        virtual int howLongMillis() { return 500; }
        virtual bool showDurStats() { return false; }
        void timed() {
            c.notify_one();
        }
    };
    class mutexspeed : public B {
    public:
        string name() { return "mutex"; }
        virtual int howLongMillis() { return 500; }
        virtual bool showDurStats() { return false; }
        void timed() {
            mongo::mutex::scoped_lock lk(mtest);
        }
    };
    class simplemutexspeed : public B {
    public:
        string name() { return "simplemutex"; }
        virtual int howLongMillis() { return 500; }
        virtual bool showDurStats() { return false; }
        void timed() {
            SimpleMutex::scoped_lock lk(m);
        }
    };
    class spinlockspeed : public B {
    public:
        string name() { return "spinlock"; }
        virtual int howLongMillis() { return 500; }
        virtual bool showDurStats() { return false; }
        void timed() {
            mongo::scoped_spinlock lk(s);
        }
    };
    int cas;
    class casspeed : public B {
    public:
        string name() { return "compareandswap"; }
        virtual int howLongMillis() { return 500; }
        virtual bool showDurStats() { return false; }
        void timed() {
#ifdef __GCC_HAVE_SYNC_COMPARE_AND_SWAP_4
#define RUNCOMPARESWAP 1
            __sync_bool_compare_and_swap(&cas, 0, 0);
#endif
        }
    };
    class rlock : public B {
    public:
        string name() { return "rlock"; }
        virtual int howLongMillis() { return 500; }
        virtual bool showDurStats() { return false; }
        void timed() {
            lk.lock_shared();
            lk.unlock_shared();
        }
    };
    class wlock : public B {
    public:
        string name() { return "wlock"; }
        virtual int howLongMillis() { return 500; }
        virtual bool showDurStats() { return false; }
        void timed() {
            lk.lock();
            lk.unlock();
        }
    };

    QLock _qlock;

    class qlock : public B {
    public:
        string name() { return "qlockr"; }
        //virtual int howLongMillis() { return 500; }
        virtual bool showDurStats() { return false; }
        void timed() {
            _qlock.lock_r();
            _qlock.unlock_r();
        }
    };
    class qlockw : public B {
    public:
        string name() { return "qlockw"; }
        //virtual int howLongMillis() { return 500; }
        virtual bool showDurStats() { return false; }
        void timed() {
            _qlock.lock_w();
            _qlock.unlock_w();
        }
    };

#if 0
    class ulock : public B {
    public:
        string name() { return "ulock"; }
        virtual int howLongMillis() { return 500; }
        virtual bool showDurStats() { return false; }
        void timed() {
            lk.lockAsUpgradable();
            lk.unlockFromUpgradable();
        }
    };
#endif

    class CTM : public B {
    public:
        CTM() : last(0), delts(0), n(0) { }
        string name() { return "curTimeMillis64"; }
        virtual int howLongMillis() { return 500; }
        virtual bool showDurStats() { return false; }
        unsigned long long last;
        unsigned long long delts;
        unsigned n;
        void timed() {
            unsigned long long x = curTimeMillis64();
            aaa += x;
            if( last ) {
                unsigned long long delt = x-last;
                if( delt ) {
                    delts += delt;
                    n++;
                }
            }
            last = x;
        }
        void post() {
            // we need to know if timing is highly ungranular - that could be relevant in some places
            if( n )
                cout << "      avg timer granularity: " << ((double)delts)/n << "ms " << endl;
        }
    };
    class CTMicros : public B {
    public:
        CTMicros() : last(0), delts(0), n(0) { }
        string name() { return "curTimeMicros64"; }
        virtual int howLongMillis() { return 500; }
        virtual bool showDurStats() { return false; }
        unsigned long long last;
        unsigned long long delts;
        unsigned n;
        void timed() {
            unsigned long long x = curTimeMicros64();
            aaa += x;
            if( last ) {
                unsigned long long delt = x-last;
                if( delt ) {
                    delts += delt;
                    n++;
                }
            }
            last = x;
        }
        void post() {
            // we need to know if timing is highly ungranular - that could be relevant in some places
            if( n )
                cout << "      avg timer granularity: " << ((double)delts)/n << "ms " << endl;
        }
    };

    class Bldr : public B {
    public:
        int n;
        string name() { return "BufBuilder"; }
        Bldr() {
        }
        virtual int howLongMillis() { return 3000; }
        virtual bool showDurStats() { return false; }
        void timed() {
            BufBuilder b;
            b.appendNum(3);
            b.appendUChar(' ');
            b.appendStr("abcd");
            n += b.len();
        }
    };

    class StkBldr : public B {
    public:
        virtual int howLongMillis() { return 3000; }
        int n;
        string name() { return "StackBufBuilder"; }
        virtual bool showDurStats() { return false; }
        void timed() {
            StackBufBuilder b;
            b.appendNum(3);
            b.appendUChar(' ');
            b.appendStr("abcd");
            n += b.len();
        }
    };

    // if a test is this fast, it was optimized out
    class Dummy : public B {
    public:
        Dummy() { }
        virtual int howLongMillis() { return 3000; }
        string name() { return "dummy"; }
        void timed() {
            dontOptimizeOutHopefully++;
        }
        virtual bool showDurStats() { return false; }
    };

    // test thread local speed
#if defined(_WIN32)
    __declspec( thread ) int x;
    class TLS2 : public B {
    public:
        virtual int howLongMillis() { return 3000; }
        string name() { return "thread-local-storage2"; }
        void timed() {
            if( x )
                dontOptimizeOutHopefully++;
        }
        virtual bool showDurStats() { return false; }
    };
#endif

    // test thread local speed
    class TLS : public B {
    public:
        virtual int howLongMillis() { return 3000; }
        string name() { return "thread-local-storage"; }
        void timed() {
            if( &cc() )
                dontOptimizeOutHopefully++;
        }
        virtual bool showDurStats() { return false; }
    };

    bool dummy1 = false;

    class TestException : public DBException { 
    public:
        TestException() : DBException("testexception",3) { }
    };
    struct Z { 
        Z() {  dontOptimizeOutHopefully--; }
        ~Z() { dontOptimizeOutHopefully++; }
    };
    void thr1(int n) { 
        if( dontOptimizeOutHopefully ) { 
            throw TestException();
        }
        log() << "hmmm" << endl;
    }
    void thr2(int n) { 
        if( --n <= 0 ) {
            if( dontOptimizeOutHopefully ) { 
                throw TestException();
            }
            log() << "hmmm" << endl;
        }
        Z z;
        try { 
            thr2(n-1);
        }
        catch(DBException&) { 
        }
    }
    void thr3(int n) { 
        if( --n <= 0 ) {
            if( dontOptimizeOutHopefully ) { 
                throw TestException();
            }
            log() << "hmmm" << endl;
        }
        try { 
            Z z;
            thr3(n-1);
        }
        catch(DBException&) { 
        }
    }
    void thr4(int n) { 
        if( --n <= 0 ) {
            if( dontOptimizeOutHopefully ) { 
                throw TestException();
            }
            log() << "hmmm" << endl;
        }
        Z z;
        thr4(n-1);
    }
    template< void T (int) >
    class Throw : public B {
    public:
        virtual int howLongMillis() { return 2000; }
        string name() { return "throw"; }
        void timed() {
            try { 
                T(10);
                dontOptimizeOutHopefully += 2;
            }
            catch(DBException& e) {
                e.getCode();
                dontOptimizeOutHopefully++;
            }
        }
        virtual bool showDurStats() { return false; }
    };

    class New128 : public B {
    public:
        virtual int howLongMillis() { return 2000; }
        string name() { return "new128"; }
        void timed() {
            char *p = new char[128];
            if( dontOptimizeOutHopefully++ > 0 )
                delete[] p;
        }
        virtual bool showDurStats() { return false; }
    };

    class New8 : public B {
    public:
        virtual int howLongMillis() { return 2000; }
        string name() { return "new8"; }
        void timed() {
            char *p = new char[8];
            if( dontOptimizeOutHopefully++ > 0 )
                delete[] p;
        }
        virtual bool showDurStats() { return false; }
    };

    class Compress : public B {
    public:
        const unsigned sz;
        void *p;
        Compress() : sz(1024*1024*100+3) { }
        virtual unsigned batchSize() { return 1; }
        string name() { return "compress"; }
        virtual bool showDurStats() { return false; }
        virtual int howLongMillis() { return 4000; }
        void prep() {
            p = malloc(sz);
            // this isn't a fair test as it is mostly rands but we just want a rough perf check
            static int last;
            for (unsigned i = 0; i<sz; i++) {
                int r = rand();
                if( (r & 0x300) == 0x300 )
                    r = last;
                ((char*)p)[i] = r;
                last = r;
            }
        }
        size_t last;
        string res;
        void timed() {
            mongo::Timer t;
            string out;
            size_t len = compress((const char *) p, sz, &out);
            bool ok = uncompress(out.c_str(), out.size(), &res);
            ASSERT(ok);
            static unsigned once;
            if( once++ == 0 )
                cout << "compress round trip " << sz/(1024.0*1024) / (t.millis()/1000.0) << "MB/sec\n";
            //cout << len / (1024.0/1024) << " compressed" << endl;
            (void)len; //fix unused error while above line is commented out
        }
        void post() {
            ASSERT( memcmp(res.c_str(), p, sz) == 0 );
            free(p);
        }
    };

    // test speed of checksum method
    class ChecksumTest : public B {
    public:
        const unsigned sz;
        ChecksumTest() : sz(1024*1024*100+3) { }
        string name() { return "checksum"; }
        virtual int howLongMillis() { return 2000; }
        virtual bool showDurStats() { return false; }
        virtual unsigned batchSize() { return 1; }

        void *p;

        void prep() {
            {
                // the checksum code assumes 'standard' rollover on addition overflows. let's check that:
                little<unsigned long long> x = 0xffffffffffffffffULL;
                ASSERT( x+2 == 1 );
            }

            p = malloc(sz);
            for (unsigned i = 0; i<sz; i++)
                ((char*)p)[i] = rand();
        }

        Checksum last;

        void timed() {
            static int i;
            Checksum c;
            c.gen(p, sz);
            if( i == 0 )
                last = c;
            else if( i == 1 ) {
                ASSERT( c == last );
            }
        }
        void post() {
            {
                mongo::Checksum c;
                c.gen(p, sz-1);
                ASSERT( c != last );
                ((char *&)p)[0]++; // check same data, different order, doesn't give same checksum
                ((char *&)p)[1]--;
                c.gen(p, sz);
                ASSERT( c != last );
                ((char *&)p)[1]++; // check same data, different order, doesn't give same checksum (different longwords case)
                ((char *&)p)[8]--;
                c.gen(p, sz);
                ASSERT( c != last );
            }
            free(p);
        }
    };

    class InsertDup : public B {
        const BSONObj o;
    public:
        InsertDup() : o( BSON("_id" << 1) ) { } // dup keys
        string name() {
            return "insert-duplicate-_ids";
        }
        void prep() {
            client().insert( ns(), o );
        }
        void timed() {
            client().insert( ns(), o );
        }
        void post() {
            verify( client().count(ns()) == 1 );
        }
    };

    class Insert1 : public B {
        const BSONObj x;
        OID oid;
        BSONObj query;
    public:
        virtual int howLongMillis() { return profiling ? 30000 : 5000; }
        Insert1() : x( BSON("x" << 99) ) {
            oid.init();
            query = BSON("_id" << oid);
            i = 0;
        }
        string name() { return "insert-simple"; }
        unsigned i;
        void timed() {
            BSONObj o = BSON( "_id" << i++ << "x" << 99 );
            client().insert( ns(), o );
        }
        virtual bool testThreaded() { 
            if( profiling ) 
                return false;
            return true; 
        }
        string timed2(DBClientBase& c) {
            Query q = QUERY( "_id" << (unsigned) (rand() % i) );
            c.findOne(ns(), q);
            return "findOne_by_id";
        }
        void post() {
#if !defined(_DEBUG)
            verify( client().count(ns()) > 50 );
#endif
        }
    };

    class InsertBig : public B {
        BSONObj x;
        virtual int howLongMillis() {
            if( sizeof(void*) == 4 )
                return 1000;  // could exceed mmapping if run too long, as this function adds a lot fasta
            return 5000;
        }
    public:
        InsertBig() {
            char buf[200000];
            BSONObjBuilder b;
            b.append("x", 99);
            b.appendBinData("bin", 200000, (BinDataType) 129, buf);
            x = b.obj();
        }
        string name() { return "insert-big"; }
        void timed() {
            client().insert( ns(), x );
        }
    };

    class InsertRandom : public B {
    public:
        virtual int howLongMillis() { return profiling ? 30000 : 5000; }
        string name() { return "random-inserts"; }
        void prep() {
            client().insert( ns(), BSONObj() );
            client().ensureIndex(ns(), BSON("x"<<1));
        }
        void timed() {
            int x = rand();
            BSONObj y = BSON("x" << x << "y" << rand() << "z" << 33);
            client().insert(ns(), y);
        }
    };

    /** upserts about 32k records and then keeps updating them
        2 indexes
    */
    class Update1 : public B {
    public:
        static int rand() {
            return std::rand() & 0x7fff;
        }
        virtual string name() { return "random-upserts"; }
        void prep() {
            client().insert( ns(), BSONObj() );
            client().ensureIndex(ns(), BSON("x"<<1));
        }
        void timed() {
            int x = rand();
            BSONObj q = BSON("x" << x);
            BSONObj y = BSON("x" << x << "y" << rand() << "z" << 33);
            client().update(ns(), q, y, /*upsert*/true);
        }
        virtual bool testThreaded() { return true; }
        virtual string timed2(DBClientBase& c) {
            static BSONObj I = BSON( "$inc" << BSON( "y" << 1 ) );
            // test some $inc's
            int x = rand();
            BSONObj q = BSON("x" << x);
            c.update(ns(), q, I);
            return name()+"-inc";
        }
    };

    template <typename T>
    class MoreIndexes : public T {
    public:
        string name() { return T::name() + "-more-indexes"; }
        void prep() {
            T::prep();
            this->client().ensureIndex(this->ns(), BSON("y"<<1));
            this->client().ensureIndex(this->ns(), BSON("z"<<1));
        }
    };

    void t() {
        for( int i = 0; i < 20; i++ ) {
            sleepmillis(21);
            string fn = "/tmp/t1";
            MongoMMF f;
            unsigned long long len = 1 * 1024 * 1024;
            verify( f.create(fn, len, /*sequential*/rand()%2==0) );
            {
                char *p = (char *) f.getView();
                verify(p);
                // write something to the private view as a test
                strcpy(p, "hello");
            }
            if( cmdLine.dur ) {
                char *w = (char *) f.view_write();
                strcpy(w + 6, "world");
            }
            MongoFileFinder ff;
            ASSERT( ff.findByPath(fn) );
        }
    }

    class All : public Suite {
    public:
        All() : Suite( "perf" ) { }

        Result * run( const string& filter ) {
            boost::thread a(t);
            Result * res = Suite::run(filter);
            a.join();
            return res;
        }

        void setupTests() {
            pstatsConnect();
            cout
                << "stats test                                       rps------  time-- "
                << dur::stats.curr->_CSVHeader() << endl;
            if( profiling ) {
                add< Insert1 >();
            }
            else {
                add< Dummy >();
                add< ChecksumTest >();
                add< Compress >();
                add< TLS >();
#if defined(_WIN32)
                add< TLS2 >();
#endif
                add< New8 >();
                add< New128 >();
                add< Throw< thr1 > >();
                add< Throw< thr2 > >();
                add< Throw< thr3 > >();
                add< Throw< thr4 > >();
                add< Timer >();
                add< Sleep0Ms >();
#if defined(__USE_XOPEN2K)
                add< Yield >();
#endif
                add< rlock >();
                add< wlock >();
                add< qlock >();
                add< qlockw >();
                add< NotifyOne >();
                add< mutexspeed >();
                add< simplemutexspeed >();
                add< spinlockspeed >();
#ifdef RUNCOMPARESWAP
                add< casspeed >();
#endif
                add< CTM >();
                add< CTMicros >();
                add< KeyTest >();
                add< Bldr >();
                add< StkBldr >();
                add< BSONIter >();
                add< BSONGetFields1 >();
                add< BSONGetFields2 >();
                //add< TaskQueueTest >();
                add< InsertDup >();
                add< Insert1 >();
                add< InsertRandom >();
                add< MoreIndexes<InsertRandom> >();
                add< Update1 >();
                add< MoreIndexes<Update1> >();
                add< InsertBig >();
            }
        }
    } myall;
}
