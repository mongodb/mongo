// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/shared_buffer.h"

#include <boost/optional.hpp>

namespace mongo::transport::grpc {

/**
 * Base class modeling a synchronous server-side gRPC stream for use by GRPCTransportLayer.
 * See: https://grpc.github.io/grpc/cpp/classgrpc_1_1_server_reader_writer.html
 *
 * ServerStream::read() is thread safe with respect to ServerStream::write(), but neither method
 * should be called concurrently with another invocation of itself on the same stream.
 */
class ServerStream {
public:
    virtual ~ServerStream() {}

    /**
     * Block to read a message from the stream.
     *
     * Returns boost::none if the stream is terminated, either cleanly or due to an underlying
     * connection failure.
     */
    virtual boost::optional<SharedBuffer> read() = 0;

    /**
     * Block to write a message to the stream.
     *
     * Returns true if the write was successful, and false if it failed due to the stream being
     * closed, either explicitly or due to an underlying connection failure.
     */
    virtual bool write(ConstSharedBuffer msg) = 0;
};
}  // namespace mongo::transport::grpc
