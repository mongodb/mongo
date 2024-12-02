/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include <list>
#include <memory>

#include <grpc/support/time.h>
#include <grpcpp/alarm.h>
#include <grpcpp/completion_queue.h>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/baton.h"
#include "mongo/platform/atomic.h"
#include "mongo/platform/rwmutex.h"
#include "mongo/stdx/mutex.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/util/duration.h"
#include "mongo/util/executor_stats.h"
#include "mongo/util/future.h"
#include "mongo/util/time_support.h"

namespace mongo::transport::grpc {

class GRPCReactor;

class GRPCReactorTimer : public ReactorTimer {
public:
    GRPCReactorTimer(std::weak_ptr<GRPCReactor> reactor) : _reactor(reactor) {}

    ~GRPCReactorTimer() {
        cancel();
    }

    void cancel(const BatonHandle& baton = nullptr) {
        if (_alarm) {
            _alarm->Cancel();
        }
    };

    Future<void> waitUntil(Date_t deadline, const BatonHandle& baton = nullptr);

private:
    std::shared_ptr<::grpc::Alarm> _alarm;
    std::weak_ptr<GRPCReactor> _reactor;
};

/**
 * The GRPCReactor owns and polls on the gRPC completion queue
 * (https://grpc.github.io/grpc/cpp/classgrpc_1_1_completion_queue.html), which drives asynchronous
 * work in gRPC. The application requests notification for the completion of asynchronous events by
 * emplacing tags on the completion queue, which will be provided to the cq.Next() function when the
 * corresponding event has been completed or cancelled.
 *
 * Arbitrary tasks are scheduled by setting a grpc::Alarm with an immediate deadline. Networking
 * tasks are scheduled by the ClientAsyncReaderWriter stream type, which is associated with a
 * completion queue on construction and notifies CompletionQueueEntry tags that are provided in
 * calls to startCall/read/write/finish.
 */
class GRPCReactor : public Reactor, public std::enable_shared_from_this<GRPCReactor> {
public:
    // The following classes are friends because they must be allowed to request notification for
    // the completion of async work via the completion queue. However, this functionality is not
    // a part of the public Reactor API.
    friend class GRPCReactorTimer;
    friend class StubFactoryImpl;
    friend class EgressSession;
    friend class MockClientStream;

    /**
     * The CompletionQueueEntry is the tag type we provide to gRPC functions. It contains a Promise
     * that will be fulfilled once the tag has been notified via the completion queue. All work
     * chained to the corresponding Future will run on the reactor thread.
     */
    class CompletionQueueEntry {
        /**
         * CompletionQueueEntry tags must be constructed from the
         * GRPCReactor::_registerCompletionQueueEntry function.
         */
        struct Passkey {
            explicit Passkey() = default;
        };

    public:
        friend class GRPCReactor;
        friend class MockClientStream;

        CompletionQueueEntry() = delete;
        CompletionQueueEntry(Passkey, Promise<void> promise) : _promise(std::move(promise)) {}

    private:
        /**
         * This function will fulfill the Promise associated with a tag, but does not remove it from
         * the _cqTaskStash. This is a workaround to introducing a mock grpc::CompletionQueue that
         * the MockClientStream interacts with, because at the MockClientStream layer we have no
         * access to which reactor this tag is associated with and just care about filling the
         * promise.
         */
        void _setPromise_forTest(Status s) {
            _promise.setFrom(s);
        }

        Promise<void> _promise;
        std::list<std::unique_ptr<CompletionQueueEntry>>::iterator _iter;
    };

    GRPCReactor() : _clkSource(this), _stats(&_clkSource), _cq() {}

    void run() noexcept override;

    /**
     * Once stop() is called, all calls to schedule() will fail the task with a ShutdownInProgress
     * error. Similarly, calls to waitUntil on any timers associated with this reactor will emplace
     * the Future with a ShutdownInProgress error.
     */
    void stop() override;

    void drain() override;

    void schedule(Task task) override;

    std::unique_ptr<ReactorTimer> makeTimer() override {
        return std::make_unique<GRPCReactorTimer>(weak_from_this());
    }

    Date_t now() override {
        return Date_t(::grpc::Timespec2Timepoint(gpr_now(::gpr_clock_type::GPR_CLOCK_REALTIME)));
    }

    void appendStats(BSONObjBuilder& bob) const override;

private:
    ::grpc::CompletionQueue* _getCompletionQueue() {
        return &_cq;
    }

    /**
     * Creates a CompletionQueueEntry and registers it in the _cqTaskStash to ensure proper lifetime
     * management of the tag. Callers that require a tag to provide to gRPC functions must create it
     * using this function.
     */
    CompletionQueueEntry* _registerCompletionQueueEntry(Promise<void> p);

    Future<void> _setAlarm(std::shared_ptr<::grpc::Alarm> alarm, gpr_timespec deadline);

    void _processCompletionQueueNotification(void* tag, bool ok);

    bool _inShutdown() {
        auto readLock = _shutdownMutex.readLock();
        return _inShutdownFlag;
    }

    ReactorClockSource _clkSource;
    ExecutorStats _stats;

    WriteRarelyRWMutex _shutdownMutex;
    ::grpc::CompletionQueue _cq;
    bool _inShutdownFlag = false;

    stdx::mutex _taskMutex;
    std::list<std::unique_ptr<CompletionQueueEntry>> _cqTaskStash;
};
}  // namespace mongo::transport::grpc
