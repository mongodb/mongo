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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/db/storage/control/journal_flusher.h"

#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/logv2/log.h"
#include "mongo/stdx/future.h"
#include "mongo/util/concurrency/idle_thread_block.h"
#include "mongo/util/fail_point.h"

namespace mongo {

namespace {

const auto getJournalFlusher = ServiceContext::declareDecoration<std::unique_ptr<JournalFlusher>>();

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
    ThreadClient tc(name(), getGlobalServiceContext());
    LOGV2_DEBUG(45847001, 1, "starting {name} thread", "name"_attr = name());

    // Initialize the thread's opCtx.
    _uniqueCtx.emplace(tc->makeOperationContext());

    // Updates to a non-replicated collection, oplogTruncateAfterPoint, are made by this thread.
    // Non-replicated writes will not contribute to replication lag and can be safely excluded
    // from Flow Control.
    _uniqueCtx->get()->setShouldParticipateInFlowControl(false);
    while (true) {

        pauseJournalFlusherThread.pauseWhileSet(_uniqueCtx->get());

        try {
            ON_BLOCK_EXIT([&] {
                // We do not want to miss an interrupt for the next round. Therefore, the opCtx
                // will be reset after a flushing round finishes.
                //
                // It is fine if the opCtx is signaled between finishing and resetting because
                // state changes will be seen before the next round. We want to catch any
                // interrupt signals that occur after state is checked at the start of a round:
                // the time during or before the next flush.
                stdx::lock_guard<Latch> lk(_opCtxMutex);
                _uniqueCtx.reset();
                _uniqueCtx.emplace(tc->makeOperationContext());
                _uniqueCtx->get()->setShouldParticipateInFlowControl(false);
            });

            _uniqueCtx->get()->recoveryUnit()->waitUntilDurable(_uniqueCtx->get());

            // Signal the waiters that a round completed.
            _currentSharedPromise->emplaceValue();
        } catch (const AssertionException& e) {
            invariant(ErrorCodes::isShutdownError(e.code()) ||
                          e.code() == ErrorCodes::InterruptedDueToReplStateChange,
                      e.toString());

            // Signal the waiters that the fsync was interrupted.
            _currentSharedPromise->setError(e.toStatus());
        }

        // Wait until either journalCommitIntervalMs passes or an immediate journal flush is
        // requested (or shutdown).

        auto deadline =
            Date_t::now() + Milliseconds(storageGlobalParams.journalCommitIntervalMs.load());

        stdx::unique_lock<Latch> lk(_stateMutex);

        MONGO_IDLE_THREAD_BLOCK;
        _flushJournalNowCV.wait_until(
            lk, deadline.toSystemTimePoint(), [&] { return _flushJournalNow || _shuttingDown; });

        _flushJournalNow = false;

        if (_shuttingDown) {
            LOGV2_DEBUG(45847002, 1, "stopping {name} thread", "name"_attr = name());
            _nextSharedPromise->setError(
                Status(ErrorCodes::ShutdownInProgress, "The storage catalog is being closed."));
            stdx::lock_guard<Latch> lk(_opCtxMutex);
            _uniqueCtx.reset();
            return;
        }

        // Take the next promise as current and reset the next promise.
        _currentSharedPromise =
            std::exchange(_nextSharedPromise, std::make_unique<SharedPromise<void>>());
    }
}

void JournalFlusher::shutdown() {
    LOGV2(22320, "Shutting down journal flusher thread");
    {
        stdx::lock_guard<Latch> lk(_stateMutex);
        _shuttingDown = true;
        _flushJournalNowCV.notify_one();
    }
    wait();
    LOGV2(22321, "Finished shutting down journal flusher thread");
}

void JournalFlusher::triggerJournalFlush() {
    stdx::lock_guard<Latch> lk(_stateMutex);
    if (!_flushJournalNow) {
        _flushJournalNow = true;
        _flushJournalNowCV.notify_one();
    }
}

void JournalFlusher::waitForJournalFlush() {
    auto myFuture = [&]() {
        stdx::unique_lock<Latch> lk(_stateMutex);
        if (!_flushJournalNow) {
            _flushJournalNow = true;
            _flushJournalNowCV.notify_one();
        }
        return _nextSharedPromise->getFuture();
    }();
    // Throws on error if the catalog is closed or the flusher round is interrupted by stepdown.
    myFuture.get();
}

void JournalFlusher::interruptJournalFlusherForReplStateChange() {
    stdx::lock_guard<Latch> lk(_opCtxMutex);
    if (_uniqueCtx) {
        stdx::lock_guard<Client> lk(*_uniqueCtx->get()->getClient());
        _uniqueCtx->get()->markKilled(ErrorCodes::InterruptedDueToReplStateChange);
    }
}

}  // namespace mongo
