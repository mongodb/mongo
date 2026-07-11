// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/stdx/condition_variable.h"
#include "mongo/transport/grpc/client_cache.h"
#include "mongo/transport/grpc/grpc_session.h"
#include "mongo/transport/grpc/serialization.h"
#include "mongo/transport/grpc/wire_version_provider.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/util/modules.h"

#include <list>
#include <mutex>
#include <string_view>

#include <grpcpp/impl/service_type.h>
#include <grpcpp/support/status.h>

namespace mongo::transport {
namespace [[MONGO_MOD_PARENT_PRIVATE]] grpc {
using namespace std::literals::string_view_literals;

/**
 * Base type for all gRPC services, allowing type-specific shutdown and stringifying logic for each
 * service.
 */
class Service : public ::grpc::Service {
public:
    virtual std::string_view name() const = 0;

    virtual void shutdown() = 0;

    virtual void stopAcceptingRequests() = 0;

    std::string toString() const {
        return std::string{name()};
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

    std::string_view name() const override {
        return "mongodb.CommandService"sv;
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

    mutable std::mutex _mutex;
    stdx::condition_variable _shutdownCV;
    std::list<InSessionPtr> _sessions;
    bool _acceptNewRequests = true;
    bool _shutdown = false;
};
}  // namespace grpc
}  // namespace mongo::transport
