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

#pragma once

#include "mongo/db/service_context.h"
#include "mongo/platform/mutex.h"
#include "mongo/util/background.h"
#include "mongo/util/future.h"

namespace mongo {

class OperationContext;

class JournalFlusher : public BackgroundJob {
public:
    explicit JournalFlusher() : BackgroundJob(/*deleteSelf*/ false) {}

    static JournalFlusher* get(ServiceContext* serviceCtx);
    static JournalFlusher* get(OperationContext* opCtx);
    static void set(ServiceContext* serviceCtx, std::unique_ptr<JournalFlusher> journalFlusher);

    std::string name() const {
        return "JournalFlusher";
    }

    void run();

    /**
     * Signals the thread to quit and then waits until it does.
     */
    void shutdown();

    /**
     * Signals an immediate journal flush and leaves.
     */
    void triggerJournalFlush();

    /**
     * Signals an immediate journal flush and waits for it to complete before returning.
     *
     * Will throw ShutdownInProgress if the flusher thread is being stopped.
     * Will throw InterruptedDueToReplStateChange if a flusher round is interrupted by stepdown.
     */
    void waitForJournalFlush();

    /**
     * Interrupts the journal flusher thread via its operation context with an
     * InterruptedDueToReplStateChange error.
     */
    void interruptJournalFlusherForReplStateChange();

private:
    // Serializes setting/resetting _uniqueCtx and marking _uniqueCtx killed.
    mutable Mutex _opCtxMutex = MONGO_MAKE_LATCH("JournalFlusherOpCtxMutex");

    // Saves a reference to the flusher thread's operation context so it can be interrupted if the
    // flusher is active.
    boost::optional<ServiceContext::UniqueOperationContext> _uniqueCtx;

    // Protects the state below.
    mutable Mutex _stateMutex = MONGO_MAKE_LATCH("JournalFlusherStateMutex");

    // Signaled to wake up the thread, if the thread is waiting. The thread will check whether
    // _flushJournalNow or _shuttingDown is set and flush or stop accordingly.
    mutable stdx::condition_variable _flushJournalNowCV;

    bool _flushJournalNow = false;
    bool _shuttingDown = false;

    // New callers get a future from nextSharedPromise. The JournalFlusher thread will swap that to
    // currentSharedPromise at the start of every round of flushing, and reset nextSharedPromise
    // with a new shared promise.
    std::unique_ptr<SharedPromise<void>> _currentSharedPromise =
        std::make_unique<SharedPromise<void>>();
    std::unique_ptr<SharedPromise<void>> _nextSharedPromise =
        std::make_unique<SharedPromise<void>>();
};

}  // namespace mongo
