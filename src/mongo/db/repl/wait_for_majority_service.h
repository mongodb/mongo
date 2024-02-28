/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include <boost/move/utility_core.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <boost/utility/in_place_factory.hpp>  // IWYU pragma: keep
#include <map>
#include <memory>
#include <utility>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/db/client_strand.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/service_context.h"
#include "mongo/executor/task_executor.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/mutex.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/future.h"
#include "mongo/util/future_impl.h"

namespace mongo {

namespace detail {

class AsyncConditionVariable {
public:
    AsyncConditionVariable() : _current(boost::in_place()) {}

    SemiFuture<void> onNotify() {
        stdx::lock_guard lk(_mutex);
        return _current->getFuture().semi();
    }

    void notifyAllAndReset() {
        stdx::lock_guard lk(_mutex);
        if (_inShutdown) {
            return;
        }
        _current->emplaceValue();
        _current = boost::in_place();
    }

    void notifyAllAndClose() {
        stdx::lock_guard lk(_mutex);
        if (_inShutdown) {
            return;
        }
        _inShutdown = true;
        _current->emplaceValue();
    }

private:
    mutable Mutex _mutex = MONGO_MAKE_LATCH("AsyncConditionVariable::_mutex");
    boost::optional<SharedPromise<void>> _current;
    bool _inShutdown{false};
};
}  // namespace detail


class WaitForMajorityServiceForReadImpl {
public:
    virtual ~WaitForMajorityServiceForReadImpl();
    void startup(ServiceContext* ctx);
    void shutDown();
    SemiFuture<void> waitUntilMajority(const repl::OpTime& opTime,
                                       const CancellationToken& cancelToken);

private:
    enum class State { kNotStarted, kRunning, kShutdown };
    // State is kNotStarted on construction, kRunning after WaitForMajorityService::startup() has
    // been called, and kShutdown after WaitForMajorityService::shutdown() has been called. It is
    // illegal to call WaitForMajorityService::waitUntilMajority before calling startup.
    State _state{State::kNotStarted};
    /**
     * Internal representation of an individual request to wait on some particular optime.
     */
    struct Request {
        explicit Request(Promise<void> promise)
            : hasBeenProcessed{false}, result(std::move(promise)) {}
        AtomicWord<bool> hasBeenProcessed;
        Promise<void> result;
    };

    using OpTimeWaitingMap = std::multimap<repl::OpTime, std::shared_ptr<Request>>;

    /**
     * Periodically checks the list of opTimes to wait for majority committed.
     */
    SemiFuture<void> _periodicallyWaitForMajority();

    virtual Status _waitForOpTime(OperationContext* opCtx, const repl::OpTime& opTime);

    // The pool of threads available to wait on opTimes and cancel existing requests.
    std::shared_ptr<ThreadPool> _pool;

    // This future is completed when the service has finished all of its work and is ready for
    // shutdown.
    boost::optional<SemiFuture<void>> _backgroundWorkComplete;

    // Manages the Client responsible for the thread that waits on opTimes.
    ClientStrandPtr _waitForMajorityClient;

    // Manages the Client responsible for the thread that cancels existing requests to wait on
    // opTimes.
    ClientStrandPtr _waitForMajorityCancellationClient;

    // This mutex synchronizes access to the members declared below.
    Mutex _mutex = MONGO_MAKE_LATCH("WaitForMajorityService::_mutex");

    // Contains an ordered list of opTimes to wait to be majority comitted.
    OpTimeWaitingMap _queuedOpTimes;

    // Contains the last opTime that the background thread was able to successfully wait to be
    // majority comitted.
    repl::OpTime _lastOpTimeWaited;

    // Use for signalling new opTime requests being queued.
    detail::AsyncConditionVariable _hasNewOpTimeCV;
};

class WaitForMajorityServiceForWriteImpl {
public:
    SemiFuture<void> waitUntilMajority(ServiceContext* service,
                                       const repl::OpTime& opTime,
                                       const CancellationToken& cancelToken);
};

/**
 * Provides a facility for asynchronously waiting a local opTime to be majority committed.
 */

class WaitForMajorityService {
public:
    ~WaitForMajorityService();

    static WaitForMajorityService& get(ServiceContext* service);

    /**
     * Sets up the background thread pool responsible for waiting for opTimes to be majority
     * committed.
     */
    void startup(ServiceContext* ctx);

    /**
     * Blocking method, which shuts down and joins the background thread.
     */
    void shutDown();

    /**
     * Enqueue a request to wait for the given opTime to be majority committed.
     */
    SemiFuture<void> waitUntilMajorityForRead(const repl::OpTime& opTime,
                                              const CancellationToken& cancelToken);
    /**
     * Wait until the given opTime to be majority committed for writes.
     */
    SemiFuture<void> waitUntilMajorityForWrite(ServiceContext* service,
                                               const repl::OpTime& opTime,
                                               const CancellationToken& cancelToken);

private:
    WaitForMajorityServiceForReadImpl _readService;
    WaitForMajorityServiceForWriteImpl _writeService;
};

}  // namespace mongo
