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

#include "mongo/db/cursor_id.h"
#include "mongo/db/repl/oplog_fetcher.h"

namespace mongo {
namespace repl {
class OplogFetcherMock : public OplogFetcher {
public:
    explicit OplogFetcherMock(
        executor::TaskExecutor* executor,
        std::unique_ptr<OplogFetcherRestartDecision> oplogFetcherRestartDecision,
        DataReplicatorExternalState* dataReplicatorExternalState,
        EnqueueDocumentsFn enqueueDocumentsFn,
        OnShutdownCallbackFn onShutdownCallbackFn,
        Config config);

    virtual ~OplogFetcherMock();

    /**
     * Simulate a batch received by the OplogFetcher. This is a batch that will be enqueued using
     * the enqueueDocumentsFn.
     * This is not thread-safe.
     */
    void receiveBatch(CursorId cursorId,
                      OplogFetcher::Documents documents,
                      boost::optional<Timestamp> resumeToken = boost::none);

    /**
     * Simulate an response error received by the OplogFetcher.
     * This is not thread-safe.
     */
    void simulateResponseError(Status status);

    /**
     * Shutdown the OplogFetcher with the given status.
     * This is not thread-safe.
     */
    void shutdownWith(Status status);

    /**
     * Wait for the OplogFetcher to shutdown.
     * This is not thread-safe.
     */
    void waitForshutdown();

private:
    // =============== AbstractAsyncComponent overrides ================

    void _doStartup_inlock() override;

    void _doShutdown_inlock() noexcept override;

    void _preJoin() noexcept override {}

    Mutex* _getMutex() noexcept override;

    // ============= End AbstractAsyncComponent overrides ==============
    class TestCodeBlock {
    public:
        TestCodeBlock(OplogFetcherMock* mock) : _mock(mock) {
            stdx::lock_guard lk(_mock->_mutex);
            _mock->_inTestCodeSemaphore++;
        }

        ~TestCodeBlock() {
            stdx::lock_guard lk(_mock->_mutex);
            _mock->_inTestCodeSemaphore--;
            _mock->_inTestCodeCV.notify_one();
        }

    private:
        OplogFetcherMock* _mock;
    };

    OpTime _getLastOpTimeFetched() const override;

    void _finishCallback(Status status);

    mutable Mutex _mutex = MONGO_MAKE_LATCH("OplogFetcherMock::_mutex");

    std::unique_ptr<OplogFetcherRestartDecision> _oplogFetcherRestartDecision;

    OnShutdownCallbackFn _onShutdownCallbackFn;
    const EnqueueDocumentsFn _enqueueDocumentsFn;
    StartingPoint _startingPoint;

    OpTime _lastFetched;

    // This promise is fulfilled with an exit status when the OplogFetcher finishes running and
    // _onShutdownCallbackFn will be called with the status.
    std::unique_ptr<SharedPromise<void>> _finishPromise = std::make_unique<SharedPromise<void>>();

    // Mutex to ensure we call join() on the _waitForFinishThread only once.  This mutex should
    // never be held when _mutex is held.
    mutable Mutex _joinFinishThreadMutex =
        MONGO_MAKE_LATCH("OplogFetcherMock::_joinFinishThreadMutex");

    // Thread to wait for _finishPromise and call _onShutdownCallbackFn with the given status only
    // once before the OplogFetcher finishes.
    stdx::thread _waitForFinishThread;

    // Counts the number of times we are in simulateReceiveBatch or simulateError, so we can
    // delay destruction until those calls complete.
    int _inTestCodeSemaphore = 0;
    stdx::condition_variable _inTestCodeCV;

    bool _first = true;
};

typedef OplogFetcherFactoryImpl<OplogFetcherMock> CreateOplogFetcherMockFn;

}  // namespace repl
}  // namespace mongo
