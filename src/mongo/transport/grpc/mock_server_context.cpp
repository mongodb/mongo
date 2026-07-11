// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/transport/grpc/mock_server_context.h"

namespace mongo::transport::grpc {

void MockServerContext::addInitialMetadataEntry(const std::string& key, const std::string& value) {
    _stream->_initialMetadata.insert(key, value);
}

const MetadataView& MockServerContext::getClientMetadata() const {
    return _stream->_clientMetadataView;
}

Date_t MockServerContext::getDeadline() const {
    return _stream->_rpcCancellationState->getDeadline();
}

void MockServerContext::tryCancel() {
    _stream->cancel(::grpc::Status::CANCELLED);
}

bool MockServerContext::isCancelled() const {
    return _stream->isCancelled();
}

}  // namespace mongo::transport::grpc
