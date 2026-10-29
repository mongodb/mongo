// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/transport/grpc/bidirectional_pipe.h"
#include "mongo/transport/grpc/metadata.h"
#include "mongo/transport/grpc/mock_util.h"
#include "mongo/transport/grpc/server_stream.h"
#include "mongo/util/future.h"
#include "mongo/util/net/hostandport.h"

#include <map>
#include <string>

#include <grpcpp/grpcpp.h>

namespace mongo::transport::grpc {

class MockServerStream : public ServerStream {
public:
    ~MockServerStream() override = default;

    boost::optional<SharedBuffer> read() override;

    bool write(ConstSharedBuffer msg) override;

    explicit MockServerStream(HostAndPort remote,
                              Promise<MetadataContainer>&& initialMetadataPromise,
                              Promise<::grpc::Status>&& rpcTerminationStatusPromise,
                              std::shared_ptr<MockCancellationState> rpcCancellationState,
                              BidirectionalPipe::End&& serverPipeEnd,
                              MetadataContainer clientMetadata);

private:
    friend class MockServerContext;
    friend class MockRPC;

    class InitialMetadata {
    public:
        void insert(const std::string& key, const std::string& value) {
            invariant(!_sent.load());
            _headers.insert({key, value});
        }

        void trySend() {
            if (!_sent.swap(true)) {
                _promise.emplaceValue(std::move(_headers));
            }
        }

    private:
        friend class MockServerStream;

        InitialMetadata(Promise<MetadataContainer> promise)
            : _promise{std::move(promise)}, _sent{false} {}

        Promise<MetadataContainer> _promise;
        /**
         * Whether or not the metadata has been made available to the client end of the stream.
         */
        Atomic<bool> _sent;
        MetadataContainer _headers;
    };

    bool isCancelled() const;

    /**
     * Cancel the RPC associated with this stream. This is used for mocking situations in
     * which an RPC handler was never able to return a final status to the client (e.g. manual
     * cancellation or a network interruption).
     *
     * This method has no effect if the stream is already terminated.
     */
    void cancel(::grpc::Status status);

    /**
     * Closes the stream and sends the final return status of the RPC to the client. This is the
     * mocked equivalent of an RPC handler returning a status.
     *
     * This does not mark the stream as cancelled.
     *
     * This method must only be called once, and this stream must not be used after this method has
     * been called.
     */
    void sendReturnStatus(::grpc::Status status);

    HostAndPort _remote;
    InitialMetadata _initialMetadata;

    /**
     * _rpcReturnStatus is set in sendReturnStatus(), and it is used to mock returning a status from
     * an RPC handler. sendReturnStatus itself is called via MockRPC::sendReturnStatus().
     */
    Promise<::grpc::Status> _rpcReturnStatus;

    /**
     * _finalStatusReturned is also set in sendReturnStatus(), and it is used to denote that a
     * status has been returned and the stream should no longer be used.
     */
    synchronized_value<bool> _finalStatusReturned;

    /**
     * _rpcCancellationState is set via cancel(), which is called by either
     * MockServerContext::tryCancel() or MockRPC::cancel(). It is used to mock situations in which a
     * server RPC handler is unable to return a status to the client (e.g. explicit cancellation or
     * a network interruption).
     */
    std::shared_ptr<MockCancellationState> _rpcCancellationState;

    BidirectionalPipe::End _pipe;
    MetadataContainer _clientMetadata;
    MetadataView _clientMetadataView;
};
}  // namespace mongo::transport::grpc
