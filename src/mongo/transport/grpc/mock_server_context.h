// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/transport/grpc/mock_server_stream.h"
#include "mongo/transport/grpc/server_context.h"

namespace mongo::transport::grpc {

class MockServerContext : public ServerContext {
public:
    explicit MockServerContext(MockServerStream* stream) : _stream{stream} {}
    ~MockServerContext() override = default;

    void addInitialMetadataEntry(const std::string& key, const std::string& value) override;
    const MetadataView& getClientMetadata() const override;
    Date_t getDeadline() const override;
    bool isCancelled() const override;
    HostAndPort getRemote() const override {
        return _stream->_remote;
    }
    void tryCancel() override;

private:
    MockServerStream* _stream;
};

}  // namespace mongo::transport::grpc
