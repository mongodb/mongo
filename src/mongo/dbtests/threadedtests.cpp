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
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include <boost/thread/barrier.hpp>
#include <boost/version.hpp>
#include <iostream>

#include "mongo/config.h"
#include "mongo/db/client.h"
#include "mongo/dbtests/dbtests.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/bits.h"
#include "mongo/stdx/functional.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/concurrency/old_thread_pool.h"
#include "mongo/util/concurrency/ticketholder.h"
#include "mongo/util/log.h"
#include "mongo/util/timer.h"

namespace ThreadedTests {

using std::unique_ptr;
using std::cout;
using std::endl;
using std::string;

template <int nthreads_param = 10>
class ThreadedTest {
public:
    virtual void setup() {}                     // optional
    virtual void subthread(int remaining) = 0;  // each thread whatever test work you want done
    virtual void validate() = 0;                // after work is done

    static const int nthreads = nthreads_param;

    void run() {
        setup();
        launch_subthreads(nthreads);
        validate();
    }

    virtual ~ThreadedTest(){};  // not necessary, but makes compilers happy

private:
    void launch_subthreads(int remaining) {
        if (!remaining)
            return;

        stdx::thread athread(stdx::bind(&ThreadedTest::subthread, this, remaining));
        launch_subthreads(remaining - 1);
        athread.join();
    }
};

template <typename _AtomicUInt>
class IsAtomicWordAtomic : public ThreadedTest<> {
    static const int iterations = 1000000;
    typedef typename _AtomicUInt::WordType WordType;
    _AtomicUInt target;

    void subthread(int) {
        for (int i = 0; i < iterations; i++) {
            target.fetchAndAdd(WordType(1));
        }
    }
    void validate() {
        ASSERT_EQUALS(target.load(), unsigned(nthreads * iterations));

        _AtomicUInt u;
        ASSERT_EQUALS(0u, u.load());
        ASSERT_EQUALS(0u, u.fetchAndAdd(WordType(1)));
        ASSERT_EQUALS(2u, u.addAndFetch(WordType(1)));
        ASSERT_EQUALS(2u, u.fetchAndSubtract(WordType(1)));
        ASSERT_EQUALS(0u, u.subtractAndFetch(WordType(1)));
        ASSERT_EQUALS(0u, u.load());

        u.fetchAndAdd(WordType(1));
        ASSERT_GREATER_THAN(u.load(), WordType(0));

        u.fetchAndSubtract(WordType(1));
        ASSERT_NOT_GREATER_THAN(u.load(), WordType(0));
    }
};

class ThreadPoolTest {
    static const unsigned iterations = 10000;
    static const unsigned nThreads = 8;

    AtomicUInt32 counter;
    void increment(unsigned n) {
        for (unsigned i = 0; i < n; i++) {
            counter.fetchAndAdd(1);
        }
    }

public:
    void run() {
        OldThreadPool tp(nThreads);

        for (unsigned i = 0; i < iterations; i++) {
            tp.schedule(&ThreadPoolTest::increment, this, 2);
        }

        tp.join();

        ASSERT_EQUALS(counter.load(), iterations * 2);
    }
};

void sleepalittle() {
    Timer t;
    while (1) {
        stdx::this_thread::yield();
        if (t.micros() > 8)
            break;
    }
}

int once;

/* This test is to see how long it takes to get a lock after there has been contention -- the OS
   will need to reschedule us. if a spinlock, it will be fast of course, but these aren't spin
   locks. Experimenting with different # of threads would be a good idea.
*/
template <class whichmutex, class scoped>
class Slack : public ThreadedTest<17> {
public:
    Slack() {
        k.store(0);
        done.store(false);
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
    AtomicInt32 k;

    virtual void validate() {
        if (once++ == 0) {
            // <= 1.35 we use a different rwmutex impl so worth noting
            cout << "Boost version : " << BOOST_VERSION << endl;
        }
        cout << typeid(whichmutex).name() << " Slack useful work fraction: " << ((double)a) / b
             << " locks:" << locks << endl;
    }
    void watch() {
        while (1) {
            b++;
            //__sync_synchronize();
            if (k.load()) {
                a++;
            }
            sleepmillis(0);
            if (done.load())
                break;
        }
    }
    AtomicBool done;
    virtual void subthread(int x) {
        if (x == 1) {
            watch();
            return;
        }
        Timer t;
        unsigned lks = 0;
        while (1) {
            scoped lk(m);
            k.store(1);
            // not very long, we'd like to simulate about 100K locks per second
            sleepalittle();
            lks++;
            if (done.load() || t.millis() > 1500) {
                locks += lks;
                k.store(0);
                break;
            }
            k.store(0);
            //__sync_synchronize();
        }
        done.store(true);
    }
};

// Tests waiting on the TicketHolder by running many more threads than can fit into the "hotel", but
// only max _nRooms threads should ever get in at once
class TicketHolderWaits : public ThreadedTest<10> {
    static const int checkIns = 1000;
    static const int rooms = 3;

public:
    TicketHolderWaits() : _hotel(rooms), _tickets(_hotel._nRooms) {}

private:
    class Hotel {
    public:
        Hotel(int nRooms) : _nRooms(nRooms), _checkedIn(0), _maxRooms(0) {}

        void checkIn() {
            stdx::lock_guard<stdx::mutex> lk(_frontDesk);
            _checkedIn++;
            verify(_checkedIn <= _nRooms);
            if (_checkedIn > _maxRooms)
                _maxRooms = _checkedIn;
        }

        void checkOut() {
            stdx::lock_guard<stdx::mutex> lk(_frontDesk);
            _checkedIn--;
            verify(_checkedIn >= 0);
        }

        stdx::mutex _frontDesk;
        int _nRooms;
        int _checkedIn;
        int _maxRooms;
    };

    Hotel _hotel;
    TicketHolder _tickets;

    virtual void subthread(int x) {
        string threadName = (str::stream() << "ticketHolder" << x);
        Client::initThread(threadName.c_str());

        for (int i = 0; i < checkIns; i++) {
            _tickets.waitForTicket();
            TicketHolderReleaser whenDone(&_tickets);

            _hotel.checkIn();

            sleepalittle();
            if (i == checkIns - 1)
                sleepsecs(2);

            _hotel.checkOut();

            if ((i % (checkIns / 10)) == 0)
                mongo::unittest::log() << "checked in " << i << " times..." << endl;
        }
    }

    virtual void validate() {
        // This should always be true, assuming that it takes < 1 sec for the hardware to process a
        // check-out/check-in Time for test is then ~ #threads / _nRooms * 2 seconds
        verify(_hotel._maxRooms == _hotel._nRooms);
    }
};

class All : public Suite {
public:
    All() : Suite("threading") {}

    void setupTests() {
        // Slack is a test to see how long it takes for another thread to pick up
        // and begin work after another relinquishes the lock.  e.g. a spin lock
        // would have very little slack.
        add<Slack<SimpleMutex, stdx::lock_guard<SimpleMutex>>>();

        add<IsAtomicWordAtomic<AtomicUInt32>>();
        add<IsAtomicWordAtomic<AtomicUInt64>>();
        add<ThreadPoolTest>();

        add<TicketHolderWaits>();
    }
};

SuiteInstance<All> myall;
}  // namespace ThreadedTests
