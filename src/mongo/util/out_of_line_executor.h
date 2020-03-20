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

#pragma once

#include "mongo/base/status.h"
#include "mongo/util/functional.h"

namespace mongo {

/**
 * RunOnceGuard promises that it its run() function is invoked exactly once.
 *
 * When a RunOnceGuard is constructed, it marks itself as armed. When a RunOnceGuard is moved from,
 * it is marked as done. When the RunOnceGuard is destructed, it invariants that it is finished.
 *
 * The RunOnceGuard is intended to provide an unsynchronized way to validate that a unit of work was
 * actually consumed. It can be bound into lambdas or be constructed as a default member of
 * parameter objects in work queues or maps.
 */
class RunOnceGuard {
    enum class State {
        kDone,
        kArmed,
    };

public:
    static constexpr const char kRanNeverStr[] = "Function never ran";
    static constexpr const char kRanTwiceStr[] = "Function ran a second time";

    constexpr RunOnceGuard() : _state{State::kArmed} {}
    ~RunOnceGuard() {
        invariant(_state == State::kDone, kRanNeverStr);
    }

    RunOnceGuard(RunOnceGuard&& other) : _state{std::exchange(other._state, State::kDone)} {}
    RunOnceGuard& operator=(RunOnceGuard&& other) noexcept {
        invariant(_state == State::kDone, kRanNeverStr);
        _state = std::exchange(other._state, State::kDone);
        return *this;
    }

    RunOnceGuard(const RunOnceGuard&) = delete;
    RunOnceGuard& operator=(const RunOnceGuard&) = delete;

    void run() noexcept {
        invariant(_state == State::kArmed, kRanTwiceStr);
        _state = State::kDone;
    }

private:
    State _state;
};

/**
 * Provides the minimal api for a simple out of line executor that can run non-cancellable
 * callbacks.
 *
 * The contract for scheduling work on an executor is that it never blocks the caller.  It doesn't
 * necessarily need to offer forward progress guarantees, but actual calls to schedule() should not
 * deadlock.
 *
 * If you manage the lifetime of your executor using a shared_ptr, you can begin a chain of
 * execution like this:
 *      ExecutorFuture(myExec)
 *          .then([] { return doThing1(); })
 *          .then([] { return doThing2(); })
 *          ...
 */
class OutOfLineExecutor {
public:
    using Task = unique_function<void(Status)>;

    static constexpr const char kRejectedWorkStr[] = "OutOfLineExecutor rejected work";
    static constexpr const char kNoExecutorStr[] = "Invalid OutOfLineExecutor provided";

public:
    /**
     * Delegates invocation of the Task to this executor
     *
     * Execution of the Task can happen in one of three contexts:
     * * By default, on an execution context maintained by the OutOfLineExecutor (i.e. a thread).
     * * During shutdown, on the execution context of shutdown/join/dtor for the OutOfLineExecutor.
     * * Post-shutdown, on the execution context of the calling code.
     *
     * The Task will be passed a Status schedStatus that is either:
     * * schedStatus.isOK() if the function is run in an out-of-line context
     * * isCancelationError(schedStatus.code()) if the function is run in an inline context
     *
     * All of this is to say: CHECK YOUR STATUS.
     */
    virtual void schedule(Task func) = 0;

    virtual ~OutOfLineExecutor() = default;
};

using ExecutorPtr = std::shared_ptr<OutOfLineExecutor>;

/**
 * A GuaranteedExecutor is a wrapper that ensures its Tasks run exactly once.
 *
 * If a Task cannot be run, would be destructed without being run, or would run multiple times, it
 * will trigger an invariant.
 */
class GuaranteedExecutor final : public OutOfLineExecutor {
public:
    explicit GuaranteedExecutor(ExecutorPtr exec) : _exec(std::move(exec)) {
        invariant(_exec, kNoExecutorStr);
    }

    virtual ~GuaranteedExecutor() = default;

    /**
     * Return a wrapped task that is enforced to run once and only once.
     */
    static auto enforceRunOnce(Task&& task) noexcept {
        return Task([task = std::move(task), guard = RunOnceGuard()](Status status) mutable {
            invariant(status, kRejectedWorkStr);
            guard.run();

            auto localTask = std::exchange(task, {});
            localTask(std::move(status));
        });
    }

    void schedule(Task func) override {
        // Make sure that the function will be called eventually, once.
        auto sureFunc = enforceRunOnce(std::move(func));
        _exec->schedule(std::move(sureFunc));
    }

private:
    ExecutorPtr _exec;
};

/**
 * A GuaranteedExecutorWithFallback is a wrapper that allows a preferred Executor to pass tasks to a
 * fallback.
 *
 * The GuaranteedExecutorWithFallback uses its _fallback executor when _preferred invokes a Task
 * with a not-okay Status. The _fallback executor is a GuaranteedExecutor wrapper, and thus must run
 * Tasks under threat of invariant.
 */
class GuaranteedExecutorWithFallback final : public OutOfLineExecutor {
public:
    explicit GuaranteedExecutorWithFallback(ExecutorPtr preferred, ExecutorPtr fallback)
        : _preferred(std::move(preferred)), _fallback(std::move(fallback)) {
        invariant(_preferred, kNoExecutorStr);
        // Fallback invariants via GuaranteedExecutor's constructor.
    }

    virtual ~GuaranteedExecutorWithFallback() = default;

    void schedule(Task func) override {
        _preferred->schedule([func = std::move(func), fallback = _fallback](Status status) mutable {
            if (!status.isOK()) {
                // This executor has rejected work, send it to the fallback.
                fallback.schedule(std::move(func));
                return;
            }

            // This executor has accepted work.
            func(std::move(status));
        });
    }

private:
    ExecutorPtr _preferred;
    GuaranteedExecutor _fallback;
};

/**
 * Make a GuaranteedExecutor without a fallback.
 *
 * If exec is invalid, this function will invariant.
 */
inline ExecutorPtr makeGuaranteedExecutor(ExecutorPtr exec) noexcept {
    // Note that each GuaranteedExecutor ctor invariants that the pointer is valid.
    return std::make_shared<GuaranteedExecutor>(std::move(exec));
}

/**
 * Make either a GuaranteedExecutor or a GuaranteedExecutorWithFallback.
 *
 * If preferred is invalid and fallback is valid, this creates a GuaranteedExecutor from fallback.
 * If fallback is invalid and preferred is valid, this creates a GuaranteedExecutor from preferred.
 * If both preferred and fallback are invalid, this function will invariant.
 */
inline ExecutorPtr makeGuaranteedExecutor(ExecutorPtr preferred, ExecutorPtr fallback) noexcept {
    // Note that each GuaranteedExecutor ctor invariants that the pointer is valid.
    if (!preferred) {
        return makeGuaranteedExecutor(std::move(fallback));
    }

    if (!fallback) {
        return makeGuaranteedExecutor(std::move(preferred));
    }

    return std::make_shared<GuaranteedExecutorWithFallback>(std::move(preferred),
                                                            std::move(fallback));
}

}  // namespace mongo
