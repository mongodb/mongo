// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/transport/grpc/metadata.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"

#include <map>
#include <string>

namespace mongo::transport::grpc {

/**
 * Base class modeling a gRPC ServerContext.
 * See: https://grpc.github.io/grpc/cpp/classgrpc_1_1_server_context.html
 */
class ServerContext {
public:
    virtual ~ServerContext() {}

    /**
     * Set the server's initial metadata.
     * This must only be called before the first message is sent using the corresponding
     * ServerStream.
     *
     * This method is not thread safe with respect to ServerStream::write().
     */
    virtual void addInitialMetadataEntry(const std::string& key, const std::string& value) = 0;
    virtual const MetadataView& getClientMetadata() const = 0;
    virtual Date_t getDeadline() const = 0;
    virtual HostAndPort getRemote() const = 0;

    /**
     * Attempt to cancel the RPC this context is associated with. This may not have an effect if the
     * RPC handler already returned a successful status to the client.
     *
     * This is thread-safe.
     */
    virtual void tryCancel() = 0;

    /**
     * Return true if the RPC associated with this ServerContext failed before the RPC handler could
     * return its final status back to the client (e.g. due to explicit cancellation or a network
     * issue).
     *
     * If the handler was able to return a status successfully, even if that status was
     * Status::CANCELLED, then this method will return false.
     */
    virtual bool isCancelled() const = 0;
};

}  // namespace mongo::transport::grpc
