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
#include "mongo/transport/grpc/grpc_transport_layer.h"
#include "mongo/transport/grpc/wire_version_provider.h"

namespace mongo::transport::grpc {

/**
 * Base type for all gRPC services, allowing type-specific shutdown and stringifying logic for each
 * service.
 */
class Service : public ::grpc::Service {
public:
    virtual StringData name() const = 0;

    virtual void shutdown() = 0;

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
    using RpcHandler = std::function<::grpc::Status(InSessionPtr)>;

    static constexpr const char* kAuthenticatedCommandStreamMethodName =
        "/mongodb.CommandService/AuthenticatedCommandStream";
    static constexpr const char* kUnauthenticatedCommandStreamMethodName =
        "/mongodb.CommandService/UnauthenticatedCommandStream";

    // Client-provided metadata keys.
    static constexpr StringData kAuthenticationTokenKey = "authorization"_sd;
    static constexpr StringData kClientIdKey = "mongodb-clientid"_sd;
    static constexpr StringData kClientMetadataKey = "mongodb-client"_sd;
    static constexpr StringData kWireVersionKey = "mongodb-wireversion"_sd;

    // Server-provided metadata keys.
    // This is defined as a std::string instead of StringData to avoid having to copy it when
    // passing to gRPC APIs that expect a const std::string&.
    static const std::string kClusterMaxWireVersionKey;

    /**
     * The provided callback is used to handle streams created from both methods. The status
     * returned from the callback will be communicated to the client. The callback MUST terminate
     * the session before returning.
     */
    CommandService(GRPCTransportLayer* tl,
                   RpcHandler callback,
                   std::shared_ptr<WireVersionProvider> wvProvider);

    ~CommandService() = default;

    StringData name() const override {
        return "mongodb.CommandService"_sd;
    }

    void shutdown() override;

private:
    ::grpc::Status _handleStream(ServerContext& serverCtx, ServerStream& stream);

    ::grpc::Status _handleAuthenticatedStream(ServerContext& serverCtx, ServerStream& stream);

    GRPCTransportLayer* _tl;
    RpcHandler _callback;
    std::shared_ptr<WireVersionProvider> _wvProvider;
    std::unique_ptr<ClientCache> _clientCache;

    mutable stdx::mutex _mutex;  // NOLINT
    stdx::condition_variable _shutdownCV;
    std::list<InSessionPtr> _sessions;
    bool _shutdown = false;
};
}  // namespace mongo::transport::grpc

namespace grpc {

template <>
class SerializationTraits<mongo::ConstSharedBuffer, void> {
public:
    static Status Serialize(const mongo::ConstSharedBuffer& source,
                            ByteBuffer* buffer,
                            bool* own_buffer) {
        // Make a shallow copy of the input buffer to increment its reference count and ensure the
        // data stays alive for at least as long as the slice referencing it does.
        auto copy = std::make_unique<mongo::ConstSharedBuffer>(source);
        ::grpc::Slice slice{(void*)source.get(), source.capacity(), Destroy, copy.release()};
        ::grpc::ByteBuffer tmp{&slice, /* n_slices */ 1};
        buffer->Swap(&tmp);
        *own_buffer = true;
        return ::grpc::Status::OK;
    }

private:
    static void Destroy(void* data) {
        std::unique_ptr<mongo::ConstSharedBuffer> buf{static_cast<mongo::ConstSharedBuffer*>(data)};
    }
};

/**
 * (De)serialization implementations required to use SharedBuffer with streams provided by gRPC.
 * See: https://grpc.github.io/grpc/cpp/classgrpc_1_1_serialization_traits.html
 */
template <>
class SerializationTraits<mongo::SharedBuffer> {
public:
    static Status Deserialize(ByteBuffer* byte_buffer, mongo::SharedBuffer* dest) {
        Slice singleSlice;
        if (byte_buffer->TrySingleSlice(&singleSlice).ok()) {
            dest->realloc(singleSlice.size());
            std::memcpy(dest->get(), singleSlice.begin(), singleSlice.size());
            return ::grpc::Status::OK;
        }

        std::vector<Slice> slices;
        auto status = byte_buffer->Dump(&slices);

        if (!status.ok()) {
            return status;
        }

        size_t total =
            std::accumulate(slices.begin(), slices.end(), 0, [](size_t runningTotal, Slice& slice) {
                return runningTotal + slice.size();
            });
        dest->realloc(total);
        size_t index = 0;
        for (auto& s : slices) {
            std::memcpy(dest->get() + index, s.begin(), s.size());
            index += s.size();
        }
        return ::grpc::Status::OK;
    }
};
}  // namespace grpc
