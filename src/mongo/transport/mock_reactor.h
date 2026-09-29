// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/duration.h"
#include "mongo/util/modules.h"
#include "mongo/util/time_support.h"

#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <vector>

namespace mongo::transport {

/**
 * A Reactor whose timers are alarms on the process-global mock clock (see ClockSourceMock), for
 * unit-testing Reactor-based code without real threads or wall-clock waits.
 */
class [[MONGO_MOD_PUBLIC]] MockReactor : public Reactor {
public:
    MockReactor() = default;

    /** Fails pending waits with CallbackCanceled; this reactor's timers stay safe to use. */
    ~MockReactor() override;

    // Reactor interface.
    void run() override;
    void stop() override;

    /** No-op, since schedule() runs tasks inline. */
    void drain() override;

    /** Runs `task` inline as the reactor thread, with Status::OK() even after stop(). */
    void schedule(Task) override;

    /**
     * Timers fire, as the reactor thread, once any ClockSourceMock reaches their deadline. A wait
     * that is cancelled, re-armed, or orphaned by ~MockReactor leaves its alarm in the global clock
     * queue until then, where firing it is a no-op.
     */
    std::unique_ptr<ReactorTimer> makeTimer() override;

    std::chrono::system_clock::time_point systemTime() override;
    void appendStats(BSONObjBuilder& bob, bool forServerStatus = false) const override {}

    // Test controls.

    /** Advances the shared mock clock, firing due alarms in deadline order. */
    void advanceTime(Milliseconds);

    /** Number of timers that are armed on this reactor and not yet fired or cancelled. */
    size_t pendingTimerCount() const;

    bool isStopped() const;

    /** Interchangeable with any other ClockSourceMock, as all share one process-global time. */
    ClockSourceMock& clock() {
        return _clock;
    }

private:
    friend class MockReactorTimer;

    /**
     * One outstanding waitUntil(). Settled at most once, by whichever of the alarm action,
     * cancel(), or the reactor destructor gets there first.
     */
    struct WaitState;

    /**
     * Registers `state` as pending on this reactor and arms a clock alarm for `deadline` that
     * fulfils it. If the deadline has already passed the alarm fires before this returns.
     */
    void _arm(Date_t deadline, std::shared_ptr<WaitState> state);

    /** Runs `fn` with onReactorThread() true, tolerating re-entrancy from this reactor's tasks. */
    template <typename F>
    void _runAsReactorThread(F&& fn);

    ClockSourceMock _clock;

    mutable std::mutex _mutex;
    stdx::condition_variable _stoppedCV;
    bool _stopped = false;
    // Waits armed on this reactor; settled or expired entries are pruned on the next _arm().
    std::vector<std::weak_ptr<WaitState>> _waits;
};

}  // namespace mongo::transport
