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

#include "mongo/transport/grpc/grpc_transport_layer.h"

namespace mongo::transport::grpc {

GRPCTransportLayer::GRPCTransportLayer(ServiceContext* svcCtx, const WireSpec& wireSpec)
    : TransportLayer(wireSpec), _svcCtx(svcCtx) {
    auto sslModeResolver = [this](ConnectSSLMode sslMode) {
        // TODO SERVER-74020: use `sslMode` and the settings to decide if we should use SSL.
        return false;
    };

    auto channelFactory = [this](HostAndPort& remote, bool useSSL) {
        // TODO SERVER-74020: use the gRPC client to create a new channel to `remote`.
        return ChannelType{};
    };

    auto stubFactory = [this](ChannelType& channel) {
        // TODO SERVER-74020: use `channel` to create a new gRPC stub.
        return StubType{};
    };

    // The pool calls into `ClockSource` to record the last usage of gRPC channels. Since the pool
    // is not concerned with sub-minute durations and this call happens as part of destroying gRPC
    // stubs (i.e., on threads running user operations), it is important to use `FastClockSource` to
    // minimize the performance implications of recording time on user operations.
    _channelPool = std::make_shared<ChannelPoolType>(_svcCtx->getFastClockSource(),
                                                     std::move(sslModeResolver),
                                                     std::move(channelFactory),
                                                     std::move(stubFactory));
}

Status GRPCTransportLayer::start() try {
    // Periodically call into the channel pool to drop idle channels. A periodic task runs to drop
    // all channels that have been idle for `kDefaultChannelTimeout`.
    PeriodicRunner::PeriodicJob prunerJob(
        "GRPCIdleChannelPrunerJob",
        [pool = _channelPool](Client*) {
            pool->dropIdleChannels(GRPCTransportLayer::kDefaultChannelTimeout);
        },
        kDefaultChannelTimeout);
    invariant(!_idleChannelPruner);
    _idleChannelPruner.emplace(_svcCtx->getPeriodicRunner()->makeJob(std::move(prunerJob)));
    _idleChannelPruner->start();

    // TODO SERVER-74020: start the Server and Client services.

    return Status::OK();
} catch (const DBException& ex) {
    return ex.toStatus();
}

void GRPCTransportLayer::shutdown() {
    // TODO SERVER-74020: shutdown the Server and Client services.

    if (_idleChannelPruner) {
        _idleChannelPruner->stop();
    }
}

}  // namespace mongo::transport::grpc
