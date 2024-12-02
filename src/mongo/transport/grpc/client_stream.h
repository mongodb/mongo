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

#include "mongo/transport/grpc/reactor.h"
#include "mongo/util/shared_buffer.h"

namespace mongo::transport::grpc {

/**
 * Base class modeling an asynchronous client side of a gRPC stream.
 * See: https://grpc.github.io/grpc/cpp/classgrpc_1_1_client_async_reader_writer.html
 *
 * This base class is defined instead of using the ClientAsyncReaderWriter directly in order to
 * facilitate mocking.
 */
class ClientStream {
public:
    virtual ~ClientStream() = default;

    /**
     * Read a message of type R into msg.
     *
     * Completion will be notified by tag on the associated completion queue. This is thread-safe
     * with respect to Write or WritesDone methods. It should not be called concurrently with other
     * streaming APIs on the same stream. It is not meaningful to call it concurrently with another
     * read on the same stream since reads on the same stream are delivered in order.
     *
     * The associated completion queue notification will have an ok=false status if the read failed
     * due to the stream being closed, either cleanly or due to an underlying connection failure.
     */
    virtual void read(SharedBuffer* msg, GRPCReactor::CompletionQueueEntry* tag) = 0;

    /**
     * Request the writing of msg with an identifying tag.
     *
     * Only one write may be outstanding at any given time. This means that after calling Write, one
     * must wait to receive tag from the completion queue BEFORE calling Write again. This is
     * thread-safe with respect to read.
     *
     * gRPC doesn't take ownership or a reference to msg, so it is safe to to deallocate once Write
     * returns.
     *
     * The associated completion queue notification will have an ok=false status if the
     * write failed due to the stream being closed, either explicitly or due to an underlying
     * connection failure.
     */
    virtual void write(ConstSharedBuffer msg, GRPCReactor::CompletionQueueEntry* tag) = 0;

    /**
     * Indicate that the stream is to be finished and request notification for when the call has
     * been ended.
     *
     * Should not be used concurrently with other operations.
     *
     * It is appropriate to call this method exactly once when both:
     *  - the client side has no more message to send (this is declared implicitly by calling this
     * method or explicitly through an earlier call to the writesDone method).
     *  - there are no more messages to be received from the server (this is known from an earlier
     * call to read that yielded a failed result, e.g. cq->Next(&read_tag, &ok) filled in 'ok' with
     * 'false').
     *
     * The tag will be returned when either:
     *  - all incoming messages have been read and the server has returned a status.
     *  - the server has returned a non-OK status.
     *  - the call failed for some reason and the library generated a status.
     */
    virtual void finish(::grpc::Status* status, GRPCReactor::CompletionQueueEntry* tag) = 0;

    /**
     * Signal the client is done with the writes (half-close the client stream).
     *
     * Thread-safe with respect to read.
     */
    virtual void writesDone(GRPCReactor::CompletionQueueEntry* tag) = 0;
};

}  // namespace mongo::transport::grpc
