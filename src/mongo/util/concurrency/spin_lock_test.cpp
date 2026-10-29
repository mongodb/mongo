// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/util/concurrency/spin_lock.h"

#include "mongo/logv2/log.h"
#include "mongo/stdx/thread.h"
#include "mongo/stdx/type_traits.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/timer.h"

#include <string>
#include <vector>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

class LockTester {
public:
    LockTester(SpinLock* spin, int* counter) : _spin(spin), _counter(counter), _requests(0) {}

    virtual ~LockTester() {
        delete _t;
    }

    void start(int increments) {
        _t = new stdx::thread([this, increments] { test(increments); });
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
            acquireLock();
            ++(*_counter);
            ++_requests;
            _spin->unlock();
        }
    }

    LockTester(LockTester&);
    LockTester& operator=(LockTester&);

protected:
    SpinLock* spin() const {
        return _spin;
    }

    virtual void acquireLock() {
        spin()->lock();
    }
};

class TryLockTester final : public LockTester {
public:
    TryLockTester(SpinLock* spin, int* counter) : LockTester(spin, counter) {}

protected:
    void acquireLock() override {
        while (!spin()->try_lock()) {
        }
    }
};

class SpinLockTester {
public:
    template <class T>
    void run(std::string testName) {
        SpinLock spin;
        int counter = 0;

        std::vector<T*> testers(_threads);
        Timer timer;

        for (int i = 0; i < _threads; i++) {
            testers[i] = new T(&spin, &counter);
        }
        for (int i = 0; i < _threads; i++) {
            testers[i]->start(_incs);
        }
        for (int i = 0; i < _threads; i++) {
            testers[i]->join();
            ASSERT_EQUALS(testers[i]->requests(), _incs);
            delete testers[i];
        }

        int ms = timer.millis();
        LOGV2(24149, "spinlock {testName} time: {ms}", "testName"_attr = testName, "ms"_attr = ms);

        ASSERT_EQUALS(counter, _threads * _incs);
    }

private:
    const int _threads = 64;
    const int _incs = 50000;
};


TEST(Concurrency, ConcurrentIncs) {
    SpinLockTester tester;
    tester.run<LockTester>("ConcurrentIncs");
}

TEST(Concurrency, ConcurrentIncsWithTryLock) {
    SpinLockTester tester;
    tester.run<TryLockTester>("ConcurrentIncsWithTryLock");
}

}  // namespace
}  // namespace mongo
