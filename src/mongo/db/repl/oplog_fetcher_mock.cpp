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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication

#include "mongo/db/repl/oplog_fetcher_mock.h"

#include <utility>


namespace mongo {
namespace repl {
OplogFetcherMock::OplogFetcherMock(
    executor::TaskExecutor* executor,
    OpTime lastFetched,
    HostAndPort source,
    ReplSetConfig config,
    std::unique_ptr<OplogFetcherRestartDecision> oplogFetcherRestartDecision,
    int requiredRBID,
    bool requireFresherSyncSource,
    DataReplicatorExternalState* dataReplicatorExternalState,
    EnqueueDocumentsFn enqueueDocumentsFn,
    OnShutdownCallbackFn onShutdownCallbackFn,
    const int batchSize,
    StartingPoint startingPoint)
    : OplogFetcher(executor,
                   lastFetched,
                   std::move(source),
                   std::move(config),
                   // Pass a dummy OplogFetcherRestartDecision to the base OplogFetcher.
                   std::make_unique<OplogFetcherRestartDecisionDefault>(0),
                   requiredRBID,
                   requireFresherSyncSource,
                   dataReplicatorExternalState,
                   // Pass a dummy EnqueueDocumentsFn to the base OplogFetcher.
                   [](const auto& a1, const auto& a2, const auto& a3) { return Status::OK(); },
                   // Pass a dummy OnShutdownCallbackFn to the base OplogFetcher.
                   [](const auto& a) {},
                   batchSize,
                   startingPoint),
      _oplogFetcherRestartDecision(std::move(oplogFetcherRestartDecision)),
      _onShutdownCallbackFn(std::move(onShutdownCallbackFn)),
      _enqueueDocumentsFn(std::move(enqueueDocumentsFn)),
      _startingPoint(startingPoint),
      _lastFetched(lastFetched) {}

OplogFetcherMock::~OplogFetcherMock() {
    shutdown();
    join();
    if (_waitForFinishThread.joinable()) {
        _waitForFinishThread.join();
    }
}

void OplogFetcherMock::receiveBatch(CursorId cursorId, OplogFetcher::Documents documents) {
    {
        stdx::lock_guard<Latch> lock(_mutex);
        if (!_isActive_inlock()) {
            return;
        }
        _oplogFetcherRestartDecision->fetchSuccessful(this);
    }

    auto validateResult = OplogFetcher::validateDocuments(
        documents, _first, _getLastOpTimeFetched().getTimestamp(), _startingPoint);

    // Set _first to false after receiving the first batch.
    _first = false;

    // Shutdown the OplogFetcher with error if documents fail to validate.
    if (!validateResult.isOK()) {
        shutdownWith(validateResult.getStatus());
        return;
    }

    if (!documents.empty()) {
        auto info = validateResult.getValue();

        // Enqueue documents in a separate thread with a different client than the test thread. This
        // is to avoid interfering the thread local client in the test thread.
        Status status = Status::OK();
        stdx::thread enqueueDocumentThread([&]() {
            Client::initThread("enqueueDocumentThread");
            status = _enqueueDocumentsFn(documents.cbegin(), documents.cend(), info);
        });
        // Wait until the enqueue finishes.
        enqueueDocumentThread.join();

        // Shutdown the OplogFetcher with error if enqueue fails.
        if (!status.isOK()) {
            shutdownWith(status);
            return;
        }

        // Update lastFetched to the last oplog entry enqueued.
        auto lastDocRes = OpTime::parseFromOplogEntry(documents.back());
        if (!lastDocRes.isOK()) {
            shutdownWith(lastDocRes.getStatus());
            return;
        }
        auto lastDoc = lastDocRes.getValue();

        stdx::lock_guard<Latch> lock(_mutex);
        _lastFetched = lastDoc;
    }

    // Shutdown the OplogFetcher successfully if the sync source closes the oplog tailing cursor.
    if (!cursorId) {
        shutdownWith(Status::OK());
    }
}

void OplogFetcherMock::simulateResponseError(Status status) {
    invariant(!status.isOK());
    // Shutdown the OplogFetcher with error if it cannot restart.
    if (!_oplogFetcherRestartDecision->shouldContinue(this, status)) {
        shutdownWith(status);
    }
}

void OplogFetcherMock::shutdownWith(Status status) {
    {
        stdx::lock_guard<Latch> lock(_mutex);
        // Noop if the OplogFetcher is not active or is already shutting down.
        if (!_isActive_inlock() || _isShuttingDown_inlock()) {
            return;
        }

        // Fulfill the finish promise so _finishCallback is called.
        if (status.isOK()) {
            _finishPromise->emplaceValue();
        } else {
            _finishPromise->setError(status);
        }
    }
    _waitForFinishThread.join();
}

void OplogFetcherMock::waitForshutdown() {
    if (_waitForFinishThread.joinable()) {
        _waitForFinishThread.join();
    }
}

Status OplogFetcherMock::_doStartup_inlock() noexcept {
    // Create a thread that waits on the _finishPromise and call _finishCallback once with the
    // finish status. This is to synchronize the OplogFetcher shutdown between the test thread and
    // the OplogFetcher's owner. For example, the OplogFetcher could be shut down by the test thread
    // by simulating an error response while the owner of the OplogFetcher (e.g. InitialSyncer) is
    // also trying to shut it down via shutdown() and _doShutdown_inlock(). Thus, by having
    // _waitForFinishThread as the only place that calls _finishCallback, we can make sure that
    // _finishCallback is called only once (outside of the mutex) on shutdown.
    _waitForFinishThread = stdx::thread([this]() {
        auto future = [&] {
            Client::initThread("OplogFetcherMock");
            stdx::lock_guard<Latch> lock(_mutex);
            return _finishPromise->getFuture();
        }();
        // Wait for the finish signal and call _finishCallback once.
        auto status = future.getNoThrow();
        _finishCallback(status);
    });
    return Status::OK();
}

void OplogFetcherMock::_doShutdown_inlock() noexcept {
    // Fulfill the finish promise so _finishCallback is called (outside of the mutex).
    if (!_finishPromise->getFuture().isReady()) {
        _finishPromise->setError(
            Status(ErrorCodes::CallbackCanceled, "oplog fetcher shutting down"));
    }
}

Mutex* OplogFetcherMock::_getMutex() noexcept {
    return &_mutex;
}

OpTime OplogFetcherMock::_getLastOpTimeFetched() const {
    stdx::lock_guard<Latch> lock(_mutex);
    return _lastFetched;
}

void OplogFetcherMock::_finishCallback(Status status) {
    invariant(isActive());

    // Call _onShutdownCallbackFn outside of the mutex.
    _onShutdownCallbackFn(status);

    decltype(_onShutdownCallbackFn) onShutdownCallbackFn;
    decltype(_oplogFetcherRestartDecision) oplogFetcherRestartDecision;
    stdx::lock_guard<Latch> lock(_mutex);
    _transitionToComplete_inlock();

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
