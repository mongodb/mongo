// spin_lock_test.cpp : spin_lcok.{h, cpp} unit test

/**
 *    Copyright (C) 2010 10gen Inc.
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
#include <boost/thread/thread.hpp>
#include "dbtests.h"
#include "../util/concurrency/spin_lock.h"
#include "../util/timer.h"

namespace {

    using mongo::SpinLock;

    class LockTester {
    public:
        LockTester( SpinLock* spin, int* counter )
            : _spin(spin), _counter(counter), _requests(0) {}

        ~LockTester() {
            delete _t;
        }

        void start( int increments ) {
            _t = new boost::thread( boost::bind(&LockTester::test, this, increments) );
        }

        void join() {
            if ( _t ) _t->join();
        }

        int requests() const {
            return _requests;
        }

    private:
        SpinLock*      _spin;     // not owned here
        int*           _counter;  // not owned here
        int            _requests;
        boost::thread* _t;

        void test( int increments ) {
            while ( increments-- > 0 ) {
                _spin->lock();
                ++(*_counter);
                ++_requests;
                _spin->unlock();
            }
        }

        LockTester( LockTester& );
        LockTester& operator=( LockTester& );
    };

    class ConcurrentIncs {
    public:
        void run() {

            SpinLock spin;
            int counter = 0;

            const int threads = 64;
            const int incs = 50000;
            LockTester* testers[threads];
            
            Timer timer;

            for ( int i = 0; i < threads; i++ ) {
                testers[i] = new LockTester( &spin, &counter );
            }
            for ( int i = 0; i < threads; i++ ) {
                testers[i]->start( incs );
            }
            for ( int i = 0; i < threads; i++ ) {
                testers[i]->join();
                ASSERT_EQUALS( testers[i]->requests(), incs );
                delete testers[i];
            }
      
            int ms = timer.millis();
            log() << "spinlock ConcurrentIncs time: " << ms << endl;
            
            ASSERT_EQUALS( counter, threads*incs );
#if defined(__linux__)
            ASSERT( SpinLock::isfast() );
#endif

        }
    };

    class SpinLockSuite : public Suite {
    public:
        SpinLockSuite() : Suite( "spinlock" ) {}

        void setupTests() {
            add< ConcurrentIncs >();
        }
    } spinLockSuite;

}  // anonymous namespace
