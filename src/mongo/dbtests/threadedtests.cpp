/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */


#include <boost/version.hpp>
#include <fmt/format.h>
#include <iostream>
#include <memory>
#include <string>
#include <typeinfo>

#include "mongo/db/client.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/admission_context.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/concurrency/ticketholder.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"
#include "mongo/util/timer.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {
namespace ThreadedTests {

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

        stdx::thread athread([=, this] { subthread(remaining); });
        launch_subthreads(remaining - 1);
        athread.join();
    }
};

template <typename _AtomicUInt>
class IsAtomicWordAtomic : public ThreadedTest<> {
    static const int iterations = 1000000;
    typedef typename _AtomicUInt::WordType WordType;
    _AtomicUInt target;

    void subthread(int) override {
        for (int i = 0; i < iterations; i++) {
            target.fetchAndAdd(WordType(1));
        }
    }
    void validate() override {
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

    AtomicWord<unsigned> counter;
    void increment(unsigned n) {
        for (unsigned i = 0; i < n; i++) {
            counter.fetchAndAdd(1);
        }
    }

public:
    void run() {
        ThreadPool::Options options;
        options.maxThreads = options.minThreads = nThreads;
        ThreadPool tp(options);
        tp.startup();

        for (unsigned i = 0; i < iterations; i++) {
            tp.schedule([=, this](auto status) {
                ASSERT_OK(status);
                increment(2);
            });
        }

        tp.waitForIdle();
        tp.shutdown();
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
    AtomicWord<int> k;

    void validate() override {
        if (once++ == 0) {
            // <= 1.35 we use a different rwmutex impl so worth noting
            std::cout << "Boost version : " << BOOST_VERSION << std::endl;
        }
        std::cout << typeid(whichmutex).name() << " Slack useful work fraction: " << ((double)a) / b
                  << " locks:" << locks << std::endl;
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
    AtomicWord<bool> done;
    void subthread(int x) override {
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


class All : public unittest::OldStyleSuiteSpecification {
public:
    All() : OldStyleSuiteSpecification("threading") {}

    void setupTests() override {
        // Slack is a test to see how long it takes for another thread to pick up
        // and begin work after another relinquishes the lock.  e.g. a spin lock
        // would have very little slack.
        add<Slack<stdx::mutex, stdx::lock_guard<stdx::mutex>>>();

        add<IsAtomicWordAtomic<AtomicWord<unsigned>>>();
        add<IsAtomicWordAtomic<AtomicWord<unsigned long long>>>();
        add<ThreadPoolTest>();
    }
};

unittest::OldStyleSuiteInitializer<All> myall;

}  // namespace ThreadedTests
}  // namespace mongo
