// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/query/client_cursor/cursor_id.h"
#include "mongo/db/repl/data_replicator_external_state.h"
#include "mongo/db/repl/oplog_fetcher.h"
#include "mongo/db/repl/optime.h"
#include "mongo/executor/task_executor.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/future.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/modules.h"

#include <memory>
#include <mutex>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace repl {
class [[MONGO_MOD_PARENT_PRIVATE]] OplogFetcherMock : public OplogFetcher {
public:
    explicit OplogFetcherMock(
        executor::TaskExecutor* executor,
        std::unique_ptr<OplogFetcherRestartDecision> oplogFetcherRestartDecision,
        DataReplicatorExternalState* dataReplicatorExternalState,
        EnqueueDocumentsFn enqueueDocumentsFn,
        OnShutdownCallbackFn onShutdownCallbackFn,
        Config config);

    ~OplogFetcherMock() override;

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

    void _doStartup(WithLock) override;

    void _doShutdown(WithLock) noexcept override;

    void _preJoin() noexcept override {}

    ObservableMutex<std::mutex>* _getMutex() noexcept override;

    // ============= End AbstractAsyncComponent overrides ==============
    class TestCodeBlock {
    public:
        TestCodeBlock(OplogFetcherMock* mock) : _mock(mock) {
            std::lock_guard lk(_mock->_mutex);
            _mock->_inTestCodeSemaphore++;
        }

        ~TestCodeBlock() {
            std::lock_guard lk(_mock->_mutex);
            _mock->_inTestCodeSemaphore--;
            _mock->_inTestCodeCV.notify_one();
        }

    private:
        OplogFetcherMock* _mock;
    };

    OpTime getLastOpTimeFetched() const override;

    void _finishCallback(Status status);

    mutable ObservableMutex<std::mutex> _mutex;

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
    mutable std::mutex _joinFinishThreadMutex;

    // Thread to wait for _finishPromise and call _onShutdownCallbackFn with the given status only
    // once before the OplogFetcher finishes.
    stdx::thread _waitForFinishThread;

    // Counts the number of times we are in simulateReceiveBatch or simulateError, so we can
    // delay destruction until those calls complete.
    int _inTestCodeSemaphore = 0;
    stdx::condition_variable _inTestCodeCV;

    bool _first = true;
};

[[MONGO_MOD_PUBLIC]] typedef OplogFetcherFactoryImpl<OplogFetcherMock> CreateOplogFetcherMockFn;

}  // namespace repl
}  // namespace mongo
