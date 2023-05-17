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

#include <boost/optional.hpp>
#include <grpcpp/grpcpp.h>

#include "mongo/util/shared_buffer.h"

namespace mongo::transport::grpc {

/**
 * Base class modeling a synchronous client side of a gRPC stream.
 * See: https://grpc.github.io/grpc/cpp/classgrpc_1_1_client_reader_writer.html
 *
 * ClientStream::read() is thread safe with respect to ClientStream::write(), but neither method
 * should be called concurrently with another invocation of itself on the same stream.
 *
 * ClientStream::finish() is thread safe with respect to ClientStream::read().
 */
class ClientStream {
public:
    virtual ~ClientStream() = default;

    /**
     * Block to read a message from the stream.
     *
     * Returns boost::none if the stream is closed, either cleanly or due to an underlying
     * connection failure.
     */
    virtual boost::optional<SharedBuffer> read() = 0;

    /**
     * Block to write a message to the stream.
     *
     * Returns true if the write was successful or false if it failed due to the stream being
     * closed, either explicitly or due to an underlying connection failure.
     */
    virtual bool write(ConstSharedBuffer msg) = 0;

    /**
     * Block waiting until all received messages have been read and the stream has been closed.
     *
     * Returns the final status of the RPC associated with this stream.
     *
     * This method should only be called once.
     */
    virtual ::grpc::Status finish() = 0;
};

}  // namespace mongo::transport::grpc
