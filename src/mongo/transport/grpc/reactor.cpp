// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/transport/grpc/reactor.h"

#include "mongo/base/error_codes.h"
#include "mongo/logv2/log.h"
#include "mongo/util/future_util.h"

#include <mutex>

#include <grpcpp/support/time.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork

namespace mongo::transport::grpc {

static const gpr_timespec kImmediateDeadline = gpr_timespec{0, 0, GPR_TIMESPAN};

Future<void> GRPCReactorTimer::waitUntil(Date_t deadline, const BatonHandle& baton) {
    std::shared_ptr<GRPCReactor> reactor = _reactor.lock();
    if (!reactor) {
        return Future<void>::makeReady(
            Status(ErrorCodes::ShutdownInProgress,
                   "The reactor associated with this timer has been destroyed"));
    }

    // Cancel the previous timer.
    cancel();

    // Make a new alarm.
    _alarm = std::make_shared<::grpc::Alarm>();

    return reactor->_setAlarm(_alarm, ::grpc::TimePoint(deadline.toSystemTimePoint()).raw_time());
};

void GRPCReactor::run() {
    ThreadIdGuard threadIdGuard(this);
    void* tag;
    bool ok = false;

    while (MONGO_likely(!_inShutdown())) {
        if (MONGO_likely(_cq.Next(&tag, &ok))) {
            _processCompletionQueueNotification(tag, ok);
        } else {
            break;
        }
    }
}

void GRPCReactor::stop() {
    auto writeLock = _shutdownMutex.writeLock();
    if (_inShutdownFlag) {
        return;
    }

    _inShutdownFlag = true;

    // Schedule an empty task to wake up and exit the reactor polling thread in run().
    auto pf = makePromiseFuture<void>();
    std::shared_ptr<::grpc::Alarm> alarm = std::make_shared<::grpc::Alarm>();
    alarm->Set(&_cq, kImmediateDeadline, _registerCompletionQueueEntry(std::move(pf.promise)));
    std::move(pf.future).getAsync([anchor = std::move(alarm)](Status s) {});

    // Attempting to schedule onto the reactor (for example, through the Alarm or through the
    // ClientAsyncReaderWriter on the EgressSession) after Shutdown() is called will crash the
    // process.
    _cq.Shutdown();
}

void GRPCReactor::drain() {
    ThreadIdGuard threadIdGuard(this);
    void* tag;
    bool ok = false;

    invariant(_inShutdown());

    // Block until tasks are drained.
    while (_cq.Next(&tag, &ok)) {
        _processCompletionQueueNotification(tag, ok);
    }

    std::lock_guard lk(_taskMutex);
    invariant(_cqTaskStash.size() == 0, "GRPCReactor did not properly drain all tasks");
}

void GRPCReactor::schedule(Task task) {
    // GRPC does not have an API for arbitrary task scheduling, and using the Alarm with an
    // immediate deadline is a workaround for this documented by gRPC.
    _setAlarm(std::make_shared<::grpc::Alarm>(), kImmediateDeadline)
        .getAsync([task = _stats.wrapTask(std::move(task))](Status s) { task(s); });
}

void GRPCReactor::appendStats(BSONObjBuilder& bob, bool forServerStatus) const {
    _stats.serialize(&bob);
}

GRPCReactor::CompletionQueueEntry* GRPCReactor::_registerCompletionQueueEntry(Promise<void> p) {
    auto cqTask =
        std::make_unique<CompletionQueueEntry>(CompletionQueueEntry::Passkey(), std::move(p));

    std::lock_guard lg(_taskMutex);
    auto iter = _cqTaskStash.insert(_cqTaskStash.end(), std::move(cqTask));
    _cqTaskStash.back()->_iter = iter;
    return _cqTaskStash.back().get();
}

Future<void> GRPCReactor::_setAlarm(std::shared_ptr<::grpc::Alarm> alarm, gpr_timespec deadline) {
    auto pf = makePromiseFuture<void>();
    {
        auto readLock = _shutdownMutex.readLock();
        if (!_inShutdownFlag) {
            alarm->Set(&_cq, deadline, _registerCompletionQueueEntry(std::move(pf.promise)));
            return std::move(pf.future).tap([anchor = std::move(alarm)]() {});
        }
    }
    return Future<void>::makeReady(Status(ErrorCodes::ShutdownInProgress, "Shutdown in progress"));
}

void GRPCReactor::_processCompletionQueueNotification(void* tag, bool ok) {
    auto cqTask = static_cast<CompletionQueueEntry*>(tag);
    if (ok) {
        cqTask->_promise.emplaceValue();
    } else {
        cqTask->_promise.setError(
            {ErrorCodes::CallbackCanceled, "Completion queue task did not execute"});
    }

    std::lock_guard lg(_taskMutex);
    _cqTaskStash.erase(cqTask->_iter);
}

}  // namespace mongo::transport::grpc
