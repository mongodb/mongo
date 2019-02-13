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
#include "mongo/transport/session.h"
#include "mongo/transport/transport_layer_mock.h"
#include "mongo/util/net/hostandport.h"

namespace mongo {
namespace transport {

class MockSession : public Session {
    MONGO_DISALLOW_COPYING(MockSession);

public:
    static std::shared_ptr<MockSession> create(TransportLayer* tl) {
        std::shared_ptr<MockSession> handle(new MockSession(tl));
        return handle;
    }

    static std::shared_ptr<MockSession> create(HostAndPort remote,
                                               HostAndPort local,
                                               TransportLayer* tl) {
        std::shared_ptr<MockSession> handle(
            new MockSession(std::move(remote), std::move(local), tl));
        return handle;
    }

    TransportLayer* getTransportLayer() const override {
        return _tl;
    }

    const HostAndPort& remote() const override {
        return _remote;
    }

    const HostAndPort& local() const override {
        return _local;
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

    void cancelAsyncOperations(const BatonHandle& handle = nullptr) override {}

    void setTimeout(boost::optional<Milliseconds>) override {}

    bool isConnected() override {
        return true;
    }

    explicit MockSession(TransportLayer* tl)
        : _tl(checked_cast<TransportLayerMock*>(tl)), _remote(), _local() {}
    explicit MockSession(HostAndPort remote, HostAndPort local, TransportLayer* tl)
        : _tl(checked_cast<TransportLayerMock*>(tl)),
          _remote(std::move(remote)),
          _local(std::move(local)) {}

protected:
    TransportLayerMock* _tl;

    HostAndPort _remote;
    HostAndPort _local;
};

}  // namespace transport
}  // namespace mongo
