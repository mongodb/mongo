// @file threadedtests.cpp - Tests for threaded code
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

#include "pch.h"
#include "../server.h"
#include "../bson/util/atomic_int.h"
#include "../util/concurrency/mvar.h"
#include "../util/concurrency/thread_pool.h"
#include "../util/concurrency/list.h"
#include "../util/timer.h"
#include <boost/thread.hpp>
#include <boost/bind.hpp>
#include "../db/d_concurrency.h"
#include "../util/concurrency/synchronization.h"
#include "../util/concurrency/qlock.h"
#include "dbtests.h"
#include "mongo/util/concurrency/ticketholder.h"

namespace mongo { 
    void testNonGreedy();
}

namespace ThreadedTests {

    template <int nthreads_param=10>
    class ThreadedTest {
    public:
        virtual void setup() {} //optional
        virtual void subthread(int remaining) = 0; // each thread whatever test work you want done
        virtual void validate() = 0; // after work is done

        static const int nthreads = nthreads_param;

        void run() {
            setup();
            launch_subthreads(nthreads);
            validate();
        }

        virtual ~ThreadedTest() {}; // not necessary, but makes compilers happy

    private:
        void launch_subthreads(int remaining) {
            if (!remaining) 
                return;

            boost::thread athread(boost::bind(&ThreadedTest::subthread, this, remaining));
            launch_subthreads(remaining - 1);
            athread.join();
        }
    };

    const int nthr=135;
    //const int nthr=7;
    class MongoMutexTest : public ThreadedTest<nthr> {
#if defined(_DEBUG)
        enum { N = 2000 };
#else
        enum { N = 4000/*0*/ };
#endif
        ProgressMeter pm;
    public:
        int upgradeWorked, upgradeFailed;
        MongoMutexTest() : pm(N * nthreads) {
            upgradeWorked = upgradeFailed = 0;
        }
        void run() {
            DEV {
                // in _DEBUG builds on linux we mprotect each time a writelock
                // is taken. That can greatly slow down this test if there are
                // many open files
                DBDirectClient db;
                db.simpleCommand("admin", NULL, "closeAllDatabases");
            }

            Timer t;
            cout << "MongoMutexTest N:" << N << endl;
            ThreadedTest<nthr>::run();
            cout << "MongoMutexTest " << t.millis() << "ms" << endl;
        }
    private:
        virtual void setup() {
        }
        virtual void subthread(int tnumber) {
            Client::initThread("mongomutextest");
            sleepmillis(0);
            for( int i = 0; i < N; i++ ) {
                int x = std::rand();
                bool sometimes = (x % 15 == 0);
                if( i % 7 == 0 ) {
                    Lock::GlobalRead r; // nested test
                    Lock::GlobalRead r2;
                    if( sometimes ) {
                        Lock::TempRelease t;
                    }
                }
                else if( i % 7 == 1 ) {
                    Lock::GlobalRead r;
                    ASSERT( Lock::isReadLocked() );
                    ASSERT( Lock::isLocked() );
                    if( sometimes ) {
                        Lock::TempRelease t;
                    }
                }
                else if( i % 7 == 4 && 
                         tnumber == 1 /*only one upgrader legal*/ ) {
                    Lock::GlobalWrite w;
                    ASSERT( Lock::isW() );
                    ASSERT( Lock::isW() );
                    if( i % 7 == 2 ) {
                        Lock::TempRelease t;
                    }
                    if( sometimes ) { 
                        w.downgrade();
                        sleepmillis(0);
                        bool worked = w.upgrade();
                        if( worked) upgradeWorked++;
                        else upgradeFailed++;
                    }
                }
                else if( i % 7 == 2 ) {
                    Lock::GlobalWrite w;
                    ASSERT( Lock::isW() );
                    ASSERT( Lock::isW() );
                    if( sometimes ) {
                        Lock::TempRelease t;
                    }
                }
                else if( i % 7 == 3 ) {
                    Lock::GlobalWrite w;
                    {
                        Lock::TempRelease t;
                    }
                    Lock::GlobalRead r;
                    ASSERT( Lock::isW() );
                    ASSERT( Lock::isW() );
                    if( sometimes ) {
                        Lock::TempRelease t;
                    }
                }
                else if( i % 7 == 5 ) {
                    {
                        Lock::DBRead r("foo");
                        if( sometimes ) {
                            Lock::TempRelease t;
                        }
                    }
                    {
                        Lock::DBRead r("bar");
                    }
                }
                else if( i % 7 == 6 ) {
                    if( i > N/2 ) { 
                        int q = i % 11;
                        if( q == 0 ) { 
                            char what = Lock::dbLevelLockingEnabled() ? 'r' : 'R';
                            Lock::DBRead r("foo");
                            ASSERT( Lock::isLocked() == what && Lock::atLeastReadLocked("foo") );
                            ASSERT( !Lock::nested() );
                            Lock::DBRead r2("foo");
                            ASSERT( Lock::nested() );
                            ASSERT( Lock::isLocked() == what && Lock::atLeastReadLocked("foo") );
                            Lock::DBRead r3("local");
                            if( sometimes ) {
                                Lock::TempRelease t;
                            }
                            ASSERT( Lock::isLocked() == what && Lock::atLeastReadLocked("foo") );
                            ASSERT( Lock::isLocked() == what && Lock::atLeastReadLocked("local") );
                        }
                        else if( q == 1 ) {
                            // test locking local only -- with no preceeding lock
                            { 
                                Lock::DBRead x("local"); 
                                //Lock::DBRead y("q");
                                if( sometimes ) {
                                    Lock::TempRelease t; // we don't temprelease (cant=true) here thus this is just a check that nothing weird happens...
                                }
                            }
                            { 
                                Lock::DBWrite x("local"); 
                                if( sometimes ) {
                                    Lock::TempRelease t;
                                }
                            }
                        } else if( q == 1 ) {
                            { Lock::DBRead  x("admin"); }
                            { Lock::DBWrite x("admin"); }
                        } else if( q == 2 ) { 
                            /*Lock::DBWrite x("foo");
                            Lock::DBWrite y("admin");
                            { Lock::TempRelease t; }*/
                        }
                        else if( q == 3 ) {
                            Lock::DBWrite x("foo");
                            Lock::DBRead y("admin");
                            { Lock::TempRelease t; }
                        } 
                        else if( q == 4 ) { 
                            Lock::DBRead x("foo2");
                            Lock::DBRead y("admin");
                            { Lock::TempRelease t; }
                        }
                        else { 
                            Lock::DBWrite w("foo");
                            {
                                Lock::TempRelease t;
                            }
                            Lock::DBRead r2("foo");
                            Lock::DBRead r3("local");
                            if( sometimes ) {
                                Lock::TempRelease t;
                            }
                        }
                    }
                    else { 
                        Lock::DBRead r("foo");
                        Lock::DBRead r2("foo");
                        Lock::DBRead r3("local");
                    }
                }
                pm.hit();
            }
            cc().shutdown();
        }
        virtual void validate() {
            log() << "mongomutextest validate" << endl;
            ASSERT( ! Lock::isReadLocked() );
            ASSERT( upgradeWorked > upgradeFailed );
            ASSERT( upgradeWorked > 4 );
            {
                    Lock::GlobalWrite w;
            }
            {
                    Lock::GlobalRead r;
            }
        }
    };

    // Tested with up to 30k threads
    class IsAtomicUIntAtomic : public ThreadedTest<> {
        static const int iterations = 1000000;
        AtomicUInt target;

        void subthread(int) {
            for(int i=0; i < iterations; i++) {
                //target.x++; // verified to fail with this version
                target++;
            }
        }
        void validate() {
            ASSERT_EQUALS(target.x , unsigned(nthreads * iterations));

            AtomicUInt u;
            ASSERT_EQUALS(0u, u);
            ASSERT_EQUALS(0u, u++);
            ASSERT_EQUALS(2u, ++u);
            ASSERT_EQUALS(2u, u--);
            ASSERT_EQUALS(0u, --u);
            ASSERT_EQUALS(0u, u);
            
            u++;
            ASSERT( u > 0 );

            u--;
            ASSERT( ! ( u > 0 ) );
        }
    };

    class MVarTest : public ThreadedTest<> {
        static const int iterations = 10000;
        MVar<int> target;

    public:
        MVarTest() : target(0) {}
        void subthread(int) {
            for(int i=0; i < iterations; i++) {
                int val = target.take();
#if BOOST_VERSION >= 103500
                //increase chances of catching failure
                boost::this_thread::yield();
#endif
                target.put(val+1);
            }
        }
        void validate() {
            ASSERT_EQUALS(target.take() , nthreads * iterations);
        }
    };

    class ThreadPoolTest {
        static const int iterations = 10000;
        static const int nThreads = 8;

        AtomicUInt counter;
        void increment(int n) {
            for (int i=0; i<n; i++) {
                counter++;
            }
        }

    public:
        void run() {
            ThreadPool tp(nThreads);

            for (int i=0; i < iterations; i++) {
                tp.schedule(&ThreadPoolTest::increment, this, 2);
            }

            tp.join();

            ASSERT(counter == (unsigned)(iterations * 2));
        }
    };

    class LockTest {
    public:
        void run() {
            // quick atomicint wrap test
            // MSGID likely assumes this semantic
            AtomicUInt counter = 0xffffffff;
            counter++;
            ASSERT( counter == 0 );

            writelocktry lk( 0 );
            ASSERT( lk.got() );
            ASSERT( Lock::isW() );
        }
    };

    class RWLockTest1 { 
    public:
        void run() { 
            RWLock lk( "eliot" );
            {
                rwlock r( lk , true , 1000 );
            }
        }
    };

    class RWLockTest2 { 
    public:
        static void worker1( RWLockRecursiveNongreedy * lk , AtomicUInt * x ) {
            (*x)++; // 1
            RWLockRecursiveNongreedy::Exclusive b(*lk);
            (*x)++; // 2
        }
        static void worker2( RWLockRecursiveNongreedy * lk , AtomicUInt * x ) {
            RWLockRecursiveNongreedy::Shared c(*lk);
            (*x)++;
        }
        void run() { 
            /**
             * note: this test will deadlock if the code breaks
             */            
            RWLockRecursiveNongreedy lk( "eliot2" , 120 * 1000 );
            cout << "RWLock impl: " << lk.implType() << endl;
            auto_ptr<RWLockRecursiveNongreedy::Shared> a( new RWLockRecursiveNongreedy::Shared(lk) );            
            AtomicUInt x1 = 0;
            cout << "A : " << &x1 << endl;
            boost::thread t1( boost::bind( worker1 , &lk , &x1 ) );
            while ( ! x1 );
            verify( x1 == 1 );
            sleepmillis( 500 );
            verify( x1 == 1 );            
            AtomicUInt x2 = 0;
            boost::thread t2( boost::bind( worker2, &lk , &x2 ) );
            t2.join();
            verify( x2 == 1 );
            a.reset();
            for ( int i=0; i<2000; i++ ) {
                if ( x1 == 2 )
                    break;
                sleepmillis(1);
            }
            verify( x1 == 2 );
            t1.join();            
        }
    };

    class RWLockTest3 { 
    public:        
        static void worker2( RWLockRecursiveNongreedy * lk , AtomicUInt * x ) {
    	    verify( ! lk->__lock_try(0) );
            RWLockRecursiveNongreedy::Shared c( *lk  );
            (*x)++;
        }

        void run() { 
            /**
             * note: this test will deadlock if the code breaks
             */
            
            RWLockRecursiveNongreedy lk( "eliot2" , 120 * 1000 );
            
            auto_ptr<RWLockRecursiveNongreedy::Shared> a( new RWLockRecursiveNongreedy::Shared( lk ) );
            
            AtomicUInt x2 = 0;

            boost::thread t2( boost::bind( worker2, &lk , &x2 ) );
            t2.join();
            verify( x2 == 1 );

            a.reset();            
        }
    };

    class RWLockTest4 { 
    public:
        
#if defined(__linux__) || defined(__APPLE__)
        static void worker1( pthread_rwlock_t * lk , AtomicUInt * x ) {
            (*x)++; // 1
            cout << "lock b try" << endl;
            while ( 1 ) {
                if ( pthread_rwlock_trywrlock( lk ) == 0 )
                    break;
                sleepmillis(10);
            }
            cout << "lock b got" << endl;
            (*x)++; // 2
            pthread_rwlock_unlock( lk );
        }

        static void worker2( pthread_rwlock_t * lk , AtomicUInt * x ) {
            cout << "lock c try" << endl;
            pthread_rwlock_rdlock( lk );
            (*x)++;
            cout << "lock c got" << endl;
            pthread_rwlock_unlock( lk );
        }
#endif
        void run() { 
            /**
             * note: this test will deadlock if the code breaks
             */
      
#if defined(__linux__) || defined(__APPLE__)      
            
            // create
            pthread_rwlock_t lk;
            verify( pthread_rwlock_init( &lk , 0 ) == 0 );
            
            // read lock
            verify( pthread_rwlock_rdlock( &lk ) == 0 );
            
            AtomicUInt x1 = 0;
            boost::thread t1( boost::bind( worker1 , &lk , &x1 ) );
            while ( ! x1 );
            verify( x1 == 1 );
            sleepmillis( 500 );
            verify( x1 == 1 );
            
            AtomicUInt x2 = 0;

            boost::thread t2( boost::bind( worker2, &lk , &x2 ) );
            t2.join();
            verify( x2 == 1 );

            pthread_rwlock_unlock( &lk );

            for ( int i=0; i<2000; i++ ) {
                if ( x1 == 2 )
                    break;
                sleepmillis(1);
            }

            verify( x1 == 2 );
            t1.join();
#endif            
        }
    };

    class List1Test2 : public ThreadedTest<> {
        static const int iterations = 1000; // note: a lot of iterations will use a lot of memory as List1 leaks on purpose
        class M : public List1<M>::Base {
        public:
            M(int x) : _x(x) { }
            const int _x;
        };
        List1<M> l;
    public:
        void validate() { }
        void subthread(int) {
            for(int i=0; i < iterations; i++) {
                int r = std::rand() % 256;
                if( r == 0 ) {
                    l.orphanAll();
                }
                else if( r < 4 ) { 
                    l.push(new M(r));
                }
                else {
                    M *orph = 0;
                    for( M *m = l.head(); m; m=m->next() ) { 
                        ASSERT( m->_x > 0 && m->_x < 4 );
                        if( r > 192 && std::rand() % 8 == 0 )
                            orph = m;
                    }
                    if( orph ) {
                        try { 
                            l.orphan(orph);
                        }
                        catch(...) { }
                    }
                }
            }
        }
    };

    class List1Test {
    public:
        class M : public List1<M>::Base {
            ~M();
        public:
            M( int x ) {
                num = x;
            }
            int num;
        };

        void run(){
            List1<M> l;
            
            vector<M*> ms;
            for ( int i=0; i<5; i++ ) {
                M * m = new M(i);
                ms.push_back( m );
                l.push( m );
            }
            
            // must assert as the item is missing
            ASSERT_THROWS( l.orphan( new M( -3 ) ) , UserException );
        }
    };

    // we don't use upgrade so that part is not important currently but the other aspects of this test are 
    // interesting; it would be nice to do analogous tests for SimpleRWLock and QLock
    class UpgradableTest : public ThreadedTest<7> {
        RWLock m;
    public:
        UpgradableTest() : m("utest") {}
    private:
        virtual void validate() { }
        virtual void subthread(int x) {
            Client::initThread("utest");

            /* r = get a read lock 
               R = get a read lock and we expect it to be fast
               u = get upgradable 
               U = get upgradable and we expect it to be fast
               w = get a write lock
            */
            //                    /-- verify upgrade can be done instantly while in a read lock already
            //                    |  /-- verify upgrade acquisition isn't greedy
            //                    |  | /-- verify writes aren't greedy while in upgradable (or are they?)
            //                    v  v v
            const char *what = " RURuRwR";

            sleepmillis(100*x);

            int Z = 1;
            log(Z) << x << ' ' << what[x] << " request" << endl;
            char ch = what[x];
            switch( ch ) { 
            case 'w':
                {
                    m.lock();
                    log(Z) << x << " w got" << endl;
                    sleepmillis(100);
                    log(Z) << x << " w unlock" << endl;
                    m.unlock();
                }
                break;
            case 'u':
            case 'U':
                {
                    Timer t;
                    RWLock::Upgradable u(m);
                    log(Z) << x << ' ' << ch << " got" << endl;
                    if( ch == 'U' ) {
#ifdef MONGO_USE_SRW_ON_WINDOWS
                        // SRW locks are neither fair nor FIFO, as per docs
                        if( t.millis() > 2000 ) {
#else
                        if( t.millis() > 20 ) {
#endif
                            DEV {
                                // a _DEBUG buildbot might be slow, try to avoid false positives
                                log() << "warning lock upgrade was slow " << t.millis() << endl;
                            }
                            else {
                                log() << "assertion failure: lock upgrade was too slow: " << t.millis() << endl;
                                ASSERT( false );
                            }
                        }
                    }
                    sleepsecs(1);
                    log(Z) << x << ' ' << ch << " unlock" << endl;
                }
                break;
            case 'r':
            case 'R':
                {
                    Timer t;
                    m.lock_shared();
                    log(Z) << x << ' ' << ch << " got " << endl;
                    if( what[x] == 'R' ) {
                        if( t.millis() > 15 ) { 
                            // commented out for less chatter, we aren't using upgradeable anyway right now: 
                            // log() << x << " info: when in upgradable, write locks are still greedy on this platform" << endl;
                        }
                    }
                    sleepmillis(200);
                    log(Z) << x << ' ' << ch << " unlock" << endl;
                    m.unlock_shared();
                }
                break;
            default:
                ASSERT(false);
            }

            cc().shutdown();
        }
    };

    void sleepalittle() { 
        Timer t;
        while( 1 ) { 
            boost::this_thread::yield();
            if( t.micros() > 8 )
                break;
        }
    }

    int once;

    /* This test is to see how long it takes to get a lock after there has been contention -- the OS 
         will need to reschedule us. if a spinlock, it will be fast of course, but these aren't spin locks.
       Experimenting with different # of threads would be a good idea.
    */
    template <class whichmutex, class scoped>
    class Slack : public ThreadedTest<17> {
    public:
        Slack() : m("slack") {
            k = 0;
            done = false;
            a = b = 0;
            locks = 0;
        }
    private:
        whichmutex m;
        char pad1[128];
        unsigned a, b;
        char pad2[128];
        unsigned locks;
        char pad3[128];
        volatile int k;

        virtual void validate() { 
            if( once++ == 0 ) {
                // <= 1.35 we use a different rwmutex impl so worth noting
                cout << "Boost version : " << BOOST_VERSION << endl;
            }
            cout << typeid(whichmutex).name() <<
             " Slack useful work fraction: " << ((double)a)/b << " locks:" << locks << endl;
        }
        void watch() {
            while( 1 ) { 
                b++;
                //__sync_synchronize();
                if( k ) { 
                    a++;
                }
                sleepmillis(0);
                if( done ) 
                    break;
            }
        }
        volatile bool done;
        virtual void subthread(int x) {
            if( x == 1 ) { 
                watch();
                return;
            }
            Timer t;
            unsigned lks = 0;
            while( 1 ) {
                scoped lk(m);
                k = 1;
                // not very long, we'd like to simulate about 100K locks per second
                sleepalittle();
                lks++;
                if( done ||  t.millis() > 1500 ) {
                    locks += lks;
                    k = 0;
                    break;
                }
                k = 0;
                //__sync_synchronize();
            }
            done = true;
        }
    };

    class CondSlack : public ThreadedTest<17> {
        Notification n;
    public:
        CondSlack() {
            k = 0;
            done = false;
            a = b = 0;
            locks = 0;
        }
    private:
        unsigned a, b;
        virtual void validate() { 
            cout << "CondSlack useful work fraction: " << ((double)a)/b << " locks:" << locks << endl;
        }
        unsigned locks;
        volatile int k;
        void watch() {
            while( 1 ) { 
                b++;
                if( k ) { 
                    a++;
                }
                sleepmillis(0);
                if( done ) 
                    break;
            }
        }
        volatile bool done;
        virtual void subthread(int x) {
            if( x == 1 ) { 
                n.notifyOne();
                watch();
                return;
            }
            Timer t;
            while( 1 ) {
                n.waitToBeNotified();
                verify( k == 0 );
                k = 1;
                // not very long, we'd like to simulate about 100K locks per second
                sleepalittle();
                k = 0; 
                locks++;
                n.notifyOne();
                if( done ||  t.millis() > 1500 )
                    break;
            }
            done = true;
        }
    };

    class WriteLocksAreGreedy : public ThreadedTest<3> {
    public:
        WriteLocksAreGreedy() : m("gtest") {}
    private:
        RWLock m;
        virtual void validate() { }
        virtual void subthread(int x) {
            int Z = 0;
            Client::initThread("utest");
            if( x == 1 ) { 
                log(Z) << mongo::curTimeMillis64() % 10000 << " 1" << endl;
                rwlock_shared lk(m);
                sleepmillis(300);
                log(Z) << mongo::curTimeMillis64() % 10000 << " 1x" << endl;
            }
            if( x == 2 ) {
                sleepmillis(100);
                log(Z) << mongo::curTimeMillis64() % 10000 << " 2" << endl;
                rwlock lk(m, true);
                log(Z) << mongo::curTimeMillis64() % 10000 << " 2x" << endl;
            }
            if( x == 3 ) {
                sleepmillis(200);
                Timer t;
                log(Z) << mongo::curTimeMillis64() % 10000 << " 3" << endl;
                rwlock_shared lk(m);
                log(Z) << mongo::curTimeMillis64() % 10000 << " 3x" << endl;
                log(Z) << t.millis() << endl;
                ASSERT( t.millis() > 50 );
            }
            cc().shutdown();
        }
    };

    static int pass;
    class QLockTest : public ThreadedTest<3> {
    public:
        bool gotW;
        QLockTest() : gotW(false), m() { }
        void setup() { 
            if( pass == 1) { 
                m.stop_greed();
            }
        }
        ~QLockTest() {
            m.start_greed();
        }
    private:
        QLock m;
        virtual void validate() { }
        virtual void subthread(int x) {
            int Z = 0;
            Client::initThread("qtest");
            if( x == 1 ) { 
                log(Z) << mongo::curTimeMillis64() % 10000 << " 1 lock_r()..." << endl;
                m.lock_r();
                log(Z) << mongo::curTimeMillis64() % 10000 << " 1            got" << endl;
                sleepmillis(300);
                m.unlock_r();
                log(Z) << mongo::curTimeMillis64() % 10000 << " 1 unlock_r()" << endl;
            }
            if( x == 2 || x == 4 ) {
                sleepmillis(x*50);
                log(Z) << mongo::curTimeMillis64() % 10000 << " 2 lock_W()..." << endl;
                m.lock_W();
                log(Z) << mongo::curTimeMillis64() % 10000 << " 2            got" << endl;
                gotW = true;
                m.unlock_W();
            }
            if( x == 3 ) {
                sleepmillis(200);

                Timer t;
                log(Z) << mongo::curTimeMillis64() % 10000 << " 3 lock_r()..." << endl;
                m.lock_r();
                verify( gotW );
                log(Z) << mongo::curTimeMillis64() % 10000 << " 3            got" << gotW << endl;
                m.unlock_r();
                log(Z) << t.millis() << endl;
                ASSERT( t.millis() > 50 );
            }
            cc().shutdown();
        }
    };

    // Tests waiting on the TicketHolder by running many more threads than can fit into the "hotel", but only
    // max _nRooms threads should ever get in at once
    class TicketHolderWaits : public ThreadedTest<10> {

        static const int checkIns = 1000;
        static const int rooms = 3;

    public:
        TicketHolderWaits() : _hotel( rooms ), _tickets( _hotel._nRooms ) {}

    private:

        class Hotel {
        public:
            Hotel( int nRooms ) : _frontDesk( "frontDesk" ), _nRooms( nRooms ), _checkedIn( 0 ), _maxRooms( 0 ) {}

            void checkIn(){
                scoped_lock lk( _frontDesk );
                _checkedIn++;
                verify( _checkedIn <= _nRooms );
                if( _checkedIn > _maxRooms ) _maxRooms = _checkedIn;
            }

            void checkOut(){
                scoped_lock lk( _frontDesk );
                _checkedIn--;
                verify( _checkedIn >= 0 );
            }

            mongo::mutex _frontDesk;
            int _nRooms;
            int _checkedIn;
            int _maxRooms;
        };

        Hotel _hotel;
        TicketHolder _tickets;

        virtual void subthread(int x) {

            string threadName = ( str::stream() << "ticketHolder" << x );
            Client::initThread( threadName.c_str() );

            for( int i = 0; i < checkIns; i++ ){

                _tickets.waitForTicket();
                TicketHolderReleaser whenDone( &_tickets );

                _hotel.checkIn();

                sleepalittle();
                if( i == checkIns - 1 ) sleepsecs( 2 );

                _hotel.checkOut();

                if( ( i % ( checkIns / 10 ) ) == 0 )
                    log() << "checked in " << i << " times..." << endl;

            }

            cc().shutdown();

        }

        virtual void validate() {

            // This should always be true, assuming that it takes < 1 sec for the hardware to process a check-out/check-in
            // Time for test is then ~ #threads / _nRooms * 2 seconds
            verify( _hotel._maxRooms == _hotel._nRooms );

        }

    };

    class All : public Suite {
    public:
        All() : Suite( "threading" ) { }

        void setupTests() {
            add< WriteLocksAreGreedy >();
            add< QLockTest >();
            add< QLockTest >();

            // Slack is a test to see how long it takes for another thread to pick up
            // and begin work after another relinquishes the lock.  e.g. a spin lock 
            // would have very little slack.
            add< Slack<mongo::mutex , mongo::mutex::scoped_lock > >();
            add< Slack<SimpleMutex,SimpleMutex::scoped_lock> >();
            add< Slack<SimpleRWLock,SimpleRWLock::Exclusive> >();
            add< CondSlack >();

            add< UpgradableTest >();
            add< List1Test >();
            add< List1Test2 >();

            add< IsAtomicUIntAtomic >();
            add< MVarTest >();
            add< ThreadPoolTest >();
            add< LockTest >();


            add< RWLockTest1 >();
            //add< RWLockTest2 >(); // SERVER-2996
            add< RWLockTest3 >();
            add< RWLockTest4 >();

            add< MongoMutexTest >();
            add< TicketHolderWaits >();
        }
    } myall;
}
