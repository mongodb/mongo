// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/s/service_entry_point_router_role.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/db/admission/ingress_request_admission_context.h"
#include "mongo/db/admission/ingress_request_rate_limiter.h"
#include "mongo/db/admission/rate_limiter.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/curop.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/stdx/thread.h"
#include "mongo/transport/service_entry_point.h"
#include "mongo/transport/service_entry_point_test_fixture.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/thread_assertion_monitor.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/tick_source_mock.h"

namespace mongo {
namespace {

using namespace admission;

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

TEST_F(ServiceEntryPointRouterRoleTest,
       PendingIngressDeferredTokenIsConsumedWhenRateLimitingDisabled) {
    testPendingIngressDeferredTokenIsConsumedWhenRateLimitingDisabled();
}

TEST_F(ServiceEntryPointRouterRoleTest, QueuedAdmissionInterrupted) {
    testQueuedAdmissionInterrupted();
}

TEST_F(ServiceEntryPointRouterRoleTest, QueuedAdmissionRespectsMaxTimeMS) {
    testQueuedAdmissionRespectsMaxTimeMS();
}

TEST_F(ServiceEntryPointRouterRoleTest, QueuedAdmissionWithLargeMaxTimeMSSucceeds) {
    testQueuedAdmissionWithLargeMaxTimeMSSucceeds();
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

TEST_F(ServiceEntryPointRouterRoleTest, TelemetryContextDeserializedFromSection) {
    testTelemetryContextDeserializedFromSection();
}

TEST_F(ServiceEntryPointRouterRoleTest, SpanNotCreatedWhenTelemetryContextNotSetInRequest) {
    testSpanNotCreatedWhenTelemetryContextNotSetInRequest();
}

TEST_F(ServiceEntryPointRouterRoleTest, IngressSpanHasServerKind) {
    testIngressSpanHasServerKind();
}

TEST_F(ServiceEntryPointRouterRoleTest, IngressSpanHasConsumerKindForMoreToCome) {
    testIngressSpanHasConsumerKindForMoreToCome();
}

}  // namespace
}  // namespace mongo
