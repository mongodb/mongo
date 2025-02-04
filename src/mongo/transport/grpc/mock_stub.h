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

#include "mongo/base/string_data.h"
#include "mongo/transport/grpc/mock_client_context.h"
#include "mongo/transport/grpc/mock_server_context.h"
#include "mongo/transport/grpc/mock_server_stream.h"
#include "mongo/transport/grpc/mock_util.h"
#include "mongo/transport/grpc/service.h"
#include "mongo/unittest/thread_assertion_monitor.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/notification.h"
#include "mongo/util/producer_consumer_queue.h"

namespace mongo::transport::grpc {

class MockRPC {
public:
    enum class MethodName { UnauthenticatedCommandStream, AuthenticatedCommandStream };

    /**
     * Close the stream and send the final return status of the RPC to the client. This is the
     * mocked equivalent of returning a status from an RPC handler.
     *
     * The RPC's stream must not be used after calling this method.
     * This method must only be called once.
     */
    void sendReturnStatus(::grpc::Status status) {
        serverStream->sendReturnStatus(std::move(status));
    }

    /**
     * Cancel the RPC with the provided status. This is used for mocking situations in which an RPC
     * handler was never able to return a final status to the client (e.g. network interruption).
     *
     * For mocking an explicit server-side cancellation, use serverCtx->tryCancel().
     * This method has no effect if the RPC has already been terminated, either by returning a
     * status or prior cancellation.
     */
    void cancel(::grpc::Status status) {
        serverStream->cancel(std::move(status));
    }

    MethodName methodName;
    std::unique_ptr<MockServerStream> serverStream;
    std::unique_ptr<MockServerContext> serverCtx;
};

using MockRPCQueue = MultiProducerMultiConsumerQueue<std::pair<Promise<void>, MockRPC>>;

class MockServer {
public:
    explicit MockServer(MockRPCQueue::Consumer queue) : _queue(std::move(queue)) {}

    boost::optional<MockRPC> acceptRPC() {
        try {
            auto entry = _queue.pop();
            entry.first.emplaceValue();
            return std::move(entry.second);
        } catch (const DBException& e) {
            if (e.code() == ErrorCodes::ProducerConsumerQueueEndClosed ||
                e.code() == ErrorCodes::ProducerConsumerQueueConsumed) {
                return boost::none;
            }
            throw;
        }
    }

    /**
     * Starts up a thread that listens for incoming RPCs and then returns immediately.
     * The listener thread will spawn a new thread for each RPC it receives and pass it to the
     * provided handler.
     *
     * The provided handler is expected to throw assertion exceptions, hence the use of
     * ThreadAssertionMonitor to spawn threads here.
     *
     * The provided WireVersionProvider will be used to determine the cluster's maxWireVersion
     * server-side. To test functionality related to the wire version gossiping, a
     * MockWireVersionProvider can be used instead of the regular WireVersionProvider.
     */
    void start(unittest::ThreadAssertionMonitor& monitor,
               CommandService::RPCHandler handler,
               std::shared_ptr<WireVersionProvider> wvProvider) {
        _listenerThread =
            monitor.spawn([&, handler = std::move(handler), wvProvider = std::move(wvProvider)] {
                CommandService svc(nullptr, handler, std::move(wvProvider));
                std::vector<stdx::thread> rpcHandlers;
                while (auto rpc = acceptRPC()) {
                    rpcHandlers.push_back(monitor.spawn([rpc = std::move(*rpc), &svc]() mutable {
                        try {
                            ::grpc::Status status;
                            switch (rpc.methodName) {
                                case MockRPC::MethodName::UnauthenticatedCommandStream:
                                    status = svc._handleStream(*rpc.serverCtx, *rpc.serverStream);
                                    break;
                                case MockRPC::MethodName::AuthenticatedCommandStream:
                                    status = svc._handleAuthenticatedStream(*rpc.serverCtx,
                                                                            *rpc.serverStream);
                                    break;
                            }
                            rpc.sendReturnStatus(std::move(status));
                        } catch (DBException& e) {
                            rpc.sendReturnStatus(
                                ::grpc::Status(::grpc::StatusCode::UNKNOWN, e.toString()));
                        }
                    }));
                }
                svc.shutdown();

                for (auto& thread : rpcHandlers) {
                    thread.join();
                }
            });
    }

    /**
     * Close the mocked channel and then block until all RPC handler threads (if any) have exited.
     */
    void shutdown() {
        _queue.close();
        if (_listenerThread) {
            _listenerThread->join();
        }
    }

private:
    boost::optional<stdx::thread> _listenerThread;
    MockRPCQueue::Consumer _queue;
};

class MockChannel {
public:
    explicit MockChannel(HostAndPort local, HostAndPort remote, MockRPCQueue::Producer queue)
        : _local(std::move(local)), _remote{std::move(remote)}, _rpcQueue{std::move(queue)} {};

    void sendRPC(MockRPC&& rpc) {
        auto pf = makePromiseFuture<void>();
        _rpcQueue.push({std::move(pf.promise), std::move(rpc)});
        pf.future.get();
    }

    const HostAndPort& getLocal() const {
        return _local;
    }

    const HostAndPort& getRemote() const {
        return _remote;
    }

private:
    HostAndPort _local;
    HostAndPort _remote;
    MockRPCQueue::Producer _rpcQueue;
};

class MockStub {
public:
    explicit MockStub(std::shared_ptr<MockChannel> channel) : _channel{std::move(channel)} {}

    ~MockStub() {}

    std::shared_ptr<MockClientStream> unauthenticatedCommandStream(
        MockClientContext* ctx, const std::shared_ptr<GRPCReactor>& reactor) {
        return _makeStream(MockRPC::MethodName::UnauthenticatedCommandStream, ctx, reactor);
    }

    std::shared_ptr<MockClientStream> authenticatedCommandStream(
        MockClientContext* ctx, const std::shared_ptr<GRPCReactor>& reactor) {
        return _makeStream(MockRPC::MethodName::AuthenticatedCommandStream, ctx, reactor);
    }

private:
    std::shared_ptr<MockClientStream> _makeStream(MockRPC::MethodName methodName,
                                                  MockClientContext* ctx,
                                                  const std::shared_ptr<GRPCReactor>& reactor) {
        MetadataView clientMetadata;
        for (auto& kvp : ctx->_metadata) {
            clientMetadata.insert(kvp);
        }

        BidirectionalPipe pipe;
        auto metadataPF = makePromiseFuture<MetadataContainer>();
        auto terminationStatusPF = makePromiseFuture<::grpc::Status>();
        auto cancellationState = std::make_shared<MockCancellationState>(ctx->getDeadline());

        MockRPC rpc;
        rpc.methodName = methodName;
        rpc.serverStream =
            std::make_unique<MockServerStream>(_channel->getLocal(),
                                               std::move(metadataPF.promise),
                                               std::move(terminationStatusPF.promise),
                                               cancellationState,
                                               std::move(*pipe.left),
                                               clientMetadata);
        rpc.serverCtx = std::make_unique<MockServerContext>(rpc.serverStream.get());
        auto clientStream =
            std::make_shared<MockClientStream>(_channel->getRemote(),
                                               std::move(metadataPF.future),
                                               std::move(terminationStatusPF.future),
                                               cancellationState,
                                               std::move(*pipe.right),
                                               reactor);

        ctx->_stream = clientStream;
        _channel->sendRPC(std::move(rpc));
        return clientStream;
    }

    std::shared_ptr<MockChannel> _channel;
};

}  // namespace mongo::transport::grpc
