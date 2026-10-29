// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once


#include "mongo/base/status.h"
#include "mongo/db/dbmessage.h"
#include "mongo/transport/mock_session.h"
#include "mongo/transport/session_manager_common.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/transport/transport_layer_mock.h"

#include <functional>

namespace mongo {
namespace transport {

using ReactorHandle = std::shared_ptr<Reactor>;

/** Scope guard to set and restore an object value. */
template <typename T>
class ScopedValueOverride {
public:
    ScopedValueOverride(T& target, T v)
        : _target{target}, _saved{std::exchange(_target, std::move(v))} {}
    ~ScopedValueOverride() {
        _target = std::move(_saved);
    }

private:
    T& _target;
    T _saved;
};

class CallbackMockSession : public MockSessionBase {
public:
    CallbackMockSession() = default;

    CallbackMockSession(HostAndPort remote, SockAddr remoteAddr, SockAddr localAddr)
        : MockSessionBase(remote, remoteAddr, localAddr) {}

    TransportLayer* getTransportLayer() const override {
        return getTransportLayerCb();
    }

    void end() override {
        endCb();
    }

    bool isConnected() override {
        return isConnectedCb();
    }

    Status waitForData() override {
        return waitForDataCb();
    }

    StatusWith<Message> sourceMessage() override {
        return sourceMessageCb();
    }

    Status sinkMessage(Message message) override {
        return sinkMessageCb(std::move(message));
    }

    Future<void> asyncWaitForData() override {
        return asyncWaitForDataCb();
    }

    Future<Message> asyncSourceMessage(const BatonHandle& handle) override {
        return asyncSourceMessageCb(handle);
    }

    Future<void> asyncSinkMessage(Message message, const BatonHandle& handle) override {
        return asyncSinkMessageCb(std::move(message), handle);
    }

    void prelude() override {
        if (preludeCb)
            preludeCb();
    }

    std::function<TransportLayer*()> getTransportLayerCb;
    std::function<void()> endCb;
    std::function<bool()> isConnectedCb;
    std::function<Status()> waitForDataCb;
    std::function<StatusWith<Message>()> sourceMessageCb;
    std::function<Status(Message)> sinkMessageCb;
    std::function<Future<void>()> asyncWaitForDataCb;
    std::function<Future<Message>(const BatonHandle&)> asyncSourceMessageCb;
    std::function<Future<void>(Message, const BatonHandle&)> asyncSinkMessageCb;
    std::function<void()> preludeCb;
};

class MockServiceEntryPoint : public ServiceEntryPoint {
public:
    Future<DbResponse> handleRequest(OperationContext* opCtx,
                                     const Message& request,
                                     Date_t started) override {
        return handleRequestCb(opCtx, request);
    }

    std::function<Future<DbResponse>(OperationContext*, const Message&)> handleRequestCb;
};

}  // namespace transport
}  // namespace mongo
