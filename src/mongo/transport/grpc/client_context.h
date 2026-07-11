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
 * Base class modeling a gRPC ClientContext.
 * See: https://grpc.github.io/grpc/cpp/classgrpc_1_1_client_context.html
 */
class ClientContext {
public:
    virtual ~ClientContext() = default;

    /**
     * Add an entry to the metadata associated with the RPC.
     *
     * This must only be called before invoking the RPC.
     */
    virtual void addMetadataEntry(const std::string& key, const std::string& value) = 0;

    /**
     * Retrieve the server's initial metadata.
     *
     * This must only be called after the first message has been received on the ClientStream
     * created from the RPC that this context is associated with.
     */
    virtual MetadataView getServerInitialMetadata() const = 0;

    /**
     * Set the deadline for the RPC to be executed using this context.
     *
     * This must only be called before invoking the RPC.
     */
    virtual void setDeadline(Date_t deadline) = 0;

    virtual Date_t getDeadline() const = 0;

    /**
     * Get the address of the remote server.
     * This must only be called after the RPC associated with this context has been invoked.
     */
    virtual HostAndPort getRemote() const = 0;

    /**
     * Send a best-effort out-of-band cancel on the call associated with this ClientContext. There
     * is no guarantee the call will be cancelled (e.g. if the call has already finished by the time
     * the cancellation is received).
     *
     * Note that tryCancel() will not impede the execution of any already scheduled work (e.g.
     * messages already queued to be sent on a stream will still be sent), though the reported
     * sucess or failure of such work may reflect the cancellation.
     *
     * This method is thread-safe, and can be called multiple times from any thread. It should not
     * be called before this ClientContext has been used to invoke an RPC.
     *
     * See:
     * https://grpc.github.io/grpc/cpp/classgrpc_1_1_client_context.html#abd0f6715c30287b75288015eee628984
     */
    virtual void tryCancel() = 0;
};
}  // namespace mongo::transport::grpc
