// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/platform/atomic.h"
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
            Atomic<bool>* flag,
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
    Atomic<bool>* const _flag;
    Notification<void>* const _canProceed;
    Notification<void>* const _destructorInvoked;
};

TEST(BackgroundJobBasic, NormalCase) {
    Atomic<bool> flag(false);
    TestJob tj(false, &flag);
    tj.go();
    ASSERT(tj.wait());
    ASSERT_EQUALS(true, flag.load());
}

TEST(BackgroundJobBasic, TimeOutCase) {
    Atomic<bool> flag(false);
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
    Atomic<bool> flag(false);
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
                std::lock_guard<std::mutex> lock(_mutex);
                ASSERT_FALSE(_hasRun);
                _hasRun = true;
            }

            _n.get();
        }

        void notify() {
            _n.set();
        }

    private:
        std::mutex _mutex;
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
