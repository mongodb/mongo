// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/transport/grpc/bidirectional_pipe.h"
#include "mongo/transport/grpc/client_stream.h"
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
                     BidirectionalPipe::End&& pipe,
                     const std::shared_ptr<GRPCReactor>& reactor);

    void read(SharedBuffer* msg, GRPCReactor::CompletionQueueEntry* tag) override;

    void write(ConstSharedBuffer msg, GRPCReactor::CompletionQueueEntry* tag) override;

    void finish(::grpc::Status* status, GRPCReactor::CompletionQueueEntry* tag) override;

    void writesDone(GRPCReactor::CompletionQueueEntry* tag) override;

    void startCall(GRPCReactor::CompletionQueueEntry* tag) override;

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

    std::shared_ptr<GRPCReactor> _reactor;
};
}  // namespace mongo::transport::grpc
