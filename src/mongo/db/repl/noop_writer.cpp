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


#include "mongo/platform/basic.h"

#include <functional>

#include "mongo/db/commands.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/curop.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/repl/noop_writer.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/util/concurrency/idle_thread_block.h"
#include "mongo/util/testing_proctor.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication


namespace mongo {
namespace repl {

namespace {

const auto kMsgObj = BSON("msg"
                          << "periodic noop");

}  // namespace


/**
 *  Runs the noopWrite argument with waitTime period until its destroyed.
 */
class NoopWriter::PeriodicNoopRunner {
    PeriodicNoopRunner(const PeriodicNoopRunner&) = delete;
    PeriodicNoopRunner& operator=(const PeriodicNoopRunner&) = delete;

    using NoopWriteFn = std::function<void(OperationContext*)>;

public:
    PeriodicNoopRunner(Seconds waitTime, NoopWriteFn noopWrite)
        : _thread([this, noopWrite, waitTime] { run(waitTime, std::move(noopWrite)); }) {}

    ~PeriodicNoopRunner() {
        stdx::unique_lock<Latch> lk(_mutex);
        _inShutdown = true;
        _cv.notify_all();
        lk.unlock();
        _thread.join();
    }

private:
    void run(Seconds waitTime, NoopWriteFn noopWrite) {
        Client::initThread("NoopWriter");
        while (true) {
            const ServiceContext::UniqueOperationContext opCtxPtr = cc().makeOperationContext();
            OperationContext& opCtx = *opCtxPtr;
            {
                stdx::unique_lock<Latch> lk(_mutex);
                MONGO_IDLE_THREAD_BLOCK;
                _cv.wait_for(lk, waitTime.toSystemDuration(), [&] { return _inShutdown; });

                if (_inShutdown)
                    return;
            }
            noopWrite(&opCtx);
        }
    }

    /**
     *  Indicator that thread is shutting down.
     */
    bool _inShutdown{false};

    /**
     *  Mutex for the CV
     */
    Mutex _mutex = MONGO_MAKE_LATCH("PeriodicNoopRunner::_mutex");

    /**
     * CV to wait for.
     */
    stdx::condition_variable _cv;

    /**
     * Thread that runs the tasks. Must be last so all other members are initialized before
     * starting.
     */
    stdx::thread _thread;
};

NoopWriter::NoopWriter(Seconds writeInterval) : _writeInterval(writeInterval) {
    uassert(ErrorCodes::BadValue, "write interval must be positive", writeInterval > Seconds(0));
}

NoopWriter::~NoopWriter() {
    stopWritingPeriodicNoops();
}

Status NoopWriter::startWritingPeriodicNoops(OpTime lastKnownOpTime) {
    stdx::lock_guard<Latch> lk(_mutex);
    _lastKnownOpTime = lastKnownOpTime;

    invariant(!_noopRunner);
    _noopRunner =
        std::make_unique<PeriodicNoopRunner>(_writeInterval, [this](OperationContext* opCtx) {
            opCtx->setShouldParticipateInFlowControl(false);
            _writeNoop(opCtx);
        });
    return Status::OK();
}

void NoopWriter::stopWritingPeriodicNoops() {
    stdx::lock_guard<Latch> lk(_mutex);
    _noopRunner.reset();
}

void NoopWriter::_writeNoop(OperationContext* opCtx) {
    // Use GlobalLock instead of DBLock to allow return when the lock is not available. It may
    // happen when the primary steps down and a shared global lock is acquired.
    Lock::GlobalLock lock(
        opCtx, MODE_IX, Date_t::now() + Milliseconds(1), Lock::InterruptBehavior::kLeaveUnlocked);
    if (!lock.isLocked()) {
        LOGV2_DEBUG(21219, 1, "Global lock is not available, skipping the noop write");
        return;
    }

    auto replCoord = ReplicationCoordinator::get(opCtx);
    // Its a proxy for being a primary
    if (!replCoord->canAcceptWritesForDatabase(opCtx, "admin")) {
        LOGV2_DEBUG(21220, 1, "Not a primary, skipping the noop write");
        return;
    }

    auto lastAppliedOpTime = replCoord->getMyLastAppliedOpTime();

    // _lastKnownOpTime is not protected by lock as its used only by one thread.
    if (lastAppliedOpTime != _lastKnownOpTime) {
        LOGV2_DEBUG(21221,
                    1,
                    "Not scheduling a noop write. Last known OpTime: {lastKnownOpTime} != last "
                    "primary OpTime: {lastAppliedOpTime}",
                    "Not scheduling a noop write. Last known OpTime != last primary OpTime",
                    "lastKnownOpTime"_attr = _lastKnownOpTime,
                    "lastAppliedOpTime"_attr = lastAppliedOpTime);
    } else {
        if (writePeriodicNoops.load()) {
            const auto logLevel = TestingProctor::instance().isEnabled() ? 0 : 1;
            LOGV2_DEBUG(21222,
                        logLevel,
                        "Writing noop to oplog as there has been no writes to this replica set in "
                        "over {writeInterval}",
                        "Writing noop to oplog as there has been no writes to this replica set "
                        "within write interval",
                        "writeInterval"_attr = _writeInterval);
            writeConflictRetry(
                opCtx, "writeNoop", NamespaceString::kRsOplogNamespace.ns(), [&opCtx] {
                    WriteUnitOfWork uow(opCtx);
                    opCtx->getClient()->getServiceContext()->getOpObserver()->onOpMessage(opCtx,
                                                                                          kMsgObj);
                    uow.commit();
                });
        }
    }

    _lastKnownOpTime = replCoord->getMyLastAppliedOpTime();
    LOGV2_DEBUG(21223,
                1,
                "Set last known op time to {lastKnownOpTime}",
                "Set last known op time",
                "lastKnownOpTime"_attr = _lastKnownOpTime);
}

}  // namespace repl
}  // namespace mongo
