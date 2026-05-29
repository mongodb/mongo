/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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
