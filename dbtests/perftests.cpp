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
#include "../db/ops/query.h"
#include "../db/db.h"
#include "../db/instance.h"
#include "../db/json.h"
#include "../db/lasterror.h"
#include "../db/ops/update.h"
#include "../db/taskqueue.h"
#include "../util/timer.h"
#include "dbtests.h"
#include "../db/dur_stats.h"
#include "../util/checksum.h"
#include "../util/version.h"
#include "../db/key.h"

using namespace bson;

namespace mongo {
    namespace regression {
        extern unsigned perfHist;
    }
}

namespace PerfTests {
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
            assert( x == tot );
        }
    };
    int TaskQueueTest::tot;

    class B : public ClientBase {
        string _ns;
    protected:
        const char *ns() { return _ns.c_str(); }

        // anything you want to do before being timed
        virtual void prep() { }

        virtual void timed() = 0;

        // optional 2nd test phase to be timed separately
        // return name of it
        virtual const char * timed2() { return 0; }

        virtual void post() { }

        virtual string name() = 0;
        virtual unsigned long long expectation() { return 0; }
        virtual int expectationTimeMillis() { return -1; }

        // how long to run test.  0 is a sentinel which means just run the timed() method once and time it.
        virtual int howLongMillis() { return 5000; } 

        /* override if your test output doesn't need that */
        virtual bool showDurStats() { return true; }

        static DBClientConnection *conn;

    public:
        void say(unsigned long long n, int ms, string s) {
            unsigned long long rps = n*1000/ms;
            cout << "stats " << setw(33) << left << s << ' ' << right << setw(9) << rps << ' ' << right << setw(5) << ms << "ms ";
            if( showDurStats() )
                cout << dur::stats.curr->_asCSV();
            cout << endl;

            /* if you want recording of the timings, place the password for the perf database 
               in ./../settings.py:
                 pstatspassword="<pwd>"
            */
            const char *fn = "../../settings.py";
            static bool ok = true;
            if( ok ) {
                DEV { 
                    // no writing to perf db if dev
                }
                else if( !exists(fn) ) { 
                    cout << "no ../../settings.py file found. will not write perf stats to db" << endl;
                }
                else {
                    try {
                        if( conn == 0 ) {
                            MemoryMappedFile f;
                            const char *p = (const char *) f.mapWithOptions(fn, MongoFile::READONLY);
                            string pwd;

                            {
                                const char *q = str::after(p, "pstatspassword=\"");
                                if( *q == 0 ) {
                                    cout << "info perftests.cpp: no pstatspassword= in settings.py" << endl;
                                    ok = false;
                                }
                                else {
                                    pwd = str::before(q, '\"');
                                }
                            }

                            if( ok ) {
                                conn = new DBClientConnection(false, 0, 10);
                                string err;
                                if( conn->connect("mongo05.10gen.cust.cbici.net", err) ) { 
                                    if( !conn->auth("perf", "perf", pwd, err) ) { 
                                        cout << "info: authentication with stats db failed: " << err << endl;
                                        assert(false);
                                    }
                                }
                                else { 
                                    cout << err << " (to log perfstats)" << endl;
                                    ok = false;
                                }
                            }
                        }
                        if( conn && !conn->isFailed() ) { 
                            const char *ns = "perf.pstats";
                            if( perfHist ) {
                                static bool needver = true;
                                try {
                                    // try to report rps from last time */
                                    Query q;
                                    {
                                        BSONObjBuilder b;
                                        b.append("host",getHostName()).append("test",s).append("dur",cmdLine.dur);
                                        DEV b.append("info.DEBUG",true);
                                        else b.appendNull("info.DEBUG");
                                        if( sizeof(int*) == 4 ) b.append("info.bits", 32);
                                        else b.appendNull("info.bits");
                                        q = Query(b.obj()).sort("when",-1);
                                   }
                                    //cout << q.toString() << endl;
                                    BSONObj fields = BSON( "rps" << 1 << "info" << 1 );
                                    vector<BSONObj> v;
                                    conn->findN(v, ns, q, perfHist, 0, &fields);
                                    for( vector<BSONObj>::iterator i = v.begin(); i != v.end(); i++ ) {
                                        BSONObj o = *i;
                                        double lastrps = o["rps"].Number();
                                        if( lastrps ) {
                                            cout << "stats " << setw(33) << right << "new/old:" << ' ' << setw(9);
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
                                b.append("host", getHostName());
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
                                conn->insert(ns, o);
                            }
                        }
                    }
                    catch(...) { 
                    }
                }
            }
        }
        void run() {
            _ns = string("perftest.") + name();
            client().dropCollection(ns());

            prep();

            int hlm = howLongMillis();
            DEV { 
                // don't run very long with _DEBUG - not very meaningful anyway on that build
                hlm = min(hlm, 500);
            }

            dur::stats._intervalMicros = 0; // no auto rotate
            dur::stats.curr->reset();
            Timer t;
            unsigned long long n = 0;
            const unsigned Batch = 50;

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
                } while( t.millis() < hlm );
            }

            client().getLastError(); // block until all ops are finished
            int ms = t.millis();

            say(n, ms, name());

            int etm = expectationTimeMillis();
            DEV { 
            }
            else if( etm > 0 ) { 
                if( ms > etm*2 ) { 
                    cout << "test  " << name() << " seems slow expected ~" << etm << "ms" << endl;
                }
            }
            else if( n < expectation() ) {
                cout << "test  " << name() << " seems slow n:" << n << " ops/sec but expect greater than:" << expectation() << endl;
            }

            post();

            {
                const char *test2name = timed2();
                if( test2name ) {
                    dur::stats.curr->reset();
                    Timer t;
                    unsigned long long n = 0;
                    while( 1 ) {
                        unsigned i;
                        for( i = 0; i < Batch; i++ )
                            timed2();
                        n += i;
                        if( t.millis() > hlm )
                            break;
                    }
                    int ms = t.millis();
                    say(n, ms, test2name);
                }
            }
        }
    };

    DBClientConnection *B::conn;

    unsigned dontOptimizeOutHopefully;

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
            assert( a.woEqual(b) );
            assert( !a.woEqual(c) );
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
        virtual int howLongMillis() { return 4000; } 
        string name() { return "dummy"; }
        void timed() {
            dontOptimizeOutHopefully++;
        }
        unsigned long long expectation() { return 1000000; }
        virtual bool showDurStats() { return false; }
    };

    // test thread local speed
    class TLS : public B {
    public:
        TLS() { }
        virtual int howLongMillis() { return 4000; } 
        string name() { return "thread-local-storage"; }
        void timed() {
            if( &cc() )
                dontOptimizeOutHopefully++;
        }
        unsigned long long expectation() { return 1000000; }
        virtual bool showDurStats() { return false; }
    };

    class Malloc : public B {
    public:
        Malloc() { }
        virtual int howLongMillis() { return 4000; } 
        string name() { return "malloc"; }
        void timed() {
            char *p = new char[128];
            if( dontOptimizeOutHopefully++ > 0 )
                delete p;
        }
        unsigned long long expectation() { return 1000000; }
        virtual bool showDurStats() { return false; }
    };

    // test speed of checksum method
    class ChecksumTest : public B {
    public:
        const unsigned sz;
        ChecksumTest() : sz(1024*1024*100+3) { }
        string name() { return "checksum"; }
        virtual int howLongMillis() { return 2000; } 
        int expectationTimeMillis() { return 5000; }
        virtual bool showDurStats() { return false; }

        void *p;

        void prep() { 
            {
                // the checksum code assumes 'standard' rollover on addition overflows. let's check that:
                unsigned long long x = 0xffffffffffffffffULL;
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
            assert( client().count(ns()) == 1 );
        }
        unsigned long long expectation() { return 1000; }
    };

    class Insert1 : public B {
        const BSONObj x;
        OID oid;
        BSONObj query;
    public:
        Insert1() : x( BSON("x" << 99) ) { 
            oid.init();
            query = BSON("_id" << oid);
        }
        string name() { return "insert-simple"; }
        void timed() {
            client().insert( ns(), x );
        }
        const char * timed2() {
            client().findOne(ns(), query);
            return "findOne_by_id";
        }
        void post() {
            assert( client().count(ns()) > 50 );
        }
        unsigned long long expectation() { return 1000; }
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
        unsigned long long expectation() { return 20; }
    };

    class InsertRandom : public B {
    public:
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
        unsigned long long expectation() { return 1000; }
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

        const char * timed2() {
            static BSONObj I = BSON( "$inc" << BSON( "y" << 1 ) );

            // test some $inc's

            int x = rand();
            BSONObj q = BSON("x" << x);
            client().update(ns(), q, I);

            static string s = name()+"-inc";
            return s.c_str();
        }

        void post() {
        }
        unsigned long long expectation() { return 1000; }
    };

    template <typename T>
    class MoreIndexes : public T {
    public:
        string name() { return T::name() + "-with-more-indexes"; }
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
            assert( f.create(fn, len, /*sequential*/rand()%2==0) );
            {
                char *p = (char *) f.getView();
                assert(p);
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
        All() : Suite( "perf" )
        {
        }
        ~All() { 
        }
        Result * run( const string& filter ) { 
            boost::thread a(t);
            Result * res = Suite::run(filter); 
            a.join();
            return res;
        }

        void setupTests() {
            cout
                << "stats test                              rps------  time-- "
                << dur::stats.curr->_CSVHeader() << endl;
            add< Dummy >();
            add< TLS >();
            add< Malloc >();
            add< Timer >();
            add< CTM >();
            add< KeyTest >();
            add< Bldr >();
            add< StkBldr >();
            add< BSONIter >();
            add< BSONGetFields1 >();
            add< BSONGetFields2 >();
            add< ChecksumTest >();
            add< TaskQueueTest >();
            add< InsertDup >();
            add< Insert1 >();
            add< InsertRandom >();
            add< MoreIndexes<InsertRandom> >();
            add< Update1 >();
            add< MoreIndexes<Update1> >();
            add< InsertBig >();
        }
    } myall;
}
