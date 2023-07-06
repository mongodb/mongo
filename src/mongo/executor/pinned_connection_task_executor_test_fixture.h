/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/base/status.h"
#include "mongo/db/dbmessage.h"
#include "mongo/executor/network_interface.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/pinned_connection_task_executor.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/rpc/op_msg_rpc_impls.h"
#include "mongo/transport/mock_session.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/transport/transport_layer_mock.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/concurrency/thread_pool.h"


namespace mongo::executor {


class PinnedConnectionTaskExecutorTest : public ThreadPoolExecutorTest {
    using SinkMessageCbT = std::function<Status(Message)>;
    using SourceMessageCbT = std::function<StatusWith<Message>()>;

public:
    void setUp() override {
        ThreadPoolExecutorTest::setUp();
        _session = std::make_shared<CustomMockSession>(this);
        getNet()->setLeasedStreamMaker(
            [this](HostAndPort hp) { return std::make_unique<LeasedStream>(hp, _session, this); });
        launchExecutorThread();
    }

    void tearDown() override {
        ThreadPoolExecutorTest::tearDown();
        _session.reset();
    }

    Status sinkMessageCalled(Message message) {
        stdx::unique_lock lk{_mutex};
        _hasWaitingSinkMessage = true;
        _cv.wait(lk, [&] { return !!_sinkMessageExpectation || _isCanceled; });
        if (_isCanceled) {
            // Consume the cancellation.
            _isCanceled = false;
            _sinkMessageExpectation = [&](auto&&) {
                return _cancellationError;
            };
        }
        auto expectation = *std::exchange(_sinkMessageExpectation, {});
        _hasWaitingSinkMessage = false;
        return expectation(message);
    }

    void expectSinkMessage(SinkMessageCbT handler) {
        stdx::lock_guard lk{_mutex};
        invariant(!_sinkMessageExpectation);
        _sinkMessageExpectation = std::move(handler);
        _cv.notify_one();
    }

    StatusWith<Message> sourceMessageCalled() {
        stdx::unique_lock lk{_mutex};
        _hasWaitingSourceMessage = true;
        _cv.wait(lk, [&] { return !!_sourceMessageExpectation || _isCanceled; });
        if (_isCanceled) {
            // Consume the cancellation.
            _isCanceled = false;
            _sourceMessageExpectation = [&]() {
                return _cancellationError;
            };
        }
        auto expectation = *std::exchange(_sourceMessageExpectation, {});
        _hasWaitingSourceMessage = false;
        return expectation();
    }

    void expectSourceMessage(SourceMessageCbT handler) {
        stdx::lock_guard lk{_mutex};
        invariant(!_sourceMessageExpectation);
        _sourceMessageExpectation = std::move(handler);
        _cv.notify_one();
    }

    void cancelAsyncOpsCalled() {
        stdx::unique_lock lk{_mutex};
        _isCanceled = true;
        _cv.notify_one();
    }

    bool hasReadyRequests() {
        stdx::lock_guard lk{_mutex};
        return _hasWaitingSinkMessage || _hasWaitingSourceMessage;
    }

    std::shared_ptr<PinnedConnectionTaskExecutor> makePinnedConnTaskExecutor() {
        return std::make_shared<PinnedConnectionTaskExecutor>(getExecutorPtr(), getNet());
    }

private:
    std::shared_ptr<transport::Session> _session;
    mutable Mutex _mutex;
    stdx::condition_variable _cv;
    boost::optional<SinkMessageCbT> _sinkMessageExpectation;
    boost::optional<SourceMessageCbT> _sourceMessageExpectation;
    bool _hasWaitingSinkMessage = false;
    bool _hasWaitingSourceMessage = false;
    bool _isCanceled = false;
    Status _cancellationError = Status{ErrorCodes::SocketException, "Socket closed"};

    class CustomMockSession : public transport::MockSessionBase {
    public:
        explicit CustomMockSession(PinnedConnectionTaskExecutorTest* fixture) : _fixture{fixture} {}

        transport::TransportLayer* getTransportLayer() const override {
            return nullptr;
        }

        void end() override {
            *_connected = false;
        }

        bool isConnected() override {
            return *_connected;
        }

        Status waitForData() noexcept override {
            return Status::OK();
        }

        StatusWith<Message> sourceMessage() noexcept override {
            return _fixture->sourceMessageCalled();
        }

        Status sinkMessage(Message message) noexcept override {
            return _fixture->sinkMessageCalled(message);
        }

        Future<void> asyncWaitForData() noexcept override {
            return ExecutorFuture<void>(_fixture->getExecutorPtr())
                .then([this] { return waitForData(); })
                .unsafeToInlineFuture();
        }

        Future<Message> asyncSourceMessage(const BatonHandle& handle) noexcept override {
            return ExecutorFuture<void>(_fixture->getExecutorPtr())
                .then([this] { return sourceMessage(); })
                .unsafeToInlineFuture();
        }

        Future<void> asyncSinkMessage(Message message,
                                      const BatonHandle& handle) noexcept override {
            return ExecutorFuture<void>(_fixture->getExecutorPtr())
                .then([this, m = std::move(message)] { return sinkMessage(m); })
                .unsafeToInlineFuture();
        }

        void cancelAsyncOperations(const BatonHandle& handle = nullptr) override {
            _fixture->cancelAsyncOpsCalled();
        }

    private:
        PinnedConnectionTaskExecutorTest* _fixture;
        synchronized_value<bool> _connected{true};
    };

    class LeasedStream : public NetworkInterface::LeasedStream {
    public:
        LeasedStream(HostAndPort hp,
                     std::shared_ptr<transport::Session> session,
                     PinnedConnectionTaskExecutorTest* fixture)
            : _fixture{fixture} {
            invariant(session);
            _client = std::make_shared<AsyncDBClient>(hp, std::move(session), nullptr);
        }
        ~LeasedStream() {
            _fixture->_streamDestroyedCalls.fetchAndAdd(1);
        }
        AsyncDBClient* getClient() override {
            return _client.get();
        }
        void indicateSuccess() override {
            _fixture->_indicateSuccessCalls.fetchAndAdd(1);
        }
        void indicateUsed() override {
            _fixture->_indicateUsedCalls.fetchAndAdd(1);
        }
        void indicateFailure(Status) override {
            _fixture->_indicateFailureCalls.fetchAndAdd(1);
        }

    private:
        PinnedConnectionTaskExecutorTest* _fixture;
        std::shared_ptr<AsyncDBClient> _client;
    };

protected:
    // Track the success/used/failure/destruction calls across LeasedStreams created via this
    // fixture. Accessible to children so tests can read them directly.
    AtomicWord<size_t> _indicateSuccessCalls{0};
    AtomicWord<size_t> _indicateUsedCalls{0};
    AtomicWord<size_t> _indicateFailureCalls{0};
    AtomicWord<size_t> _streamDestroyedCalls{0};
};


}  // namespace mongo::executor
