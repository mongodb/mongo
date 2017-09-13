/**
 *    Copyright (C) 2013 10gen Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/stdx/functional.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/concurrency/spin_lock.h"
#include "mongo/util/timer.h"

namespace {

using mongo::SpinLock;
using mongo::Timer;

namespace stdx = mongo::stdx;

class LockTester {
public:
    LockTester(SpinLock* spin, int* counter) : _spin(spin), _counter(counter), _requests(0) {}

    ~LockTester() {
        delete _t;
    }

    void start(int increments) {
        _t = new stdx::thread(mongo::stdx::bind(&LockTester::test, this, increments));
    }

    void join() {
        if (_t)
            _t->join();
    }

    int requests() const {
        return _requests;
    }

private:
    SpinLock* _spin;  // not owned here
    int* _counter;    // not owned here
    int _requests;
    stdx::thread* _t;

    void test(int increments) {
        while (increments-- > 0) {
            _spin->lock();
            ++(*_counter);
            ++_requests;
            _spin->unlock();
        }
    }

    LockTester(LockTester&);
    LockTester& operator=(LockTester&);
};


TEST(Concurrency, ConcurrentIncs) {
    SpinLock spin;
    int counter = 0;

    const int threads = 64;
    const int incs = 50000;
    LockTester* testers[threads];

    Timer timer;

    for (int i = 0; i < threads; i++) {
        testers[i] = new LockTester(&spin, &counter);
    }
    for (int i = 0; i < threads; i++) {
        testers[i]->start(incs);
    }
    for (int i = 0; i < threads; i++) {
        testers[i]->join();
        ASSERT_EQUALS(testers[i]->requests(), incs);
        delete testers[i];
    }

    int ms = timer.millis();
    mongo::unittest::log() << "spinlock ConcurrentIncs time: " << ms << std::endl;

    ASSERT_EQUALS(counter, threads * incs);
}

}  // namespace
