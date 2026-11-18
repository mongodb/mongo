// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/baton.h"
#include "mongo/platform/atomic.h"
#include "mongo/platform/rwmutex.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/util/duration.h"
#include "mongo/util/executor_stats.h"
#include "mongo/util/future.h"
#include "mongo/util/time_support.h"

#include <chrono>
#include <list>
#include <memory>
#include <mutex>

#include <grpc/support/time.h>
#include <grpcpp/alarm.h>
#include <grpcpp/completion_queue.h>

namespace mongo::transport::grpc {

class GRPCReactor;

class GRPCReactorTimer : public ReactorTimer {
public:
    GRPCReactorTimer(std::shared_ptr<Reactor> reactor)
        : _reactor(dynamic_pointer_cast<GRPCReactor>(reactor)) {}

    ~GRPCReactorTimer() override {
        cancel();
    }

    void cancel(const BatonHandle& baton = nullptr) override {
        if (_alarm) {
            _alarm->Cancel();
        }
    };

    Future<void> waitUntil(Date_t deadline, const BatonHandle& baton = nullptr) override;

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
class GRPCReactor : public Reactor {
public:
    // The following classes are friends because they must be allowed to request notification for
    // the completion of async work via the completion queue. However, this functionality is not
    // a part of the public Reactor API.
    friend class GRPCReactorTimer;
    friend class StubFactoryImpl;
    friend class EgressSession;
    friend class MockClientStream;
    friend class Client;
    friend class Channel;

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

        CompletionQueueEntry() = delete;
        CompletionQueueEntry(Passkey, Promise<void> promise) : _promise(std::move(promise)) {}

    private:
        Promise<void> _promise;
        std::list<std::unique_ptr<CompletionQueueEntry>>::iterator _iter;
    };

    GRPCReactor() : _tickSource(this), _stats(&_tickSource), _cq() {}

    void run() override;

    /**
     * Once stop() is called, all calls to schedule() will fail the task with a ShutdownInProgress
     * error. Similarly, calls to waitUntil on any timers associated with this reactor will emplace
     * the Future with a ShutdownInProgress error.
     */
    void stop() override;

    void drain() override;

    void schedule(Task task) override;

    std::unique_ptr<ReactorTimer> makeTimer() override {
        return std::make_unique<GRPCReactorTimer>(shared_from_this());
    }

    std::chrono::system_clock::time_point systemTime() override {
        return ::grpc::Timespec2Timepoint(gpr_now(::gpr_clock_type::GPR_CLOCK_REALTIME));
    }

    void appendStats(BSONObjBuilder& bob, bool forServerStatus) const override;

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

    ReactorTickSource _tickSource;
    ExecutorStats _stats;

    WriteRarelyRWMutex _shutdownMutex;
    ::grpc::CompletionQueue _cq;
    bool _inShutdownFlag = false;

    std::mutex _taskMutex;
    std::list<std::unique_ptr<CompletionQueueEntry>> _cqTaskStash;
};
}  // namespace mongo::transport::grpc
