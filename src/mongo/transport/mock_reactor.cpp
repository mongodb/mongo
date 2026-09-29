// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/transport/mock_reactor.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/future.h"

#include <algorithm>
#include <string>
#include <utility>

namespace mongo::transport {

struct MockReactor::WaitState {
    explicit WaitState(Promise<void> promise_) : promise(std::move(promise_)) {}

    /**
     * Runs `settler` on the promise if nothing has settled it yet; returns whether it did. The flag
     * is flipped under the mutex but the promise is touched outside it, because fulfilling the
     * promise runs continuations inline and those may call back into cancel() on this same state.
     */
    template <typename F>
    bool settle(F&& settler) {
        {
            std::lock_guard lk(mutex);
            if (settled) {
                return false;
            }
            settled = true;
        }
        settler(promise);
        return true;
    }

    void fulfil() {
        settle([](Promise<void>& p) { p.emplaceValue(); });
    }

    void fail(std::string reason) {
        settle([&](Promise<void>& p) {
            p.setError(Status(ErrorCodes::CallbackCanceled, std::move(reason)));
        });
    }

    bool isSettled() const {
        std::lock_guard lk(mutex);
        return settled;
    }

    mutable std::mutex mutex;
    bool settled = false;
    Promise<void> promise;
};

/**
 * A ReactorTimer with at most one outstanding wait. The wait itself lives in a WaitState shared
 * with the clock alarm that fulfils it and with the owning reactor, so that any of the three can
 * settle it exactly once.
 */
class MockReactorTimer final : public ReactorTimer {
public:
    explicit MockReactorTimer(std::weak_ptr<MockReactor> reactor) : _reactor(std::move(reactor)) {}

    ~MockReactorTimer() override {
        cancel();
    }

    void cancel(const BatonHandle& = nullptr) override {
        std::shared_ptr<MockReactor::WaitState> state;
        {
            std::lock_guard lk(_mutex);
            state = std::move(_state);
        }
        if (state) {
            state->fail("MockReactor timer was cancelled");
        }
    }

    Future<void> waitUntil(Date_t deadline, const BatonHandle&) override {
        auto reactor = _reactor.lock();
        if (!reactor || reactor->isStopped()) {
            return Future<void>::makeReady(
                Status(ErrorCodes::ShutdownInProgress,
                       "The MockReactor associated with this timer has been shut down"));
        }

        // At most one outstanding wait per timer.
        cancel();

        auto pf = makePromiseFuture<void>();
        auto state = std::make_shared<MockReactor::WaitState>(std::move(pf.promise));
        {
            std::lock_guard lk(_mutex);
            _state = state;
        }
        reactor->_arm(deadline, std::move(state));
        return std::move(pf.future);
    }

private:
    const std::weak_ptr<MockReactor> _reactor;

    std::mutex _mutex;
    std::shared_ptr<MockReactor::WaitState> _state;
};

MockReactor::~MockReactor() {
    // Fail anything still armed so that no waiter is left hanging. The alarm actions still queued
    // in the clock become no-ops because their WaitState is already settled.
    decltype(_waits) waits;
    {
        std::lock_guard lk(_mutex);
        waits = std::move(_waits);
    }
    for (auto&& weak : waits) {
        if (auto state = weak.lock()) {
            state->fail("MockReactor destroyed with timer pending");
        }
    }
}

template <typename F>
void MockReactor::_runAsReactorThread(F&& fn) {
    if (onReactorThread()) {
        fn();
        return;
    }
    ThreadIdGuard guard(this);
    fn();
}

void MockReactor::run() {
    _runAsReactorThread([&] {
        std::unique_lock lk(_mutex);
        _stoppedCV.wait(lk, [&] { return _stopped; });
    });
}

void MockReactor::stop() {
    {
        std::lock_guard lk(_mutex);
        _stopped = true;
    }
    _stoppedCV.notify_all();
}

void MockReactor::drain() {}

void MockReactor::schedule(Task task) {
    _runAsReactorThread([&] { task(Status::OK()); });
}

std::unique_ptr<ReactorTimer> MockReactor::makeTimer() {
    // enable_shared_from_this is on the Reactor base, so downcast the handle for the timer.
    auto self = std::static_pointer_cast<MockReactor>(weak_from_this().lock());
    invariant(self, "MockReactor must be owned by a std::shared_ptr to make timers");
    return std::make_unique<MockReactorTimer>(std::weak_ptr<MockReactor>(self));
}

std::chrono::system_clock::time_point MockReactor::systemTime() {
    return _clock.now().toSystemTimePoint();
}

void MockReactor::advanceTime(Milliseconds diff) {
    // ClockSourceMock fires every alarm that becomes due, in deadline order, before returning.
    _clock.advance(diff);
}

size_t MockReactor::pendingTimerCount() const {
    std::lock_guard lk(_mutex);
    return std::count_if(_waits.begin(), _waits.end(), [](const std::weak_ptr<WaitState>& weak) {
        auto state = weak.lock();
        return state && !state->isSettled();
    });
}

bool MockReactor::isStopped() const {
    std::lock_guard lk(_mutex);
    return _stopped;
}

void MockReactor::_arm(Date_t deadline, std::shared_ptr<WaitState> state) {
    {
        std::lock_guard lk(_mutex);
        std::erase_if(_waits, [](const std::weak_ptr<WaitState>& weak) {
            auto s = weak.lock();
            return !s || s->isSettled();
        });
        _waits.push_back(state);
    }

    // The clock runs the action outside its mutex, either right now if the deadline has passed
    // or during whichever advance() first reaches it. Mirror a real reactor by firing the wait on
    // "the reactor thread".
    _clock.setAlarm(deadline, [weakSelf = weak_from_this(), state = std::move(state)]() mutable {
        if (auto self = std::static_pointer_cast<MockReactor>(weakSelf.lock())) {
            self->_runAsReactorThread([&] { state->fulfil(); });
        }
        // Else ~MockReactor has already failed this wait with CallbackCanceled.
    });
}

}  // namespace mongo::transport
