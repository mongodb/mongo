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

#include <grpcpp/support/time.h>

#include "mongo/base/error_codes.h"
#include "mongo/logv2/log.h"
#include "mongo/transport/grpc/reactor.h"

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

    invariant(_cqTaskStash.size() == 0, "GRPCReactor did not properly drain all tasks");
}

void GRPCReactor::schedule(Task task) {
    // GRPC does not have an API for arbitrary task scheduling, and using the Alarm with an
    // immediate deadline is a workaround for this documented by gRPC.
    _setAlarm(std::make_shared<::grpc::Alarm>(), kImmediateDeadline)
        .getAsync([task = _stats.wrapTask(std::move(task))](Status s) { task(s); });
}

void GRPCReactor::appendStats(BSONObjBuilder& bob) const {
    _stats.serialize(&bob);
}

GRPCReactor::CompletionQueueEntry* GRPCReactor::_registerCompletionQueueEntry(Promise<void> p) {
    auto cqTask =
        std::make_unique<CompletionQueueEntry>(CompletionQueueEntry::Passkey(), std::move(p));

    stdx::lock_guard lg(_taskMutex);
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

    stdx::lock_guard lg(_taskMutex);
    _cqTaskStash.erase(cqTask->_iter);
}

}  // namespace mongo::transport::grpc
