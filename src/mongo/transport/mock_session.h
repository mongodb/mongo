/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/base/checked_cast.h"
#include "mongo/config.h"
#include "mongo/stdx/mutex.h"
#include "mongo/transport/session.h"
#include "mongo/transport/session_util.h"
#include "mongo/transport/transport_layer_mock.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/net/sockaddr.h"
#include "mongo/util/net/ssl_types.h"

#include <list>

namespace mongo {
namespace transport {

class MONGO_MOD_UNFORTUNATELY_OPEN MockSessionBase : public Session {
public:
    MockSessionBase() = default;

    explicit MockSessionBase(HostAndPort remote, SockAddr remoteAddr, SockAddr localAddr)
        : _remote(std::move(remote)),
          _remoteAddr(std::move(remoteAddr)),
          _localAddr(std::move(localAddr)),
          _local(HostAndPort(_localAddr.getAddr(), _localAddr.getPort())),
          _restrictionEnvironment(_remoteAddr, _localAddr) {}

    const HostAndPort& remote() const override {
        return _remote;
    }

    const HostAndPort& local() const override {
        return _local;
    }

    const SockAddr& remoteAddr() const {
        return _remoteAddr;
    }

    const SockAddr& localAddr() const {
        return _localAddr;
    }

    void cancelAsyncOperations(const BatonHandle& handle = nullptr) override {}

    void setTimeout(boost::optional<Milliseconds>) override {}

    bool isConnected() override {
        return true;
    }

    bool isLoadBalancerPeer() const override {
        return false;
    };

    bool isConnectedToLoadBalancerPort() const override {
        return false;
    };

    bool isConnectedToMaintenancePort() const override {
        return false;
    }

    void setisLoadBalancerPeer(bool helloHasLoadBalancedOption) override {}

    bool bindsToOperationState() const override {
        return false;
    }

    void appendToBSON(BSONObjBuilder& bb) const override {
        bb.append("id", static_cast<int32_t>(id()));
        bb.append("remote", _remote.toString());
        bb.append("local", _local.toString());
    }

    bool isExemptedByCIDRList(const CIDRList& exemptions) const override {
        return transport::util::isExemptedByCIDRList(remoteAddr(), localAddr(), exemptions);
    }

#ifdef MONGO_CONFIG_SSL
    void setSSLManager(std::shared_ptr<SSLManagerInterface> interface) {
        _sslConfig = interface->getSSLConfiguration();
    }

    const SSLConfiguration* getSSLConfiguration() const override {
        return _sslConfig ? &*_sslConfig : nullptr;
    }
#endif

    const RestrictionEnvironment& getAuthEnvironment() const override {
        return _restrictionEnvironment;
    }

private:
    const HostAndPort _remote;
    const SockAddr _remoteAddr;
    const SockAddr _localAddr;
    const HostAndPort _local;
    RestrictionEnvironment _restrictionEnvironment;
    boost::optional<SSLConfiguration> _sslConfig;
};

class MONGO_MOD_OPEN MockSession : public MockSessionBase {
    MockSession(const MockSession&) = delete;
    MockSession& operator=(const MockSession&) = delete;

public:
    static std::shared_ptr<MockSession> create(TransportLayer* tl) {
        auto handle = std::make_shared<MockSession>(tl);
        return handle;
    }

    static std::shared_ptr<MockSession> create(HostAndPort remote,
                                               SockAddr remoteAddr,
                                               SockAddr localAddr,
                                               TransportLayer* tl) {
        auto handle = std::make_shared<MockSession>(
            std::move(remote), std::move(remoteAddr), std::move(localAddr), tl);
        return handle;
    }

    TransportLayer* getTransportLayer() const override {
        return _tl;
    }

    void end() override {
        if (!_tl || !_tl->owns(id()))
            return;
        _tl->_sessions[id()].ended = true;
    }

    StatusWith<Message> sourceMessage() override {
        if (!_tl || _tl->inShutdown()) {
            return TransportLayer::ShutdownStatus;
        } else if (!_tl->owns(id())) {
            return TransportLayer::SessionUnknownStatus;
        } else if (_tl->_sessions[id()].ended) {
            return TransportLayer::TicketSessionClosedStatus;
        }

        return Message();  // Subclasses can do something different.
    }

    Future<Message> asyncSourceMessage(const BatonHandle& handle = nullptr) override {
        return Future<Message>::makeReady(sourceMessage());
    }

    Status waitForData() override {
        return asyncWaitForData().getNoThrow();
    }

    Future<void> asyncWaitForData() override {
        auto fp = makePromiseFuture<void>();
        stdx::lock_guard<stdx::mutex> lk(_waitForDataMutex);
        _waitForDataQueue.emplace_back(std::move(fp.promise));
        return std::move(fp.future);
    }

    void signalAvailableData() {
        stdx::lock_guard<stdx::mutex> lk(_waitForDataMutex);
        if (_waitForDataQueue.size() == 0)
            return;
        Promise<void> promise = std::move(_waitForDataQueue.front());
        _waitForDataQueue.pop_front();
        promise.emplaceValue();
    }

    Status sinkMessage(Message message) override {
        if (!_tl || _tl->inShutdown()) {
            return TransportLayer::ShutdownStatus;
        } else if (!_tl->owns(id())) {
            return TransportLayer::SessionUnknownStatus;
        } else if (_tl->_sessions[id()].ended) {
            return TransportLayer::TicketSessionClosedStatus;
        }

        return Status::OK();
    }

    Future<void> asyncSinkMessage(Message message, const BatonHandle& handle = nullptr) override {
        return Future<void>::makeReady(sinkMessage(message));
    }

    explicit MockSession(TransportLayer* tl)
        : MockSessionBase(), _tl(checked_cast<TransportLayerMock*>(tl)) {}
    explicit MockSession(HostAndPort remote,
                         SockAddr remoteAddr,
                         SockAddr localAddr,
                         TransportLayer* tl)
        : MockSessionBase(std::move(remote), std::move(remoteAddr), std::move(localAddr)),
          _tl(checked_cast<TransportLayerMock*>(tl)) {}

protected:
    TransportLayerMock* const _tl;

    mutable stdx::mutex _waitForDataMutex;
    std::list<Promise<void>> _waitForDataQueue;
};

}  // namespace transport
}  // namespace mongo
