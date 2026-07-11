// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/transport/grpc/server_stream.h"

#include <grpcpp/support/sync_stream.h>

namespace mongo::transport::grpc {

class GRPCServerStream : public ServerStream {
public:
    explicit GRPCServerStream(::grpc::ServerReaderWriter<ConstSharedBuffer, SharedBuffer>* stream)
        : _stream{stream} {}

    ~GRPCServerStream() override = default;

    boost::optional<SharedBuffer> read() override {
        SharedBuffer msg;
        if (_stream->Read(&msg)) {
            return std::move(msg);
        } else {
            return boost::none;
        }
    };

    bool write(ConstSharedBuffer msg) override {
        return _stream->Write(msg);
    }

private:
    ::grpc::ServerReaderWriter<ConstSharedBuffer, SharedBuffer>* _stream;
};
}  // namespace mongo::transport::grpc
