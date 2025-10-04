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

#include "mongo/base/string_data.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/mutex.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/background.h"
#include "mongo/util/concurrency/notification.h"

#include <mutex>
#include <string>

namespace mongo {
namespace {

class TestJob final : public BackgroundJob {
public:
    TestJob(bool selfDelete,
            AtomicWord<bool>* flag,
            Notification<void>* canProceed = nullptr,
            Notification<void>* destructorInvoked = nullptr)
        : BackgroundJob(selfDelete),
          _flag(flag),
          _canProceed(canProceed),
          _destructorInvoked(destructorInvoked) {}

    ~TestJob() override {
        if (_destructorInvoked)
            _destructorInvoked->set();
    }

    std::string name() const override {
        return "TestJob";
    }

    void run() override {
        if (_canProceed)
            _canProceed->get();
        _flag->store(true);
    }

private:
    AtomicWord<bool>* const _flag;
    Notification<void>* const _canProceed;
    Notification<void>* const _destructorInvoked;
};

TEST(BackgroundJobBasic, NormalCase) {
    AtomicWord<bool> flag(false);
    TestJob tj(false, &flag);
    tj.go();
    ASSERT(tj.wait());
    ASSERT_EQUALS(true, flag.load());
}

TEST(BackgroundJobBasic, TimeOutCase) {
    AtomicWord<bool> flag(false);
    Notification<void> canProceed;
    TestJob tj(false, &flag, &canProceed);
    tj.go();

    ASSERT(!tj.wait(1000));
    ASSERT_EQUALS(false, flag.load());

    canProceed.set();
    ASSERT(tj.wait());
    ASSERT_EQUALS(true, flag.load());
}

TEST(BackgroundJobBasic, SelfDeletingCase) {
    AtomicWord<bool> flag(false);
    Notification<void> destructorInvoked;
    // Though it looks like one, this is not a leak since the job is self deleting.
    (new TestJob(true, &flag, nullptr, &destructorInvoked))->go();
    destructorInvoked.get();
    ASSERT_EQUALS(true, flag.load());
}

TEST(BackgroundJobLifeCycle, Go) {
    class Job : public BackgroundJob {
    public:
        Job() : _hasRun(false) {}

        std::string name() const override {
            return "BackgroundLifeCycle::CannotCallGoAgain";
        }

        void run() override {
            {
                stdx::lock_guard<stdx::mutex> lock(_mutex);
                ASSERT_FALSE(_hasRun);
                _hasRun = true;
            }

            _n.get();
        }

        void notify() {
            _n.set();
        }

    private:
        stdx::mutex _mutex;
        bool _hasRun;
        Notification<void> _n;
    };

    Job j;

    // This call starts Job running.
    j.go();

    // Calling 'go' again while it is running is an error.
    ASSERT_THROWS(j.go(), AssertionException);

    // Stop the Job
    j.notify();
    j.wait();

    // Calling 'go' on a done task is a no-op. If it were not,
    // we would fail the assert in Job::run above.
    j.go();
}

}  // namespace
}  // namespace mongo
