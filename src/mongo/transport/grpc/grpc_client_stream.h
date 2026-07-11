// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/transport/grpc/client_stream.h"

#include <grpcpp/support/async_stream.h>

namespace mongo::transport::grpc {

class GRPCClientStream : public ClientStream {
public:
    explicit GRPCClientStream(
        std::unique_ptr<::grpc::ClientAsyncReaderWriter<ConstSharedBuffer, SharedBuffer>> stream)
        : _stream{std::move(stream)} {}

    void read(SharedBuffer* msg, GRPCReactor::CompletionQueueEntry* tag) override {
        _stream->Read(msg, tag);
    };

    void write(ConstSharedBuffer msg, GRPCReactor::CompletionQueueEntry* tag) override {
        _stream->Write(msg, tag);
    }

    void finish(::grpc::Status* status, GRPCReactor::CompletionQueueEntry* tag) override {
        _stream->Finish(status, tag);
    }

    void writesDone(GRPCReactor::CompletionQueueEntry* tag) override {
        _stream->WritesDone(tag);
    }

    void startCall(GRPCReactor::CompletionQueueEntry* tag) override {
        _stream->StartCall(tag);
    }

private:
    std::unique_ptr<::grpc::ClientAsyncReaderWriter<ConstSharedBuffer, SharedBuffer>> _stream;
};
}  // namespace mongo::transport::grpc
