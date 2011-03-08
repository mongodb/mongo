// threadedtests.cpp - Tests for threaded code
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
#include "../bson/util/atomic_int.h"
#include "../util/concurrency/mvar.h"
#include "../util/concurrency/thread_pool.h"
#include "../util/timer.h"
#include <boost/thread.hpp>
#include <boost/bind.hpp>

#include "dbtests.h"

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

    class MongoMutexTest : public ThreadedTest<135> {
#if defined(_DEBUG)
        enum { N = 5000 };
#else
        enum { N = 40000 };
#endif
        MongoMutex *mm;
    public:
        void run() {
            Timer t;
            cout << "MongoMutexTest N:" << N << endl;
            ThreadedTest<135>::run();
            cout << "MongoMutexTest " << t.millis() << "ms" << endl;
        }
    private:
        virtual void setup() {
            mm = new MongoMutex("MongoMutexTest");
        }
        virtual void subthread(int) {
            Client::initThread("mongomutextest");
            sleepmillis(0);
            for( int i = 0; i < N; i++ ) {
                if( i % 7 == 0 ) {
                    mm->lock_shared();
                    mm->lock_shared();
                    mm->unlock_shared();
                    mm->unlock_shared();
                }
                else if( i % 7 == 1 ) {
                    mm->lock_shared();
                    ASSERT( mm->atLeastReadLocked() );
                    mm->unlock_shared();
                }
                else if( i % 7 == 2 ) {
                    mm->lock();
                    ASSERT( mm->isWriteLocked() );
                    mm->unlock();
                }
                else if( i % 7 == 3 ) {
                    mm->lock();
                    mm->lock_shared();
                    ASSERT( mm->isWriteLocked() );
                    mm->unlock_shared();
                    mm->unlock();
                }
                else if( i % 7 == 4 ) {
                    mm->lock();
                    mm->releaseEarly();
                    mm->unlock();
                }
                else if( i % 7 == 5 ) {
                    if( mm->lock_try(1) ) {
                        mm->unlock();
                    }
                }
                else if( i % 7 == 6 ) {
                    if( mm->lock_shared_try(0) ) {
                        mm->unlock_shared();
                    }
                }
                else {
                    mm->lock_shared();
                    mm->unlock_shared();
                }
            }
            cc().shutdown();
        }
        virtual void validate() {
            ASSERT( !mm->atLeastReadLocked() );
            mm->lock();
            mm->unlock();
            mm->lock_shared();
            mm->unlock_shared();
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

            writelocktry lk( "" , 0 );
            ASSERT( lk.got() );
        }
    };

    class RWLockTest1 { 
    public:
        void run() { 
            RWLock lk( "eliot" );
            {
                rwlock r( lk , true , false , 1000 );
            }
        }
    };

    class RWLockTest2 { 
    public:
        
        static void worker( const RWLock * lk , int * x ) {
            *x = 1;
            cout << "lock b try" << endl;
            rwlock b( *lk , true ); 
            cout << "lock b got" << endl;
            *x = 2;
        }

        void run() { 
            /**
             * note: this test will deadlock if the code breaks
             */
            
            RWLock lk( "eliot2" , 10000 );
            
            auto_ptr<rwlock> a( new rwlock( lk , false ) );
            
            int x = 0;
            boost::thread t( boost::bind( worker , &lk , &x ) );
            while ( ! x );
            assert( x == 1 );
            sleepmillis( 500 );
            assert( x == 1 );
            
            cout << "lock c try" << endl;
            auto_ptr<rwlock> c( new rwlock( lk , false ) );
            cout << "lock c got" << endl;

            c.reset();
            a.reset();

            for ( int i=0; i<2000; i++ ) {
                if ( x == 2 )
                    break;
                sleepmillis(1);
            }

            assert( x == 2 );
            t.join();
            
        }
    };


    class All : public Suite {
    public:
        All() : Suite( "threading" ) {
        }

        void setupTests() {
            add< IsAtomicUIntAtomic >();
            add< MVarTest >();
            add< ThreadPoolTest >();
            add< LockTest >();
            add< RWLockTest1 >();
            add< RWLockTest2 >();
            add< MongoMutexTest >();
        }
    } myall;
}
