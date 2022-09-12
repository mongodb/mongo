/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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


#include <functional>

#include "mongo/base/status.h"
#include "mongo/db/dbmessage.h"
#include "mongo/transport/mock_session.h"
#include "mongo/transport/service_entry_point_impl.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/transport/transport_layer_mock.h"

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
    TransportLayer* getTransportLayer() const override {
        return getTransportLayerCb();
    }

    void end() override {
        endCb();
    }

    bool isConnected() override {
        return isConnectedCb();
    }

    Status waitForData() noexcept override {
        return waitForDataCb();
    }

    StatusWith<Message> sourceMessage() noexcept override {
        return sourceMessageCb();
    }

    Status sinkMessage(Message message) noexcept override {
        return sinkMessageCb(std::move(message));
    }

    Future<void> asyncWaitForData() noexcept override {
        return asyncWaitForDataCb();
    }

    Future<Message> asyncSourceMessage(const BatonHandle& handle) noexcept override {
        return asyncSourceMessageCb(handle);
    }

    Future<void> asyncSinkMessage(Message message, const BatonHandle& handle) noexcept override {
        return asyncSinkMessageCb(std::move(message), handle);
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
};

class MockServiceEntryPoint : public ServiceEntryPointImpl {
public:
    explicit MockServiceEntryPoint(ServiceContext* svcCtx) : ServiceEntryPointImpl(svcCtx) {}

    Future<DbResponse> handleRequest(OperationContext* opCtx,
                                     const Message& request) noexcept override {
        return handleRequestCb(opCtx, request);
    }

    void onEndSession(const SessionHandle& handle) override {
        onEndSessionCb(handle);
    }

    void derivedOnClientDisconnect(Client* client) override {
        derivedOnClientDisconnectCb(client);
    }

    void configureServiceExecutorContext(ServiceContext::UniqueClient& client,
                                         bool isPrivilegedSession) override {
        if (configureServiceExecutorContextCb)
            configureServiceExecutorContextCb(client, isPrivilegedSession);
        else
            ServiceEntryPointImpl::configureServiceExecutorContext(client, isPrivilegedSession);
    }

    std::function<Future<DbResponse>(OperationContext*, const Message&)> handleRequestCb;
    std::function<void(const SessionHandle)> onEndSessionCb;
    std::function<void(Client*)> derivedOnClientDisconnectCb;
    std::function<void(ServiceContext::UniqueClient&, bool)> configureServiceExecutorContextCb;
};

}  // namespace transport
}  // namespace mongo
