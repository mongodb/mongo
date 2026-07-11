// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/storage/checkpoint_schedule_policy.h"

#include "mongo/db/storage/storage_options.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"

#include <mutex>

namespace mongo {
namespace {

//
// FixedIntervalPolicy test fixture. Drives the policy through its wait loop using a
// mock clock and shared mutex/cv/flags, mirroring the real Checkpointer interface.
//
class FixedIntervalPolicyTest : public unittest::Test {
protected:
    void setUp() override {
        _clock.reset();
        _originalSyncdelay = storageGlobalParams.syncdelay.load();
    }

    void tearDown() override {
        storageGlobalParams.syncdelay.store(_originalSyncdelay);
        _clock.reset();
    }

    stdx::thread startWait(CheckpointSchedulePolicy& policy) {
        _needStartupWait = true;
        return stdx::thread([this, &policy] {
            std::unique_lock<std::mutex> lock(_mutex);
            {
                std::lock_guard<std::mutex> startLk(_startMutex);
                _threadHasLock = true;
            }
            _startCv.notify_one();
            policy.waitUntilReady(lock, _cv, [&] { return _shuttingDown || _triggerCheckpoint; });
        });
    }

    // Blocks until the background thread is inside a cv.wait_until call (alarm registered).
    void waitUntilThreadBlocking() {
        if (_needStartupWait) {
            _needStartupWait = false;
            std::unique_lock<std::mutex> startLk(_startMutex);
            _startCv.wait(startLk, [this] { return _threadHasLock; });
            _threadHasLock = false;
        }
        std::lock_guard<std::mutex> lk(_mutex);
    }

    void signalShutdown() {
        {
            std::lock_guard<std::mutex> lk(_mutex);
            _shuttingDown = true;
        }
        _cv.notify_one();
    }

    void signalTrigger() {
        {
            std::lock_guard<std::mutex> lk(_mutex);
            _triggerCheckpoint = true;
        }
        _cv.notify_one();
    }

    ClockSourceMock _clock;

private:
    double _originalSyncdelay{-1.0};
    std::mutex _mutex;
    stdx::condition_variable _cv;
    bool _shuttingDown{false};
    bool _triggerCheckpoint{false};
    std::mutex _startMutex;
    stdx::condition_variable _startCv;
    bool _threadHasLock{false};
    bool _needStartupWait{false};
};

TEST_F(FixedIntervalPolicyTest, ExitsAfterSyncdelayInterval) {
    storageGlobalParams.syncdelay.store(10.0);
    auto policy = createFixedIntervalPolicy(&_clock);
    auto t = startWait(*policy);
    waitUntilThreadBlocking();
    _clock.advance(Milliseconds(10000));
    t.join();
}

TEST_F(FixedIntervalPolicyTest, ShutdownSignalExitsImmediately) {
    storageGlobalParams.syncdelay.store(60.0);
    auto policy = createFixedIntervalPolicy(&_clock);
    auto t = startWait(*policy);
    waitUntilThreadBlocking();
    signalShutdown();
    t.join();
}

TEST_F(FixedIntervalPolicyTest, TriggerCheckpointExitsImmediately) {
    storageGlobalParams.syncdelay.store(60.0);
    auto policy = createFixedIntervalPolicy(&_clock);
    auto t = startWait(*policy);
    waitUntilThreadBlocking();
    signalTrigger();
    t.join();
}

TEST_F(FixedIntervalPolicyTest, SyncdelayZeroBlocksInPollingLoopUntilShutdown) {
    // syncdelay=0 disables checkpointing. The policy enters a polling while loop, re-waiting
    // every 3 seconds. It does not exit until shutdown or triggerCheckpoint is signaled.
    storageGlobalParams.syncdelay.store(0.0);
    auto policy = createFixedIntervalPolicy(&_clock);
    auto t = startWait(*policy);
    waitUntilThreadBlocking();  // thread is in the 3-second polling wait
    signalShutdown();
    t.join();
}

}  // namespace
}  // namespace mongo
