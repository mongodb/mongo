/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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


#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
// IWYU pragma: no_include "cxxabi.h"
#include <mutex>
#include <utility>

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/control/journal_flusher.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/admission_context.h"
#include "mongo/util/concurrency/idle_thread_block.h"
#include "mongo/util/decorable.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/time_support.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


namespace mongo {

namespace {

const auto getJournalFlusher = ServiceContext::declareDecoration<std::unique_ptr<JournalFlusher>>();

MONGO_FAIL_POINT_DEFINE(pauseJournalFlusherBeforeFlush);
MONGO_FAIL_POINT_DEFINE(pauseJournalFlusherThread);

}  // namespace

JournalFlusher* JournalFlusher::get(ServiceContext* serviceCtx) {
    auto& journalFlusher = getJournalFlusher(serviceCtx);
    invariant(journalFlusher);
    return journalFlusher.get();
}

JournalFlusher* JournalFlusher::get(OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

void JournalFlusher::set(ServiceContext* serviceCtx, std::unique_ptr<JournalFlusher> flusher) {
    auto& journalFlusher = getJournalFlusher(serviceCtx);
    if (journalFlusher) {
        invariant(!journalFlusher->running(),
                  "Tried to reset the JournalFlusher without shutting down the original instance.");
    }

    invariant(flusher);
    journalFlusher = std::move(flusher);
}

void JournalFlusher::run() {
    ThreadClient tc(name(), getGlobalServiceContext()->getService());
    LOGV2_DEBUG(4584701, 1, "starting {name} thread", "name"_attr = name());

    // The thread must not run and access the service context to create an opCtx while unit test
    // infrastructure is still being set up and expects sole access to the service context (there is
    // no conurrency control on the service context during this phase).
    if (_disablePeriodicFlushes) {
        stdx::unique_lock<Latch> lk(_stateMutex);
        _flushJournalNowCV.wait(lk,
                                [&] { return _flushJournalNow || _needToPause || _shuttingDown; });
    }

    auto setUpOpCtx = [&] {
        // Initialize the thread's opCtx.
        _uniqueCtx.emplace(tc->makeOperationContext());

        // Updates to a non-replicated collection, oplogTruncateAfterPoint, are made by this thread.
        // As this operation is critical for data durability we mark it as having Immediate priority
        // to skip ticket and flow control.
        shard_role_details::getLocker(_uniqueCtx->get())
            ->setAdmissionPriority(AdmissionContext::Priority::kImmediate);
    };

    setUpOpCtx();
    while (true) {
        pauseJournalFlusherBeforeFlush.pauseWhileSet();
        try {
            shard_role_details::getRecoveryUnit(_uniqueCtx->get())
                ->waitUntilDurable(_uniqueCtx->get());

            // Signal the waiters that a round completed.
            _currentSharedPromise->emplaceValue();

            // Release snapshot before we start the next round.
            shard_role_details::getRecoveryUnit(_uniqueCtx->get())->abandonSnapshot();
        } catch (const AssertionException& e) {
            {
                // Reset opCtx if we get an error.
                stdx::lock_guard<Latch> lk(_opCtxMutex);
                _uniqueCtx.reset();
                setUpOpCtx();
            }

            // Can be caused by killOp or stepdown.
            if (ErrorCodes::isInterruption(e.code())) {
                // When this thread is interrupted it will immediately restart the journal flush
                // without sending errors to waiting callers. The opCtx error should already be
                // cleared of the interrupt by the ON_BLOCK_EXIT handling above.
                LOGV2_DEBUG(5574501,
                            1,
                            "The JournalFlusher got interrupted, retrying",
                            "error"_attr = e.toString());
                continue;
            }

            // We want to log errors for debugability.
            LOGV2_WARNING(
                6148401,
                "The JournalFlusher encountered an error attempting to flush data to disk",
                "JournalFlusherError"_attr = e.toString());

            // Signal the waiters that the fsync was interrupted.
            _currentSharedPromise->setError(e.toStatus());
        }

        // Wait until either journalCommitIntervalMs passes or an immediate journal flush is
        // requested (or shutdown). If _disablePeriodicFlushes is set, then the thread will not
        // wake up until a journal flush is externally requested.

        auto deadline =
            Date_t::now() + Milliseconds(storageGlobalParams.journalCommitIntervalMs.load());

        stdx::unique_lock<Latch> lk(_stateMutex);

        MONGO_IDLE_THREAD_BLOCK;
        if (_disablePeriodicFlushes || MONGO_unlikely(pauseJournalFlusherThread.shouldFail())) {
            // This is not an ideal solution for the failpoint usage because turning the failpoint
            // off at this point in the code would leave this thread sleeping until explicitly
            // pinged by an async thread to flush the journal.
            _flushJournalNowCV.wait(
                lk, [&] { return _flushJournalNow || _needToPause || _shuttingDown; });
        } else {
            _flushJournalNowCV.wait_until(lk, deadline.toSystemTimePoint(), [&] {
                return _flushJournalNow || _needToPause || _shuttingDown;
            });
        }

        if (_needToPause) {
            _state = States::Paused;
            _stateChangeCV.notify_all();

            _flushJournalNowCV.wait(lk, [&] { return !_needToPause || _shuttingDown; });

            _state = States::Running;
            _stateChangeCV.notify_all();
        }

        _flushJournalNow = false;

        if (_shuttingDown) {
            LOGV2_DEBUG(4584702, 1, "stopping {name} thread", "name"_attr = name());
            invariant(!_shutdownReason.isOK());
            _nextSharedPromise->setError(_shutdownReason);

            _state = States::ShutDown;
            _stateChangeCV.notify_all();

            stdx::lock_guard<Latch> lk(_opCtxMutex);
            _uniqueCtx.reset();
            return;
        }

        // Take the next promise as current and reset the next promise.
        _currentSharedPromise =
            std::exchange(_nextSharedPromise, std::make_unique<SharedPromise<void>>());
    }
}

void JournalFlusher::shutdown(const Status& reason) {
    LOGV2(22320, "Shutting down journal flusher thread");
    {
        stdx::lock_guard<Latch> lk(_stateMutex);
        _shuttingDown = true;
        _shutdownReason = reason;
        _flushJournalNowCV.notify_one();
    }
    wait();
    LOGV2(22321, "Finished shutting down journal flusher thread");
}

void JournalFlusher::pause() {
    LOGV2(5142500, "Pausing journal flusher thread");
    {
        stdx::unique_lock<Latch> lk(_stateMutex);
        _needToPause = true;
        _stateChangeCV.wait(lk,
                            [&] { return _state == States::Paused || _state == States::ShutDown; });
    }
    LOGV2(5142501, "Paused journal flusher thread");
}

void JournalFlusher::resume() {
    LOGV2(5142502, "Resuming journal flusher thread");
    {
        stdx::lock_guard<Latch> lk(_stateMutex);
        _needToPause = false;
        _flushJournalNowCV.notify_one();
    }
    LOGV2(5142503, "Resumed journal flusher thread");
}

void JournalFlusher::waitForJournalFlush(Interruptible* interruptible) {
    auto myFuture = [&]() {
        stdx::lock_guard<Latch> lk(_stateMutex);
        _triggerJournalFlush(lk);
        return _nextSharedPromise->getFuture();
    }();

    // Throws on error if the flusher thread is shutdown.
    myFuture.get(interruptible);
}

void JournalFlusher::triggerJournalFlush() {
    stdx::unique_lock<Latch> lk(_stateMutex);
    _triggerJournalFlush(lk);
}

void JournalFlusher::interruptJournalFlusherForReplStateChange() {
    stdx::lock_guard<Latch> lk(_opCtxMutex);
    if (_uniqueCtx) {
        stdx::lock_guard<Client> lk(*_uniqueCtx->get()->getClient());
        _uniqueCtx->get()->markKilled(ErrorCodes::InterruptedDueToReplStateChange);
    }
}

void JournalFlusher::_triggerJournalFlush(WithLock lk) {
    if (!_flushJournalNow) {
        _flushJournalNow = true;
        _flushJournalNowCV.notify_one();
    }
}

}  // namespace mongo
