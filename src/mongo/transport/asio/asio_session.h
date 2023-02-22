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

#include "mongo/base/system_error.h"
#include "mongo/config.h"
#include "mongo/transport/asio/asio_transport_layer.h"
#include "mongo/transport/asio/asio_utils.h"
#include "mongo/transport/session.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/util/fail_point.h"

namespace mongo::transport {

extern FailPoint asioTransportLayerShortOpportunisticReadWrite;
extern FailPoint asioTransportLayerSessionPauseBeforeSetSocketOption;

template <typename SuccessValue>
auto futurize(const std::error_code& ec, SuccessValue&& successValue) {
    using T = std::decay_t<SuccessValue>;
    if (MONGO_unlikely(ec)) {
        using namespace fmt::literals;
        static StaticImmortal memo = "futurize<{}>"_format(demangleName(typeid(T)));
        return Future<T>::makeReady(errorCodeToStatus(ec, *memo));
    }
    return Future<T>::makeReady(std::forward<SuccessValue>(successValue));
}

inline Future<void> futurize(const std::error_code& ec) {
    using Result = Future<void>;
    if (MONGO_unlikely(ec)) {
        return Result::makeReady(errorCodeToStatus(ec, "futurize"));
    }
    return Result::makeReady();
}

/**
 * Extends the interface of transport::Session to include any ASIO-specific functionality. The
 * intention of this class is to expose any ASIO-specific information necessary to systems outside
 * of the transport layer, separate from other implementation details.
 *
 * NOTE: Moving forward, the expectation is to expand this interface and reduce the interface of
 * Session:
 * TODO(SERVER-71100): Move ASIO-specific `remoteAddr` and `localAddr` into AsioSession.
 */
class AsioSession : public Session {
public:
    using GenericSocket = asio::generic::stream_protocol::socket;
    using Endpoint = asio::generic::stream_protocol::endpoint;

protected:
    friend class AsioTransportLayer;
    friend class AsioNetworkingBaton;

    /** Notifies the Session that it will be used in a synchronous way. */
    virtual void ensureSync() = 0;

    /** Notifies the Session that it will be used in an asynchronous way. */
    virtual void ensureAsync() = 0;

#ifdef MONGO_CONFIG_SSL
    /** Constructs an SSL socket required to initiate SSL handshake for egress connections. */
    virtual Status buildSSLSocket(const HostAndPort& target) = 0;

    /**
     * Instructs the Session to initiate an SSL handshake. Returns a Future that is emplaced once
     * the handshake is complete, or when some error has arisen.
     */
    virtual Future<void> handshakeSSLForEgress(const HostAndPort& target,
                                               const ReactorHandle& reactor) = 0;
#endif

    /** Returns the ASIO socket underlying this Session. */
    virtual GenericSocket& getSocket() = 0;

    virtual ExecutorFuture<void> parseProxyProtocolHeader(const ReactorHandle& reactor) = 0;
};

}  // namespace mongo::transport
