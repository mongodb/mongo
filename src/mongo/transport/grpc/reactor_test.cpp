/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/service_context_test_fixture.h"
#include "mongo/transport/grpc/grpc_transport_layer_impl.h"
#include "mongo/transport/grpc/test_fixtures.h"
#include "mongo/transport/reactor_test_fixture.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

namespace mongo::transport::grpc {
namespace {
class GRPCReactorTest : public ReactorTestFixture {
public:
    void setUp() override {
        grpc::GRPCTransportLayer::Options grpcOpts;
        grpcOpts.enableEgress = true;
        grpcOpts.clientMetadata = makeClientMetadataDocument();
        _tl = std::make_unique<grpc::GRPCTransportLayerImpl>(
            /* serviceContext */ nullptr, std::move(grpcOpts), /*sessionManager*/ nullptr);
    }

    TransportLayer* getTransportLayer() override {
        return _tl.get();
    }

private:
    std::unique_ptr<GRPCTransportLayer> _tl;
};

TEST_F(GRPCReactorTest, BasicSchedule) {
    testBasicSchedule();
}

TEST_F(GRPCReactorTest, DrainTask) {
    testDrainTask();
}

TEST_F(GRPCReactorTest, RecordsTaskStats) {
    testRecordsTaskStats();
}

TEST_F(GRPCReactorTest, OnReactorThread) {
    testOnReactorThread();
}

TEST_F(GRPCReactorTest, ScheduleOnReactorAfterShutdownFails) {
    testScheduleOnReactorAfterShutdownFails();
}

TEST_F(GRPCReactorTest, BasicTimer) {
    testBasicTimer();
}

TEST_F(GRPCReactorTest, BasicTimerCancel) {
    testBasicTimerCancel();
}

TEST_F(GRPCReactorTest, SchedulingTwiceOnTimerCancelsFirstOne) {
    testSchedulingTwiceOnTimerCancelsFirstOne();
}

TEST_F(GRPCReactorTest, UseTimerAfterReactorShutdown) {
    testUseTimerAfterReactorShutdown();
}

TEST_F(GRPCReactorTest, SafeToUseTimerAfterReactorDestruction) {
    testSafeToUseTimerAfterReactorDestruction();
}

}  // namespace
}  // namespace mongo::transport::grpc
