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

#include <cstring>
#include <functional>
#include <numeric>

#include <grpcpp/grpcpp.h>
#include <grpcpp/impl/rpc_method.h>
#include <grpcpp/impl/rpc_service_method.h>
#include <grpcpp/impl/service_type.h>
#include <grpcpp/support/byte_buffer.h>
#include <grpcpp/support/method_handler.h>
#include <grpcpp/support/status.h>
#include <grpcpp/support/sync_stream.h>

#include "mongo/transport/grpc/grpc_server_context.h"
#include "mongo/transport/grpc/grpc_server_stream.h"
#include "mongo/transport/grpc/server_context.h"
#include "mongo/transport/grpc/server_stream.h"
#include "mongo/util/shared_buffer.h"

namespace mongo::transport::grpc {

/**
 * A gRPC service definition for handling commands according to the MongoDB gRPC Protocol.
 * The service's name is "MongoDB", and it provides two methods: "UnauthenticatedCommandStream" and
 * "AuthenticatedCommandStream", both of which are bidirectional streaming methods.
 *
 * These methods use SharedBuffer as the message type (not a protocol buffer), the contents of which
 * are either OP_MSG or OP_COMPRESSED encoded bytes.
 */
class Service : public ::grpc::Service {
public:
    using RpcHandler = std::function<::grpc::Status(ServerContext&, ServerStream&)>;

    static constexpr const char* kAuthenticatedCommandStreamMethodName =
        "/MongoDB/AuthenticatedCommandStream";
    static constexpr const char* kUnauthenticatedCommandStreamMethodName =
        "/MongoDB/UnauthenticatedCommandStream";

    Service(RpcHandler unauthenticatedCommandStreamHandler,
            RpcHandler authenticatedCommandStreamHandler)
        : _unauthenticatedCommandStreamHandler{std::move(unauthenticatedCommandStreamHandler)},
          _authenticatedCommandStreamHandler{std::move(authenticatedCommandStreamHandler)} {

        AddMethod(new ::grpc::internal::RpcServiceMethod(
            kAuthenticatedCommandStreamMethodName,
            ::grpc::internal::RpcMethod::BIDI_STREAMING,
            new ::grpc::internal::BidiStreamingHandler<Service, SharedBuffer, ConstSharedBuffer>(
                [](Service* service,
                   ::grpc::ServerContext* nativeServerCtx,
                   ::grpc::ServerReaderWriter<ConstSharedBuffer, SharedBuffer>*
                       nativeServerStream) {
                    GRPCServerContext ctx{nativeServerCtx};
                    GRPCServerStream stream{nativeServerStream};

                    return service->_authenticatedCommandStreamHandler(ctx, stream);
                },
                this)));

        AddMethod(new ::grpc::internal::RpcServiceMethod(
            kUnauthenticatedCommandStreamMethodName,
            ::grpc::internal::RpcMethod::BIDI_STREAMING,
            new ::grpc::internal::BidiStreamingHandler<Service, SharedBuffer, ConstSharedBuffer>(
                [](Service* service,
                   ::grpc::ServerContext* nativeServerCtx,
                   ::grpc::ServerReaderWriter<ConstSharedBuffer, SharedBuffer>*
                       nativeServerStream) {
                    GRPCServerContext ctx{nativeServerCtx};
                    GRPCServerStream stream{nativeServerStream};

                    return service->_unauthenticatedCommandStreamHandler(ctx, stream);
                },
                this)));
    }

    ~Service() = default;

private:
    RpcHandler _unauthenticatedCommandStreamHandler;
    RpcHandler _authenticatedCommandStreamHandler;
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
