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

#include "mongo/transport/grpc/client_stream.h"

#include "mongo/transport/grpc/bidirectional_pipe.h"
#include "mongo/transport/grpc/metadata.h"
#include "mongo/transport/grpc/mock_util.h"
#include "mongo/util/future.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/synchronized_value.h"
#include "mongo/util/time_support.h"

namespace mongo::transport::grpc {

/**
 * The MockClientStream provides mock networking functionality through a pipe. Despite mocking the
 * ClientAsyncReaderWriter API, these functions currently block, and then fulfil the Promise
 * associated with the CompletionQueueEntry tag once the blocking work has completed. This may be
 * modified in the future by introducing a mock grpc::CompletionQueue, but is not strictly required
 * for useful unit testing.
 */
class MockClientStream : public ClientStream {
public:
    MockClientStream(HostAndPort remote,
                     Future<MetadataContainer>&& serverInitialMetadata,
                     Future<::grpc::Status>&& rpcReturnStatus,
                     std::shared_ptr<MockCancellationState> rpcCancellationState,
                     BidirectionalPipe::End&& pipe);

    void read(SharedBuffer* msg, GRPCReactor::CompletionQueueEntry* tag) override;

    void write(ConstSharedBuffer msg, GRPCReactor::CompletionQueueEntry* tag) override;

    void finish(::grpc::Status* status, GRPCReactor::CompletionQueueEntry* tag) override;

    void writesDone(GRPCReactor::CompletionQueueEntry* tag) override;


    // The below sync APIs are provided to enable easier unit testing.
    boost::optional<SharedBuffer> syncRead(const std::shared_ptr<GRPCReactor>& reactor);

    bool syncWrite(const std::shared_ptr<GRPCReactor>& reactor, ConstSharedBuffer msg);

    ::grpc::Status syncFinish(const std::shared_ptr<GRPCReactor>& reactor);

private:
    friend class MockClientContext;

    void _cancel();

    HostAndPort _remote;
    MetadataContainer _clientMetadata;
    Future<MetadataContainer> _serverInitialMetadata;

    /**
     * The mocked equivalent of a status returned from a server-side RPC handler.
     */
    Future<::grpc::Status> _rpcReturnStatus;

    /**
     * State used to mock RPC cancellation, including explicit cancellation (client or server side)
     * or network errors.
     */
    std::shared_ptr<MockCancellationState> _rpcCancellationState;

    BidirectionalPipe::End _pipe;
};
}  // namespace mongo::transport::grpc
