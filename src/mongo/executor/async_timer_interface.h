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
