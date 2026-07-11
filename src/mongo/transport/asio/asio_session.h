// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/system_error.h"
#include "mongo/config.h"
#include "mongo/transport/asio/asio_transport_layer.h"
#include "mongo/transport/asio/asio_utils.h"
#include "mongo/transport/session.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/util/demangle.h"
#include "mongo/util/fail_point.h"

namespace mongo::transport {

extern FailPoint asioTransportLayerShortOpportunisticReadWrite;
extern FailPoint asioTransportLayerSessionPauseBeforeSetSocketOption;

template <typename SuccessValue>
auto futurize(const std::error_code& ec, SuccessValue&& successValue) {
    using T = std::decay_t<SuccessValue>;
    if (MONGO_unlikely(ec)) {
        static StaticImmortal memo = fmt::format("futurize<{}>", demangleName(typeid(T)));
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
 */
class AsioSession : public Session {
public:
    using GenericSocket = asio::generic::stream_protocol::socket;
    using Endpoint = asio::generic::stream_protocol::endpoint;

protected:
    friend class AsioTransportLayer;
    friend class AsioNetworkingBaton;

    AsioSession(bool isIngress) : Session(isIngress) {}

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
    virtual Future<void> handshakeSSLForEgress(const HostAndPort& target) = 0;
#endif

    /** Returns the ASIO socket underlying this Session. */
    virtual GenericSocket& getSocket() = 0;

    virtual ExecutorFuture<void> parseProxyProtocolHeader(const ReactorHandle& reactor) = 0;
};

}  // namespace mongo::transport
