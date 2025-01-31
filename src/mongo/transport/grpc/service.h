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

#include <list>

#include <grpcpp/impl/service_type.h>
#include <grpcpp/support/status.h>

#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/transport/grpc/client_cache.h"
#include "mongo/transport/grpc/grpc_session.h"
#include "mongo/transport/grpc/serialization.h"
#include "mongo/transport/grpc/wire_version_provider.h"
#include "mongo/transport/transport_layer.h"

namespace mongo::transport::grpc {

/**
 * Base type for all gRPC services, allowing type-specific shutdown and stringifying logic for each
 * service.
 */
class Service : public ::grpc::Service {
public:
    virtual StringData name() const = 0;

    virtual void shutdown() = 0;

    virtual void stopAcceptingRequests() = 0;

    std::string toString() const {
        return name().toString();
    }
};

inline std::string toStringForLogging(const std::unique_ptr<Service>& service) {
    return service->toString();
}

/**
 * A gRPC service definition for handling commands according to the MongoDB gRPC Protocol.
 * The service's name is "MongoDB", and it provides two methods: "UnauthenticatedCommandStream" and
 * "AuthenticatedCommandStream", both of which are bidirectional streaming methods.
 *
 * These methods use SharedBuffer as the message type (not a protocol buffer), the contents of which
 * are either OP_MSG or OP_COMPRESSED encoded bytes.
 */
class CommandService : public Service {
public:
    using InSessionPtr = std::shared_ptr<IngressSession>;
    using RPCHandler = std::function<void(InSessionPtr)>;

    /**
     * The provided callback is used to handle streams created from both methods. The status
     * returned from the callback will be communicated to the client. The callback MUST terminate
     * the session before returning.
     *
     * The session's termination status will be converted to the closest matching gRPC status and
     * returned to the client once the handler exits. This conversion is lossy though, so it is
     * better to communicate errors to the client by writing messages to the stream rather than by
     * setting a termination status.
     */
    CommandService(TransportLayer* tl,
                   RPCHandler callback,
                   std::shared_ptr<WireVersionProvider> wvProvider,
                   std::shared_ptr<ClientCache> clientCache = nullptr);

    ~CommandService() override = default;

    StringData name() const override {
        return "mongodb.CommandService"_sd;
    }

    void shutdown() override;

    void stopAcceptingRequests() override;

private:
    friend class MockServer;

    ::grpc::Status _handleStream(ServerContext& serverCtx,
                                 ServerStream& stream,
                                 boost::optional<std::string> authToken = boost::none);

    ::grpc::Status _handleAuthenticatedStream(ServerContext& serverCtx, ServerStream& stream);

    TransportLayer* _tl;
    RPCHandler _callback;
    std::shared_ptr<WireVersionProvider> _wvProvider;
    std::shared_ptr<ClientCache> _clientCache;

    mutable stdx::mutex _mutex;
    stdx::condition_variable _shutdownCV;
    std::list<InSessionPtr> _sessions;
    bool _acceptNewRequests = true;
    bool _shutdown = false;
};
}  // namespace mongo::transport::grpc
