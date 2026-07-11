// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/repl/oplog_fetcher_mock.h"

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
// IWYU pragma: no_include "cxxabi.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/db/client.h"
#include "mongo/db/repl/replication_process.h"
#include "mongo/util/assert_util.h"

#include <functional>
#include <mutex>
#include <utility>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication


namespace mongo {
namespace repl {
OplogFetcherMock::OplogFetcherMock(
    executor::TaskExecutor* executor,
    std::unique_ptr<OplogFetcherRestartDecision> oplogFetcherRestartDecision,
    DataReplicatorExternalState* dataReplicatorExternalState,
    EnqueueDocumentsFn enqueueDocumentsFn,
    OnShutdownCallbackFn onShutdownCallbackFn,
    Config config)
    : OplogFetcher(
          executor,
          // Pass a dummy OplogFetcherRestartDecision to the base OplogFetcher.
          std::make_unique<OplogFetcherRestartDecisionDefault>(0),
          dataReplicatorExternalState,
          // Pass a dummy EnqueueDocumentsFn to the base OplogFetcher.
          [](const auto& a1, const auto& a2, const auto& a3) { return Status::OK(); },
          // Pass a dummy OnShutdownCallbackFn to the base OplogFetcher.
          [](const auto& a) {},
          config),
      _oplogFetcherRestartDecision(std::move(oplogFetcherRestartDecision)),
      _onShutdownCallbackFn(std::move(onShutdownCallbackFn)),
      _enqueueDocumentsFn(std::move(enqueueDocumentsFn)),
      _startingPoint(config.startingPoint),
      _lastFetched(config.initialLastFetched) {}

OplogFetcherMock::~OplogFetcherMock() {
    shutdown();
    join();
    std::lock_guard lk(_joinFinishThreadMutex);
    if (_waitForFinishThread.joinable()) {
        _waitForFinishThread.join();
    }
    std::unique_lock ul(_mutex);
    _inTestCodeCV.wait(ul, [this] { return _inTestCodeSemaphore == 0; });
}

void OplogFetcherMock::receiveBatch(CursorId cursorId,
                                    OplogFetcher::Documents documents,
                                    boost::optional<Timestamp> resumeToken) {
    TestCodeBlock tcb(this);
    {
        std::lock_guard lock(_mutex);
        if (!_isActive(lock)) {
            return;
        }
        _oplogFetcherRestartDecision->fetchSuccessful(this);
    }

    auto validateResult = OplogFetcher::validateDocuments(
        documents, _first, getLastOpTimeFetched().getTimestamp(), _startingPoint);

    // Set _first to false after receiving the first batch.
    _first = false;

    // Shutdown the OplogFetcher with error if documents fail to validate.
    if (!validateResult.isOK()) {
        shutdownWith(validateResult.getStatus());
        return;
    }

    auto info = validateResult.getValue();
    if (resumeToken) {
        info.resumeToken = *resumeToken;
    }

    // Enqueue documents in a separate thread with a different client than the test thread. This
    // is to avoid interfering the thread local client in the test thread.
    Status status = Status::OK();
    stdx::thread enqueueDocumentThread([&]() {
        Client::initThread("enqueueDocumentThread", getGlobalServiceContext()->getService());
        status = _enqueueDocumentsFn(documents.cbegin(), documents.cend(), info);
    });
    // Wait until the enqueue finishes.
    enqueueDocumentThread.join();

    // Shutdown the OplogFetcher with error if enqueue fails.
    if (!status.isOK()) {
        shutdownWith(status);
        return;
    }

    if (!documents.empty()) {
        // Update lastFetched to the last oplog entry enqueued.
        auto lastDocRes = OpTime::parseFromOplogEntry(documents.back());
        if (!lastDocRes.isOK()) {
            shutdownWith(lastDocRes.getStatus());
            return;
        }
        auto lastDoc = lastDocRes.getValue();

        std::lock_guard lock(_mutex);
        _lastFetched = lastDoc;
    }

    // Shutdown the OplogFetcher successfully if the sync source closes the oplog tailing cursor.
    if (!cursorId) {
        shutdownWith(Status::OK());
    }
}

void OplogFetcherMock::simulateResponseError(Status status) {
    TestCodeBlock tcb(this);
    invariant(!status.isOK());
    // Shutdown the OplogFetcher with error if it cannot restart.
    if (!_oplogFetcherRestartDecision->shouldContinue(this, status)) {
        shutdownWith(status);
    }
}

void OplogFetcherMock::shutdownWith(Status status) {
    {
        std::lock_guard lock(_mutex);
        // Noop if the OplogFetcher is not active or is already shutting down.
        if (!_isActive(lock) || _isShuttingDown(lock)) {
            return;
        }

        // Fulfill the finish promise so _finishCallback is called.
        if (status.isOK()) {
            _finishPromise->emplaceValue();
        } else {
            _finishPromise->setError(status);
        }
    }
    std::lock_guard lk(_joinFinishThreadMutex);
    if (_waitForFinishThread.joinable()) {
        _waitForFinishThread.join();
    }
}

void OplogFetcherMock::waitForshutdown() {
    std::lock_guard lk(_joinFinishThreadMutex);
    if (_waitForFinishThread.joinable()) {
        _waitForFinishThread.join();
    }
}

void OplogFetcherMock::_doStartup(WithLock) {
    // Create a thread that waits on the _finishPromise and call _finishCallback once with the
    // finish status. This is to synchronize the OplogFetcher shutdown between the test thread and
    // the OplogFetcher's owner. For example, the OplogFetcher could be shut down by the test thread
    // by simulating an error response while the owner of the OplogFetcher (e.g. InitialSyncer) is
    // also trying to shut it down via shutdown() and _doShutdown(). Thus, by having
    // _waitForFinishThread as the only place that calls _finishCallback, we can make sure that
    // _finishCallback is called only once (outside of the mutex) on shutdown.
    _waitForFinishThread = stdx::thread([this]() {
        auto future = [&] {
            Client::initThread("OplogFetcherMock", getGlobalServiceContext()->getService());
            std::lock_guard lock(_mutex);
            return _finishPromise->getFuture();
        }();
        // Wait for the finish signal and call _finishCallback once.
        auto status = future.getNoThrow();
        _finishCallback(status);
    });
}

void OplogFetcherMock::_doShutdown(WithLock) noexcept {
    // Fulfill the finish promise so _finishCallback is called (outside of the mutex).
    if (!_finishPromise->getFuture().isReady()) {
        _finishPromise->setError(
            Status(ErrorCodes::CallbackCanceled, "oplog fetcher shutting down"));
    }
}

ObservableMutex<std::mutex>* OplogFetcherMock::_getMutex() noexcept {
    return &_mutex;
}

OpTime OplogFetcherMock::getLastOpTimeFetched() const {
    std::lock_guard lock(_mutex);
    return _lastFetched;
}

void OplogFetcherMock::_finishCallback(Status status) {
    invariant(isActive());

    // Call _onShutdownCallbackFn outside of the mutex.
    _onShutdownCallbackFn(status);

    decltype(_onShutdownCallbackFn) onShutdownCallbackFn;
    decltype(_oplogFetcherRestartDecision) oplogFetcherRestartDecision;
    std::lock_guard lock(_mutex);
    _transitionToComplete(lock);

    // Release any resources that might be held by the '_onShutdownCallbackFn' function object.
    // The function object will be destroyed outside the lock since the temporary variable
    // 'onShutdownCallbackFn' is declared before 'lock'.
    invariant(_onShutdownCallbackFn);
    std::swap(_onShutdownCallbackFn, onShutdownCallbackFn);

    // Release any resources held by the OplogFetcherRestartDecision.
    invariant(_oplogFetcherRestartDecision);
    std::swap(_oplogFetcherRestartDecision, oplogFetcherRestartDecision);
}
}  // namespace repl
}  // namespace mongo
