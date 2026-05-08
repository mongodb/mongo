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

#include "mongo/s/service_entry_point_router_role.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/db/admission/ingress_request_rate_limiter.h"
#include "mongo/db/admission/rate_limiter.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/stdx/thread.h"
#include "mongo/transport/service_entry_point.h"
#include "mongo/transport/service_entry_point_test_fixture.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/tick_source_mock.h"

namespace mongo {
namespace {

using namespace admission;

class TestCmdRouterIngressSubject final : public TestCmdBase {
public:
    static constexpr auto kCommandName = "testRouterIngressSubject";
    TestCmdRouterIngressSubject() : TestCmdBase(kCommandName) {}

    bool isSubjectToIngressAdmissionControl() const override {
        return true;
    }

    bool runWithBuilderOnly(BSONObjBuilder&) override {
        return true;
    }
};
MONGO_REGISTER_COMMAND(TestCmdRouterIngressSubject).testOnly().forRouter();

class ServiceEntryPointRouterRoleTest : public virtual service_context_test::RouterRoleOverride,
                                        public ServiceEntryPointTestFixture {
public:
    void setUp() override {
        ServiceEntryPointTestFixture::setUp();
        auto routerService = getGlobalServiceContext()->getService();
        ReadWriteConcernDefaults::create(routerService, _lookupMock.getFetchDefaultsFn());
        _lookupMock.setLookupCallReturnValue({});

        getGlobalServiceContext()->getService()->setServiceEntryPoint(
            std::make_unique<ServiceEntryPointRouterRole>());
    }
    void assertResponseForClusterAndOperationTime(BSONObj response) override {
        // For ServiceEntryPointRouterRole, either cluster time or operation time will be present,
        // or neither field.
        ASSERT_FALSE(response.hasField("$clusterTime") && response.hasField("operationTime"))
            << ", response: " << response;
    }
    void assertResponseStatus(BSONObj response,
                              Status expectedStatus,
                              OperationContext* opCtx) override {
        auto status = getStatusFromCommandResult(response);
        // TODO SERVER-97138 there are some differences between shard and router SEPs with
        // regard to when errInfo is recorded. Once those are resolved, this function can be
        // a non-virtual member of ServiceEntryPointTestFixture.
        // ASSERT_EQ(CurOp::get(opCtx)->debug().errInfo, expectedStatus);
        ASSERT_EQ(status, expectedStatus);
    }
};

TEST_F(ServiceEntryPointRouterRoleTest, TestCommandSucceeds) {
    testCommandSucceeds();
}

TEST_F(ServiceEntryPointRouterRoleTest, TestCommandFailsRunInvocationWithResponse) {
    testCommandFailsRunInvocationWithResponse();
}

TEST_F(ServiceEntryPointRouterRoleTest, TestCommandFailsRunInvocationWithException) {
    testCommandFailsRunInvocationWithException("Exception thrown while processing command");
}

TEST_F(ServiceEntryPointRouterRoleTest, WaitForAdmissionResolvesDeferredTokenBeforeInvocation) {
    RAIIServerParameterControllerForTest requestLimiterEnabled{"ingressRequestRateLimiterEnabled",
                                                               true};

    RateLimiter limiterForDeferredToken(
        /*refreshRatePerSec=*/1.0,
        /*burstCapacitySecs=*/1.0,
        /*maxQueueDepth=*/2,
        "WaitForAdmissionResolvesDeferredTokenBeforeInvocation");

    auto opCtx = makeOperationContext();
    ASSERT_OK(limiterForDeferredToken.acquireToken(opCtx.get()));
    auto queuedTokenResult = limiterForDeferredToken.acquireToken();
    ASSERT_OK(queuedTokenResult.getStatus());
    ASSERT_FALSE(queuedTokenResult.getValue().isReady());
    IngressRequestRateLimiter::setDeferredAdmissionToken_forTest(
        opCtx->getClient(), std::move(queuedTokenResult.getValue()));

    // Mark this client as a direct client so that waitForAdmission exempts it rather than sleeping
    // on the mock clock. We are just testing that waitForAdmission resolves the deferred token and
    // allows the command to proceed, not that it waits for the token to be available.
    opCtx->getClient()->setInDirectClient(true);

    runCommandTestWithResponse(BSON("ping" << 1), opCtx.get(), Status::OK());
    ASSERT_EQ(limiterForDeferredToken.queued(), 0);
    ASSERT_EQ(limiterForDeferredToken.stats().exemptedAdmissions.get(), 1);
}

TEST_F(ServiceEntryPointRouterRoleTest,
       PendingIngressDeferredTokenIsConsumedWhenRateLimitingDisabled) {
    RAIIServerParameterControllerForTest requestLimiterEnabled{"ingressRequestRateLimiterEnabled",
                                                               false};

    RateLimiter limiterForDeferredToken(
        /*refreshRatePerSec=*/1.0,
        /*burstCapacitySecs=*/1.0,
        /*maxQueueDepth=*/1,
        "PendingIngressDeferredTokenIsConsumedWhenRateLimitingDisabled");

    auto opCtx = makeOperationContext();
    ASSERT_OK(limiterForDeferredToken.acquireToken(opCtx.get()));
    auto queuedTokenResult = limiterForDeferredToken.acquireToken();
    ASSERT_OK(queuedTokenResult.getStatus());
    ASSERT_FALSE(queuedTokenResult.getValue().isReady());
    IngressRequestRateLimiter::setDeferredAdmissionToken_forTest(
        opCtx->getClient(), std::move(queuedTokenResult.getValue()));

    auto msg = constructMessage(BSON(TestCmdSucceeds::kCommandName << 1), opCtx.get());
    ASSERT_OK(handleRequest(msg, opCtx.get()));

    ASSERT_EQ(limiterForDeferredToken.stats().addedToQueue.get(), 1);
    ASSERT_EQ(limiterForDeferredToken.stats().removedFromQueue.get(), 1);
    ASSERT_EQ(limiterForDeferredToken.stats().exemptedAdmissions.get(), 1);
    ASSERT_EQ(limiterForDeferredToken.queued(), 0);
}

TEST_F(ServiceEntryPointRouterRoleTest, QueuedAdmissionInterrupted) {
    gFeatureFlagIngressRateLimiting.setForServerParameter(true);
    RAIIServerParameterControllerForTest requestLimiterEnabled{"ingressRequestRateLimiterEnabled",
                                                               true};

    RateLimiter limiterForDeferredToken(
        /*refreshRatePerSec=*/1.0,
        /*burstCapacitySecs=*/1.0,
        /*maxQueueDepth=*/2,
        "QueuedAdmissionInterrupted");

    auto opCtx = makeOperationContext();
    ASSERT_OK(limiterForDeferredToken.acquireToken(opCtx.get()));
    auto queuedTokenResult = limiterForDeferredToken.acquireToken();
    ASSERT_OK(queuedTokenResult.getStatus());
    ASSERT_FALSE(queuedTokenResult.getValue().isReady());
    IngressRequestRateLimiter::setDeferredAdmissionToken_forTest(
        opCtx->getClient(), std::move(queuedTokenResult.getValue()));

    // handleRequest blocks in waitForAdmission while the queued token's napTime elapses. A
    // background thread waits until the opCtx is blocking there, then kills it to trigger
    // Interrupted.
    auto msg = constructMessage(BSON(TestCmdRouterIngressSubject::kCommandName << 1), opCtx.get());
    stdx::thread interrupter([&] {
        while (!opCtx->isWaitingForConditionOrInterrupt()) {
            sleepmillis(1);
        }
        opCtx->markKilled(ErrorCodes::Interrupted);
    });
    auto swDbResponse = handleRequest(msg, opCtx.get());
    interrupter.join();
    ASSERT_OK(swDbResponse);
    auto response = dbResponseToBSON(swDbResponse.getValue());
    auto status = getStatusFromCommandResult(response);
    ASSERT_EQ(status.code(), ErrorCodes::Interrupted);
    ASSERT_EQ(limiterForDeferredToken.stats().interruptedInQueue.get(), 1);
}

TEST_F(ServiceEntryPointRouterRoleTest, QueuedAdmissionRespectsMaxTimeMS) {
    gFeatureFlagIngressRateLimiting.setForServerParameter(true);
    RAIIServerParameterControllerForTest requestLimiterEnabled{"ingressRequestRateLimiterEnabled",
                                                               true};

    auto* clockSource =
        static_cast<ClockSourceMock*>(getGlobalServiceContext()->getFastClockSource());
    auto* tickSource =
        static_cast<TickSourceMock<Milliseconds>*>(getGlobalServiceContext()->getTickSource());

    // Create the limiter with the mock tick source so clock advancement controls token
    // availability.
    RateLimiter limiterForDeferredToken(
        /*refreshRatePerSec=*/1.0,
        /*burstCapacitySecs=*/1.0,
        /*maxQueueDepth=*/2,
        "QueuedAdmissionRespectsMaxTimeMS",
        tickSource);

    auto opCtx = makeOperationContext();
    ASSERT_OK(limiterForDeferredToken.acquireToken(opCtx.get()));
    auto queuedTokenResult = limiterForDeferredToken.acquireToken();
    ASSERT_OK(queuedTokenResult.getStatus());
    ASSERT_FALSE(queuedTokenResult.getValue().isReady());
    IngressRequestRateLimiter::setDeferredAdmissionToken_forTest(
        opCtx->getClient(), std::move(queuedTokenResult.getValue()));

    // handleRequest parses maxTimeMS from the message and sets the opCtx deadline before calling
    // waitForAdmission. A background thread waits until the opCtx is blocking in waitForAdmission,
    // then advances the mock clock past the 5ms deadline (well under the ~1000ms napTime) to
    // trigger MaxTimeMSExpired.
    stdx::thread clockAdvancer([&] {
        while (!opCtx->isWaitingForConditionOrInterrupt()) {
            sleepmillis(1);
        }
        clockSource->advance(Milliseconds(6));
        tickSource->advance(Milliseconds(6));
    });

    auto msg = constructMessage(BSON(TestCmdRouterIngressSubject::kCommandName
                                     << 1 << GenericArguments::kMaxTimeMSFieldName << 5),
                                opCtx.get());
    auto swDbResponse = handleRequest(msg, opCtx.get());
    clockAdvancer.join();
    ASSERT_OK(swDbResponse);

    const auto response = dbResponseToBSON(swDbResponse.getValue());
    const auto status = getStatusFromCommandResult(response);
    ASSERT_EQ(status.code(), ErrorCodes::MaxTimeMSExpired);
    ASSERT_EQ(limiterForDeferredToken.stats().interruptedInQueue.get(), 1);
}

TEST_F(ServiceEntryPointRouterRoleTest, QueuedAdmissionWithLargeMaxTimeMSSucceeds) {
    gFeatureFlagIngressRateLimiting.setForServerParameter(true);
    RAIIServerParameterControllerForTest requestLimiterEnabled{"ingressRequestRateLimiterEnabled",
                                                               true};

    auto* clockSource =
        static_cast<ClockSourceMock*>(getGlobalServiceContext()->getFastClockSource());
    auto* tickSource =
        static_cast<TickSourceMock<Milliseconds>*>(getGlobalServiceContext()->getTickSource());

    // Create the limiter with the mock tick source so clock advancement controls token
    // availability.
    RateLimiter limiterForDeferredToken(
        /*refreshRatePerSec=*/1.0,
        /*burstCapacitySecs=*/1.0,
        /*maxQueueDepth=*/2,
        "QueuedAdmissionWithLargeMaxTimeMSSucceeds",
        tickSource);

    auto opCtx = makeOperationContext();
    ASSERT_OK(limiterForDeferredToken.acquireToken(opCtx.get()));
    auto queuedTokenResult = limiterForDeferredToken.acquireToken();
    ASSERT_OK(queuedTokenResult.getStatus());
    ASSERT_FALSE(queuedTokenResult.getValue().isReady());
    IngressRequestRateLimiter::setDeferredAdmissionToken_forTest(
        opCtx->getClient(), std::move(queuedTokenResult.getValue()));

    // handleRequest will block in waitForAdmission while the queued token's napTime (~1000ms at
    // 1 token/sec) elapses. A background thread waits until the opCtx is blocking, then advances
    // the mock clock past the napTime to release the token and let the command succeed.
    stdx::thread clockAdvancer([&] {
        while (!opCtx->isWaitingForConditionOrInterrupt()) {
            sleepmillis(1);
        }
        clockSource->advance(Milliseconds(1001));
        tickSource->advance(Milliseconds(1001));
    });

    auto msg = constructMessage(BSON(TestCmdRouterIngressSubject::kCommandName
                                     << 1 << GenericArguments::kMaxTimeMSFieldName << (60 * 1000)),
                                opCtx.get());
    auto swDbResponse = handleRequest(msg, opCtx.get());
    clockAdvancer.join();

    ASSERT_OK(swDbResponse);
    ASSERT_EQ(getStatusFromCommandResult(dbResponseToBSON(swDbResponse.getValue())), Status::OK());
    ASSERT_EQ(limiterForDeferredToken.stats().successfulAdmissions.get(), 2);
}

TEST_F(ServiceEntryPointRouterRoleTest, HandleRequestException) {
    testHandleRequestException(5745706);
}

TEST_F(ServiceEntryPointRouterRoleTest, ParseCommandFailsDbQueryUnsupportedCommand) {
    testParseCommandFailsDbQueryUnsupportedCommand("Exception thrown while parsing command");
}

TEST_F(ServiceEntryPointRouterRoleTest, TestCommandNotFound) {
    testCommandNotFound(false);
}

TEST_F(ServiceEntryPointRouterRoleTest, HelloCmdSetsClientMetadata) {
    testHelloCmdSetsClientMetadata();
}

TEST_F(ServiceEntryPointRouterRoleTest, TestCommentField) {
    testCommentField();
}

TEST_F(ServiceEntryPointRouterRoleTest, TestHelpField) {
    testHelpField();
}

TEST_F(ServiceEntryPointRouterRoleTest, TestCommandServiceCounters) {
    testCommandServiceCounters(ClusterRole::RouterServer);
}

TEST_F(ServiceEntryPointRouterRoleTest, TestCommandMaxTimeMS) {
    testCommandMaxTimeMS();
}

TEST_F(ServiceEntryPointRouterRoleTest, TestOpCtxInterrupt) {
    testOpCtxInterrupt(false);
}

TEST_F(ServiceEntryPointRouterRoleTest, TestReadConcernClientUnspecifiedNoDefault) {
    testReadConcernClientUnspecifiedNoDefault(1);
}

TEST_F(ServiceEntryPointRouterRoleTest, TestReadConcernClientUnspecifiedWithDefault) {
    testReadConcernClientUnspecifiedWithDefault();
}

TEST_F(ServiceEntryPointRouterRoleTest, TestReadConcernClientSuppliedLevelNotAllowed) {
    testReadConcernClientSuppliedLevelNotAllowed(false);
}

TEST_F(ServiceEntryPointRouterRoleTest, TestReadConcernClientSuppliedAllowed) {
    testReadConcernClientSuppliedAllowed();
}

TEST_F(ServiceEntryPointRouterRoleTest, TestReadConcernExtractedOnException) {
    testReadConcernExtractedOnException();
}

TEST_F(ServiceEntryPointRouterRoleTest, TestCommandInvocationHooks) {
    testCommandInvocationHooks();
}

TEST_F(ServiceEntryPointRouterRoleTest, TestExhaustCommandNextInvocationSet) {
    testExhaustCommandNextInvocationSet();
}

TEST_F(ServiceEntryPointRouterRoleTest, TestWriteConcernClientSpecified) {
    testWriteConcernClientSpecified();
}

TEST_F(ServiceEntryPointRouterRoleTest, TestWriteConcernClientUnspecifiedNoDefault) {
    testWriteConcernClientUnspecifiedNoDefault();
}

TEST_F(ServiceEntryPointRouterRoleTest, TestWriteConcernClientUnspecifiedWithDefault) {
    testWriteConcernClientUnspecifiedWithDefault();
}

#ifdef MONGO_CONFIG_OTEL
TEST_F(ServiceEntryPointRouterRoleTest, TelemetryContextDeserializedFromRequest) {
    testTelemetryContextDeserializedFromRequest();
}

TEST_F(ServiceEntryPointRouterRoleTest, TelemetryContextNotSetWhenNotInRequest) {
    testTelemetryContextNotSetWhenNotInRequest();
}
#endif

}  // namespace
}  // namespace mongo
