// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
