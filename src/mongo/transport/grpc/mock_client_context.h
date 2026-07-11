// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/transport/grpc/client_context.h"
#include "mongo/transport/grpc/mock_client_stream.h"

namespace mongo::transport::grpc {

class MockClientContext : public ClientContext {
public:
    MockClientContext() : _deadline{Date_t::max()}, _stream{nullptr} {}

    void addMetadataEntry(const std::string& key, const std::string& value) override {
        invariant(!_stream);
        _metadata.insert({key, value});
    };

    MetadataView getServerInitialMetadata() const override {
        invariant(_stream);
        invariant(_stream->_serverInitialMetadata.isReady());

        MetadataView mv;
        for (auto& kvp : _stream->_serverInitialMetadata.get()) {
            mv.insert({kvp.first, kvp.second});
        }
        return mv;
    }

    Date_t getDeadline() const override {
        return _deadline;
    }

    void setDeadline(Date_t deadline) override {
        invariant(!_stream);
        _deadline = deadline;
    }

    HostAndPort getRemote() const override {
        invariant(_stream);
        return _stream->_remote;
    }

    void tryCancel() override {
        invariant(_stream);
        _stream->_cancel();
    }

private:
    friend class MockStub;
    friend struct MockStreamTestFixtures;

    Date_t _deadline;
    MetadataContainer _metadata;
    std::shared_ptr<MockClientStream> _stream;
};

}  // namespace mongo::transport::grpc
