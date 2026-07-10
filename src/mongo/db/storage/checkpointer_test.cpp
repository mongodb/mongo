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

#include "mongo/db/storage/checkpointer.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/storage/checkpoint_schedule_policy.h"
#include "mongo/platform/atomic.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/time_support.h"

#include <functional>
#include <memory>
#include <mutex>

namespace mongo {
namespace {

/**
 * A schedule policy that blocks each loop iteration until the Checkpointer must wake (shutdown,
 * trigger, or a first-checkpoint pause request), counting how many times the loop has entered the
 * schedule. Because it never returns on its own, the Checkpointer never advances to take an actual
 * checkpoint, so the run loop never touches the storage engine. That keeps the test focused on the
 * pause/resume synchronization and free of storage-engine setup.
 */
class GatedSchedulePolicy : public CheckpointSchedulePolicy {
public:
    void waitUntilReady(std::unique_lock<std::mutex>& lock,
                        stdx::condition_variable& cv,
                        std::function<bool()> shouldWake) override {
        enteredCount.fetchAndAdd(1);
        cv.wait(lock, std::move(shouldWake));
    }

    bool accumulateOplogBytes(int64_t) override {
        return false;
    }

    Atomic<int64_t> enteredCount{0};
};

/**
 * A schedule policy that deliberately ignores the wake predicate and blocks on a test-controlled
 * gate instead, so the run loop can never reach the first-checkpoint park branch on its own. That
 * keeps a pauseCheckpointing() caller blocked on its own CV, letting a test verify shutdown()
 * wakes it. The checkpointer lock is released while gated so other threads can make progress, and
 * reacquired before returning to honor the run loop's contract.
 */
class ManualGatePolicy : public CheckpointSchedulePolicy {
public:
    void waitUntilReady(std::unique_lock<std::mutex>& lock,
                        stdx::condition_variable& cv,
                        std::function<bool()> shouldWake) override {
        enteredCount.fetchAndAdd(1);
        lock.unlock();
        {
            std::unique_lock<std::mutex> gateLock(_gateMutex);
            _gateCv.wait(gateLock, [&] { return _gateOpen; });
        }
        lock.lock();
    }

    bool accumulateOplogBytes(int64_t) override {
        return false;
    }

    void openGate() {
        {
            std::lock_guard<std::mutex> gateLock(_gateMutex);
            _gateOpen = true;
        }
        _gateCv.notify_all();
    }

    Atomic<int64_t> enteredCount{0};

private:
    std::mutex _gateMutex;
    stdx::condition_variable _gateCv;
    bool _gateOpen = false;
};

// Polls until the schedule has been entered at least 'target' times, failing the test rather than
// hanging forever if the Checkpointer thread never gets there.
void waitForEnteredCountAtLeast(Atomic<int64_t>& enteredCount, int64_t target) {
    const auto deadline = Date_t::now() + Seconds(30);
    while (enteredCount.load() < target) {
        ASSERT_LT(Date_t::now(), deadline)
            << "checkpoint thread did not re-enter the schedule in time";
        sleepmillis(5);
    }
}

class CheckpointerTest : public ServiceContextTest {};

TEST_F(CheckpointerTest, WaitForFirstCheckpointParksThreadThenResumeReleasesIt) {
    auto policy = std::make_unique<GatedSchedulePolicy>();
    auto* policyPtr = policy.get();
    Checkpointer checkpointer(std::move(policy));
    checkpointer.go();

    // The thread enters the schedule once and blocks there.
    waitForEnteredCountAtLeast(policyPtr->enteredCount, 1);
    const auto enteredBeforePause = policyPtr->enteredCount.load();

    // Must return (a deadlock here means the pause protocol is broken).
    checkpointer.pauseCheckpointing();

    // While parked the thread sits in the pause branch, not in the schedule, so the count is
    // frozen.
    sleepmillis(100);
    ASSERT_EQ(policyPtr->enteredCount.load(), enteredBeforePause);

    // Resuming lets the loop run again and re-enter the schedule.
    checkpointer.resumeCheckpointing();
    waitForEnteredCountAtLeast(policyPtr->enteredCount, enteredBeforePause + 1);

    checkpointer.shutdown({ErrorCodes::ShutdownInProgress, "test complete"});
}

TEST_F(CheckpointerTest, ShutdownWhileParkedDoesNotHang) {
    auto policy = std::make_unique<GatedSchedulePolicy>();
    auto* policyPtr = policy.get();
    Checkpointer checkpointer(std::move(policy));
    checkpointer.go();

    waitForEnteredCountAtLeast(policyPtr->enteredCount, 1);
    checkpointer.pauseCheckpointing();

    // Shutting down a parked thread must unblock it and join cleanly.
    checkpointer.shutdown({ErrorCodes::ShutdownInProgress, "test complete"});
}

TEST_F(CheckpointerTest, ShutdownWakesCallerBlockedInWaitForFirstCheckpoint) {
    auto policy = std::make_unique<ManualGatePolicy>();
    auto* policyPtr = policy.get();
    Checkpointer checkpointer(std::move(policy));
    checkpointer.go();

    // The run loop is gated and cannot park, so it can never release a pauseCheckpointing()
    // caller on its own. The only thing that can release the caller is shutdown().
    waitForEnteredCountAtLeast(policyPtr->enteredCount, 1);

    Atomic<bool> waiterReturned{false};
    stdx::thread waiter([&] {
        checkpointer.pauseCheckpointing();
        waiterReturned.store(true);
    });

    // Confirm the waiter is genuinely blocked before we shut down. Because the run loop is gated it
    // cannot park, so a not-yet-returned waiter is parked on _waitForPauseCV. This
    // rules out the waiter observing _shuttingDown up front, which would mask the missed-wakeup
    // bug.
    sleepmillis(200);
    ASSERT_FALSE(waiterReturned.load())
        << "waiter should still be blocked while the run loop is gated";

    // shutdown() joins the (still gated) run loop, so run it on its own thread and open the gate
    // afterwards to let the run loop observe _shuttingDown and exit.
    stdx::thread shutdownThread(
        [&] { checkpointer.shutdown({ErrorCodes::ShutdownInProgress, "test complete"}); });

    // shutdown() must wake the blocked waiter promptly. Without notifying
    // _waitForPauseCV the waiter would only return on a spurious wakeup, so a missed
    // wakeup shows up as this deadline expiring.
    const auto deadline = Date_t::now() + Seconds(30);
    while (!waiterReturned.load()) {
        ASSERT_LT(Date_t::now(), deadline) << "shutdown() did not wake pauseCheckpointing()";
        sleepmillis(5);
    }

    waiter.join();
    policyPtr->openGate();
    shutdownThread.join();
}

// Unlike CheckpointerTest, this fixture brings up a real storage engine (plus the replication
// coordinator and replicated-storage-service plumbing the run loop touches after a checkpoint), so
// the loop can take an actual checkpoint. That lets us exercise the path where a checkpoint is in
// progress when pauseCheckpointing() is called.
class CheckpointerWithStorageTest : public ServiceContextMongoDTest {};

TEST_F(CheckpointerWithStorageTest, PauseWaitsForInProgressCheckpoint) {
    // Hold the checkpoint thread inside an in-progress checkpoint: the failpoint sits between the
    // schedule and the actual storage-engine checkpoint, with the checkpointer lock released, which
    // is exactly the window pauseCheckpointing() must wait out before it can return.
    auto* fp = globalFailPointRegistry().find("pauseCheckpointThread");
    ASSERT(fp);
    const auto timesEnteredBefore = fp->setMode(FailPoint::alwaysOn);

    auto policy = std::make_unique<GatedSchedulePolicy>();
    auto* policyPtr = policy.get();
    Checkpointer checkpointer(std::move(policy));
    checkpointer.go();

    // Let the loop reach the schedule, then trigger a checkpoint so it leaves the schedule and
    // advances into the (failpoint-held) checkpoint phase.
    waitForEnteredCountAtLeast(policyPtr->enteredCount, 1);
    checkpointer.triggerFirstStableCheckpoint(Timestamp(1, 1) /* prevStable */,
                                              Timestamp(1, 2) /* initialData */,
                                              Timestamp(1, 2) /* currStable */);

    // Wait until the thread is parked in the failpoint: a checkpoint is now genuinely in progress.
    fp->waitForTimesEntered(timesEnteredBefore + 1);

    Atomic<bool> pauseReturned{false};
    stdx::thread pauser([&] {
        checkpointer.pauseCheckpointing();
        pauseReturned.store(true);
    });

    // pauseCheckpointing() must not return while the checkpoint is still in progress.
    sleepmillis(200);
    ASSERT_FALSE(pauseReturned.load())
        << "pauseCheckpointing() returned while a checkpoint was still in progress";

    // Letting the in-progress checkpoint finish must allow pauseCheckpointing() to complete: the
    // loop finishes the checkpoint, comes back around, and parks, which is what releases the
    // caller.
    fp->setMode(FailPoint::off);
    const auto deadline = Date_t::now() + Seconds(30);
    while (!pauseReturned.load()) {
        ASSERT_LT(Date_t::now(), deadline)
            << "pauseCheckpointing() did not return after the in-progress checkpoint finished";
        sleepmillis(5);
    }

    pauser.join();
    checkpointer.resumeCheckpointing();
    checkpointer.shutdown({ErrorCodes::ShutdownInProgress, "test complete"});
}

}  // namespace
}  // namespace mongo
