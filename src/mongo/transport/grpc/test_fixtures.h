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
#include <memory>
#include <string>

#include "mongo/db/concurrency/locker_noop_service_context_test_fixture.h"
#include "mongo/rpc/message.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/transport/grpc/bidirectional_pipe.h"
#include "mongo/transport/grpc/metadata.h"
#include "mongo/transport/grpc/mock_client_context.h"
#include "mongo/transport/grpc/mock_client_stream.h"
#include "mongo/transport/grpc/mock_server_context.h"
#include "mongo/transport/grpc/mock_server_stream.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/uuid.h"

namespace mongo::transport::grpc {

#define ASSERT_EQ_MSG(a, b) ASSERT_EQ((a).opMsgDebugString(), (b).opMsgDebugString())

inline Message makeUniqueMessage() {
    OpMsg msg;
    msg.body = BSON("id" << UUID::gen().toBSON());
    return msg.serialize();
}

struct MockStreamTestFixtures {
    MockStreamTestFixtures(HostAndPort hostAndPort,
                           Milliseconds timeout,
                           MetadataView clientMetadata) {
        BidirectionalPipe pipe;
        auto promiseAndFuture = makePromiseFuture<MetadataContainer>();

        serverStream = std::make_unique<MockServerStream>(hostAndPort,
                                                          timeout,
                                                          std::move(promiseAndFuture.promise),
                                                          std::move(*pipe.left),
                                                          clientMetadata);
        serverCtx = std::make_unique<MockServerContext>(serverStream.get());

        clientStream = std::make_unique<MockClientStream>(
            hostAndPort, timeout, std::move(promiseAndFuture.future), std::move(*pipe.right));
        clientCtx = std::make_unique<MockClientContext>(clientStream.get());
    }

    std::unique_ptr<MockClientStream> clientStream;
    std::unique_ptr<MockClientContext> clientCtx;
    std::unique_ptr<MockServerStream> serverStream;
    std::unique_ptr<MockServerContext> serverCtx;
};

class ServiceContextWithClockSourceMockTest : public LockerNoopServiceContextTest {
public:
    void setUp() override {
        _clkSource = std::make_shared<ClockSourceMock>();
        getServiceContext()->setFastClockSource(
            std::make_unique<SharedClockSourceAdapter>(_clkSource));
        getServiceContext()->setPreciseClockSource(
            std::make_unique<SharedClockSourceAdapter>(_clkSource));
    }

    auto& clockSource() {
        return *_clkSource;
    }

private:
    std::shared_ptr<ClockSourceMock> _clkSource;
};

}  // namespace mongo::transport::grpc
