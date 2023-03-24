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

#include <map>
#include <string>

#include "mongo/transport/grpc/bidirectional_pipe.h"
#include "mongo/transport/grpc/metadata.h"
#include "mongo/transport/grpc/server_stream.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/future.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"

namespace mongo::transport::grpc {

class MockServerStream : public ServerStream {
public:
    ~MockServerStream() = default;

    boost::optional<SharedBuffer> read() override;
    bool write(ConstSharedBuffer msg) override;

    explicit MockServerStream(HostAndPort hostAndPort,
                              Milliseconds timeout,
                              Promise<MetadataContainer>&& initialMetadataPromise,
                              BidirectionalPipe::End&& serverPipeEnd,
                              MetadataView clientMetadata);

private:
    friend class MockServerContext;

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
        AtomicWord<bool> _sent;
        MetadataContainer _headers;
    };

    bool isCancelled() const;
    void close();

    CancellationSource _cancellationSource;
    Date_t _deadline;
    InitialMetadata _initialMetadata;
    BidirectionalPipe::End _pipe;
    MetadataView _clientMetadata;
    HostAndPort _hostAndPort;
};
}  // namespace mongo::transport::grpc
