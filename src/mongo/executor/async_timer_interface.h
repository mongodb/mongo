// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/time_support.h"

#include <functional>
#include <system_error>

namespace mongo {
namespace executor {

/**
 * An asynchronous waitable timer interface.
 */
class AsyncTimerInterface {
    AsyncTimerInterface(const AsyncTimerInterface&) = delete;
    AsyncTimerInterface& operator=(const AsyncTimerInterface&) = delete;

public:
    virtual ~AsyncTimerInterface() = default;

    using Handler = std::function<void(std::error_code)>;

    /**
     * Cancel any asynchronous operations waiting on this timer, invoking
     * their handlers immediately with an 'operation aborted' error code.
     *
     * If the timer has already expired when cancel() is called, the handlers
     * for asyncWait operations may have already fired or been enqueued to
     * fire soon, in which case we cannot cancel them.
     *
     * Calling cancel() does not change this timer's expiration time; future
     * calls to asyncWait() will schedule callbacks to run as usual.
     */
    virtual void cancel() = 0;

    /**
     * Perform an asynchronous wait on this timer.
     */
    virtual void asyncWait(Handler handler) = 0;

    /**
     * Reset this timer's expiry time relative to now. Any pending asyncWait operations
     * will be canceled, and their handlers will be invoked with an error code.
     */
    virtual void expireAfter(Milliseconds expiration) = 0;

protected:
    AsyncTimerInterface() = default;
};

/**
 * A factory for AsyncTimers.
 */
class AsyncTimerFactoryInterface {
    AsyncTimerFactoryInterface(const AsyncTimerFactoryInterface&) = delete;
    AsyncTimerFactoryInterface& operator=(const AsyncTimerFactoryInterface&) = delete;

public:
    virtual ~AsyncTimerFactoryInterface() = default;

    virtual std::unique_ptr<AsyncTimerInterface> make(Milliseconds expiration) = 0;

    virtual Date_t now() = 0;

protected:
    AsyncTimerFactoryInterface() = default;
};

}  // namespace executor
}  // namespace mongo
