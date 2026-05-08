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

#include "mongo/db/service_entry_point_shard_role.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/admission/ingress_admission_context.h"
#include "mongo/db/admission/ingress_request_rate_limiter.h"
#include "mongo/db/admission/rate_limiter.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/rss/attached_storage/attached_persistence_provider.h"
#include "mongo/db/rss/replicated_storage_service.h"
#include "mongo/db/service_context.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/rpc/message.h"
#include "mongo/stdx/thread.h"
#include "mongo/transport/service_entry_point.h"
#include "mongo/transport/service_entry_point_test_fixture.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/duration.h"
#include "mongo/util/tick_source_mock.h"

namespace mongo {
namespace {

using namespace admission;

MONGO_REGISTER_COMMAND(TestCmdProcessInternalCommand).testOnly().forShard();
MONGO_REGISTER_COMMAND(TestCmdProcessInternalSucceedCommand).testOnly().forShard();

class TestCmdShardIngressSubject final : public TestCmdBase {
public:
    static constexpr auto kCommandName = "testShardIngressSubject";
    TestCmdShardIngressSubject() : TestCmdBase(kCommandName) {}

    bool isSubjectToIngressAdmissionControl() const override {
        return true;
    }

    bool runWithBuilderOnly(BSONObjBuilder&) override {
        return true;
    }
};
MONGO_REGISTER_COMMAND(TestCmdShardIngressSubject).testOnly().forShard();

class ServiceEntryPointShardRoleTest : public ServiceEntryPointTestFixture {
public:
    void setUp() override {
        ServiceEntryPointTestFixture::setUp();
        auto shardService = getGlobalServiceContext()->getService();
        ReadWriteConcernDefaults::create(shardService, _lookupMock.getFetchDefaultsFn());
        _lookupMock.setLookupCallReturnValue({});

        repl::ReplSettings replSettings;
        replSettings.setReplSetString("rs0/host1");

        auto replCoordMock = std::make_unique<repl::ReplicationCoordinatorMock>(
            getGlobalServiceContext(), replSettings);
        _replCoordMock = replCoordMock.get();
        invariant(replCoordMock->setFollowerMode(repl::MemberState::RS_PRIMARY));
        repl::ReplicationCoordinator::set(getGlobalServiceContext(), std::move(replCoordMock));

        auto persistenceProvider = std::make_unique<rss::AttachedPersistenceProvider>();
        rss::ReplicatedStorageService::get(getGlobalServiceContext())
            .setPersistenceProvider(std::move(persistenceProvider));

        getGlobalServiceContext()->getService()->setServiceEntryPoint(
            std::make_unique<ServiceEntryPointShardRole>());
    }

    void testProcessInternalCommand();
    void testCommandFailsRunInvocationWithCloseConnectionError();
    void testCommandMaxTimeMSOpOnly();

protected:
    repl::ReplicationCoordinatorMock* _replCoordMock;
};

void ServiceEntryPointShardRoleTest::testProcessInternalCommand() {
    auto opCtx = makeOperationContext();
    auto& admCtx = IngressAdmissionContext::get(opCtx.get());
    // Here we send a command that will itself send another command using DBDirectClient
    auto msg =
        constructMessage(BSON(TestCmdProcessInternalCommand::kCommandName << 1), opCtx.get());
    auto swDbResponse = handleRequest(msg, opCtx.get());
    ASSERT_OK(swDbResponse);
    const auto admissions = admCtx.getAdmissions();
    const auto exemptedAdmissions = admCtx.getExemptedAdmissions();
    // After sending one command, we expect two admissions since we admit the command itself and
    // also the subcommand. However, we expect the second command to be exempted since the top level
    // command was already admitted.
    ASSERT_EQ(admissions, 2);
    ASSERT_EQ(exemptedAdmissions, 1);
}

void ServiceEntryPointShardRoleTest::testCommandFailsRunInvocationWithCloseConnectionError() {
    auto opCtx = makeOperationContext();
    auto msg = constructMessage(
        BSON(TestCmdFailsRunInvocationWithCloseConnectionError::kCommandName << 1), opCtx.get());
    unittest::LogCaptureGuard logs;
    auto swDbResponse = handleRequest(msg, opCtx.get());
    logs.stop();
    ASSERT_NOT_OK(swDbResponse);
    ASSERT_EQ(swDbResponse.getStatus().code(), ErrorCodes::SplitHorizonChange);
    ASSERT_EQ(logs.countTextContaining("Failed to handle request"), 1);
}

void ServiceEntryPointShardRoleTest::testCommandMaxTimeMSOpOnly() {
    auto* clkSource = getGlobalServiceContext()->getFastClockSource();
    auto initialMs = clkSource->now();
    // Only internal clients may set maxTimeMSOpOnly.
    getClient()->setIsInternalClient(true);

    // ServiceEntryPoint uses maxTimeMSOpOnly if maxTimeMS is unset.
    {
        const auto requestMaxTimeMsOpOnly = Milliseconds(200);
        auto opCtx = makeOperationContext();
        const auto cmdBSON = BSON(TestCmdSucceeds::kCommandName << 1 << "maxTimeMSOpOnly"
                                                                << requestMaxTimeMsOpOnly.count());
        runCommandTestWithResponse(cmdBSON, opCtx.get());

        // ServiceEntryPoint should have stored the request's maxTimeMS value, which was previously
        // unset.
        opCtx->restoreMaxTimeMS();
        ASSERT_EQ(Date_t::max(), opCtx->getDeadline());
    }

    // ServiceEntryPoint uses maxTimeMs if maxTimeMs is less than maxTimeMsOpOnly.
    {
        const auto requestMaxTimeMsOpOnly = Milliseconds(200);
        const auto requestMaxTimeMs = Milliseconds(100);
        auto opCtx = makeOperationContext();
        const auto cmdBSON = BSON(TestCmdSucceeds::kCommandName
                                  << 1 << "maxTimeMSOpOnly" << requestMaxTimeMsOpOnly.count()
                                  << "maxTimeMS" << requestMaxTimeMs.count());
        runCommandTestWithResponse(cmdBSON, opCtx.get());

        ASSERT_LTE(opCtx->getDeadline(), initialMs + requestMaxTimeMs + Milliseconds(1));

        // ServiceEntryPoint did not store anything, the deadline remains the same.
        opCtx->restoreMaxTimeMS();
        // Account for mock clock precision of 1 ms, which is added when using restoreMaxTimeMS.
        ASSERT_LTE(opCtx->getDeadline(), initialMs + requestMaxTimeMs + Milliseconds(1));
    }

    // ServiceEntryPoint uses maxTimeMsOpOnly if maxTimeMs is greater than maxTimeMsOpOnly.
    {
        const auto requestMaxTimeMsOpOnly = Milliseconds(200);
        const auto requestMaxTimeMs = Milliseconds(500);
        auto opCtx = makeOperationContext();
        const auto cmdBSON = BSON(TestCmdSucceeds::kCommandName
                                  << 1 << "maxTimeMSOpOnly" << requestMaxTimeMsOpOnly.count()
                                  << "maxTimeMS" << requestMaxTimeMs.count());
        runCommandTestWithResponse(cmdBSON, opCtx.get());

        ASSERT_EQ(initialMs + requestMaxTimeMsOpOnly, opCtx->getDeadline());

        // ServiceEntryPoint should have stored the request's maxTimeMS value.
        opCtx->restoreMaxTimeMS();
        // Account for mock clock precision of 1 ms, which is added when using restoreMaxTimeMS.
        ASSERT_LTE(opCtx->getDeadline(), initialMs + requestMaxTimeMs + Milliseconds(1));
    }
}

class ServiceEntryPointShardServerTest : public virtual service_context_test::ShardRoleOverride,
                                         public ServiceEntryPointShardRoleTest {};

TEST_F(ServiceEntryPointShardServerTest, TestCommandSucceeds) {
    testCommandSucceeds();
}

TEST_F(ServiceEntryPointShardServerTest, TestProcessInternalCommand) {
    testProcessInternalCommand();
}

TEST_F(ServiceEntryPointShardServerTest, TestCommandFailsRunInvocationWithResponse) {
    testCommandFailsRunInvocationWithResponse();
}

TEST_F(ServiceEntryPointShardServerTest, TestCommandFailsRunInvocationWithException) {
    testCommandFailsRunInvocationWithException("Assertion while executing command");
}

TEST_F(ServiceEntryPointShardServerTest, TestCommandFailsRunInvocationWithCloseConnectionError) {
    testCommandFailsRunInvocationWithCloseConnectionError();
}

TEST_F(ServiceEntryPointShardServerTest, HandleRequestException) {
    testHandleRequestException(5745702);
}

TEST_F(ServiceEntryPointShardServerTest, ParseCommandFailsDbQueryUnsupportedCommand) {
    testParseCommandFailsDbQueryUnsupportedCommand("Assertion while parsing command");
}

TEST_F(ServiceEntryPointShardServerTest, TestCommandNotFound) {
    testCommandNotFound(true);
}

TEST_F(ServiceEntryPointShardServerTest, HelloCmdSetsClientMetadata) {
    testHelloCmdSetsClientMetadata();
}

TEST_F(ServiceEntryPointShardServerTest, TestCommentField) {
    testCommentField();
}

TEST_F(ServiceEntryPointShardServerTest, TestHelpField) {
    testHelpField();
}

TEST_F(ServiceEntryPointShardServerTest, TestCommandAdminOnlyLog) {
    unittest::LogCaptureGuard logs;
    runCommandTestWithResponse(BSON(TestCmdSucceedsAdminOnly::kCommandName << 1));
    logs.stop();
    ASSERT_EQ(logs.countTextContaining("Admin only command"), 1);
}

TEST_F(ServiceEntryPointShardServerTest, TestCommandServiceCounters) {
    testCommandServiceCounters(ClusterRole::ShardServer);
}

TEST_F(ServiceEntryPointShardServerTest, TestCommandMaxTimeMS) {
    testCommandMaxTimeMS();
}

TEST_F(ServiceEntryPointShardServerTest, TestCommandMaxTimeMSOpOnly) {
    testCommandMaxTimeMSOpOnly();
}

TEST_F(ServiceEntryPointShardServerTest, TestOpCtxInterrupt) {
    testOpCtxInterrupt(true);
}

TEST_F(ServiceEntryPointShardServerTest, TestReadConcernClientUnspecifiedNoDefault) {
    testReadConcernClientUnspecifiedNoDefault();
}

TEST_F(ServiceEntryPointShardServerTest, TestReadConcernClientUnspecifiedWithDefault) {
    testReadConcernClientUnspecifiedWithDefault();
}

TEST_F(ServiceEntryPointShardServerTest, TestReadConcernClientSuppliedLevelNotAllowed) {
    testReadConcernClientSuppliedLevelNotAllowed(true);
}

TEST_F(ServiceEntryPointShardServerTest, TestReadConcernClientSuppliedAllowed) {
    testReadConcernClientSuppliedAllowed();
}

TEST_F(ServiceEntryPointShardServerTest, TestReadConcernExtractedOnException) {
    testReadConcernExtractedOnException();
}

TEST_F(ServiceEntryPointShardServerTest, TestCommandInvocationHooks) {
    testCommandInvocationHooks();
}

TEST_F(ServiceEntryPointShardServerTest, TestExhaustCommandNextInvocationSet) {
    testExhaustCommandNextInvocationSet();
}

TEST_F(ServiceEntryPointShardServerTest, TestWriteConcernClientSpecified) {
    _replCoordMock->setWriteConcernMajorityShouldJournal(false);
    testWriteConcernClientSpecified();
}

TEST_F(ServiceEntryPointShardServerTest, TestWriteConcernClientUnspecifiedNoDefault) {
    _replCoordMock->setWriteConcernMajorityShouldJournal(false);
    testWriteConcernClientUnspecifiedNoDefault();
}

TEST_F(ServiceEntryPointShardServerTest, TestWriteConcernClientUnspecifiedWithDefault) {
    _replCoordMock->setWriteConcernMajorityShouldJournal(false);
    testWriteConcernClientUnspecifiedWithDefault();
}

TEST_F(ServiceEntryPointShardServerTest, WaitForAdmissionResolvesDeferredTokenBeforeInvocation) {
    RAIIServerParameterControllerForTest rateLimiterEnabled{"ingressRequestRateLimiterEnabled",
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

TEST_F(ServiceEntryPointShardServerTest,
       PendingIngressDeferredTokenIsConsumedWhenRateLimitingDisabled) {
    RAIIServerParameterControllerForTest rateLimiterEnabled{"ingressRequestRateLimiterEnabled",
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

TEST_F(ServiceEntryPointShardServerTest, QueuedAdmissionInterrupted) {
    RAIIServerParameterControllerForTest rateLimiterEnabled{"ingressRequestRateLimiterEnabled",
                                                            true};

    auto opCtx = makeOperationContext();
    auto* client = opCtx->getClient();

    RateLimiter limiterForDeferredToken(
        /*refreshRatePerSec=*/1.0,
        /*burstCapacitySecs=*/1.0,
        /*maxQueueDepth=*/2,
        "QueuedAdmissionInterrupted");

    ASSERT_OK(limiterForDeferredToken.acquireToken(opCtx.get()));
    auto queuedTokenResult = limiterForDeferredToken.acquireToken();
    ASSERT_OK(queuedTokenResult.getStatus());
    ASSERT_FALSE(queuedTokenResult.getValue().isReady());
    IngressRequestRateLimiter::setDeferredAdmissionToken_forTest(
        client, std::move(queuedTokenResult.getValue()));

    // handleRequest blocks in waitForAdmission while the queued token's napTime elapses. A
    // background thread waits until the opCtx is blocking there, then kills it to trigger
    // Interrupted.
    auto msg = constructMessage(BSON(TestCmdShardIngressSubject::kCommandName << 1), opCtx.get());
    stdx::thread interrupter([&] {
        while (!opCtx->isWaitingForConditionOrInterrupt()) {
            sleepmillis(1);
        }
        opCtx->markKilled(ErrorCodes::Interrupted);
    });
    auto swDbResponse = handleRequest(msg, opCtx.get());
    interrupter.join();
    ASSERT_OK(swDbResponse);

    const auto response = dbResponseToBSON(swDbResponse.getValue());
    const auto status = getStatusFromCommandResult(response);
    ASSERT_EQ(status.code(), ErrorCodes::Interrupted);
    ASSERT_EQ(limiterForDeferredToken.stats().interruptedInQueue.get(), 1);
}

TEST_F(ServiceEntryPointShardServerTest, QueuedAdmissionRespectsMaxTimeMS) {
    gFeatureFlagIngressRateLimiting.setForServerParameter(true);
    RAIIServerParameterControllerForTest requestLimiterEnabled{"ingressRequestRateLimiterEnabled",
                                                               true};

    auto* clockSource =
        static_cast<ClockSourceMock*>(getGlobalServiceContext()->getFastClockSource());
    auto* tickSource =
        static_cast<TickSourceMock<Milliseconds>*>(getGlobalServiceContext()->getTickSource());

    auto opCtx = makeOperationContext();
    auto* client = opCtx->getClient();

    // ServiceEntryPointShardRole has a contract guard ensuring presence of an AuthorizationSession.
    AuthorizationSession::get(client);

    // Create the limiter with the mock tick source so clock advancement controls token
    // availability.
    RateLimiter limiterForDeferredToken(
        /*refreshRatePerSec=*/1.0,
        /*burstCapacitySecs=*/1.0,
        /*maxQueueDepth=*/2,
        "QueuedAdmissionRespectsMaxTimeMS",
        tickSource);

    ASSERT_OK(limiterForDeferredToken.acquireToken(opCtx.get()));
    auto queuedTokenResult = limiterForDeferredToken.acquireToken();
    ASSERT_OK(queuedTokenResult.getStatus());
    ASSERT_FALSE(queuedTokenResult.getValue().isReady());
    IngressRequestRateLimiter::setDeferredAdmissionToken_forTest(
        client, std::move(queuedTokenResult.getValue()));

    // handleRequest parses maxTimeMS from the message and sets the opCtx deadline. A background
    // thread waits until the opCtx is blocking in waitForAdmission, then advances the mock clock
    // past the 5ms deadline (well under the ~1000ms napTime) to trigger MaxTimeMSExpired.
    stdx::thread clockAdvancer([&] {
        while (!opCtx->isWaitingForConditionOrInterrupt()) {
            sleepmillis(1);
        }
        clockSource->advance(Milliseconds(6));
        tickSource->advance(Milliseconds(6));
    });

    auto msg = constructMessage(BSON(TestCmdShardIngressSubject::kCommandName
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

TEST_F(ServiceEntryPointShardServerTest, QueuedAdmissionWithLargeMaxTimeMSSucceeds) {
    gFeatureFlagIngressRateLimiting.setForServerParameter(true);
    RAIIServerParameterControllerForTest requestLimiterEnabled{"ingressRequestRateLimiterEnabled",
                                                               true};

    auto* clockSource =
        static_cast<ClockSourceMock*>(getGlobalServiceContext()->getFastClockSource());
    auto* tickSource =
        static_cast<TickSourceMock<Milliseconds>*>(getGlobalServiceContext()->getTickSource());

    auto opCtx = makeOperationContext();
    auto* client = opCtx->getClient();

    // ServiceEntryPointShardRole has a contract guard ensuring presence of an AuthorizationSession.
    AuthorizationSession::get(client);

    RateLimiter limiterForDeferredToken(
        /*refreshRatePerSec=*/1.0,
        /*burstCapacitySecs=*/1.0,
        /*maxQueueDepth=*/2,
        "QueuedAdmissionWithLargeMaxTimeMSSucceeds",
        tickSource);

    ASSERT_OK(limiterForDeferredToken.acquireToken(opCtx.get()));
    auto queuedTokenResult = limiterForDeferredToken.acquireToken();
    ASSERT_OK(queuedTokenResult.getStatus());
    ASSERT_FALSE(queuedTokenResult.getValue().isReady());
    IngressRequestRateLimiter::setDeferredAdmissionToken_forTest(
        client, std::move(queuedTokenResult.getValue()));

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

    auto msg = constructMessage(BSON(TestCmdShardIngressSubject::kCommandName
                                     << 1 << GenericArguments::kMaxTimeMSFieldName << (60 * 1000)),
                                opCtx.get());
    auto swDbResponse = handleRequest(msg, opCtx.get());
    clockAdvancer.join();

    ASSERT_OK(swDbResponse);
    ASSERT_EQ(getStatusFromCommandResult(dbResponseToBSON(swDbResponse.getValue())), Status::OK());
    ASSERT_EQ(limiterForDeferredToken.stats().successfulAdmissions.get(), 2);
}

#ifdef MONGO_CONFIG_OTEL
TEST_F(ServiceEntryPointShardServerTest, TelemetryContextDeserializedFromRequest) {
    testTelemetryContextDeserializedFromRequest();
}

TEST_F(ServiceEntryPointShardServerTest, TelemetryContextNotSetWhenNotInRequest) {
    testTelemetryContextNotSetWhenNotInRequest();
}
#endif


class ServiceEntryPointReplicaSetTest : public virtual service_context_test::ReplicaSetRoleOverride,
                                        public ServiceEntryPointShardRoleTest {};

TEST_F(ServiceEntryPointReplicaSetTest, TestCommandSucceeds) {
    testCommandSucceeds();
}

TEST_F(ServiceEntryPointReplicaSetTest, TestProcessInternalCommand) {
    testProcessInternalCommand();
}

TEST_F(ServiceEntryPointReplicaSetTest, TestCommandFailsRunInvocationWithResponse) {
    testCommandFailsRunInvocationWithResponse();
}

TEST_F(ServiceEntryPointReplicaSetTest, TestCommandFailsRunInvocationWithException) {
    testCommandFailsRunInvocationWithException("Assertion while executing command");
}

TEST_F(ServiceEntryPointReplicaSetTest, TestCommandFailsRunInvocationWithCloseConnectionError) {
    testCommandFailsRunInvocationWithCloseConnectionError();
}

TEST_F(ServiceEntryPointReplicaSetTest, HandleRequestException) {
    testHandleRequestException(5745702);
}

TEST_F(ServiceEntryPointReplicaSetTest, ParseCommandFailsDbQueryUnsupportedCommand) {
    testParseCommandFailsDbQueryUnsupportedCommand("Assertion while parsing command");
}

TEST_F(ServiceEntryPointReplicaSetTest, TestCommandNotFound) {
    testCommandNotFound(true);
}

TEST_F(ServiceEntryPointReplicaSetTest, HelloCmdSetsClientMetadata) {
    testHelloCmdSetsClientMetadata();
}

TEST_F(ServiceEntryPointReplicaSetTest, TestCommentField) {
    testCommentField();
}

TEST_F(ServiceEntryPointReplicaSetTest, TestHelpField) {
    testHelpField();
}

TEST_F(ServiceEntryPointReplicaSetTest, TestCommandAdminOnlyLog) {
    unittest::LogCaptureGuard logs;
    runCommandTestWithResponse(BSON(TestCmdSucceedsAdminOnly::kCommandName << 1));
    logs.stop();
    ASSERT_EQ(logs.countTextContaining("Admin only command"), 1);
}

TEST_F(ServiceEntryPointReplicaSetTest, TestCommandServiceCounters) {
    testCommandServiceCounters(ClusterRole::ShardServer);
}

TEST_F(ServiceEntryPointReplicaSetTest, TestCommandMaxTimeMS) {
    testCommandMaxTimeMS();
}

TEST_F(ServiceEntryPointReplicaSetTest, TestCommandMaxTimeMSOpOnly) {
    testCommandMaxTimeMSOpOnly();
}

TEST_F(ServiceEntryPointReplicaSetTest, TestOpCtxInterrupt) {
    testOpCtxInterrupt(true);
}

TEST_F(ServiceEntryPointReplicaSetTest, TestReadConcernClientUnspecifiedNoDefault) {
    testReadConcernClientUnspecifiedNoDefault();
}

TEST_F(ServiceEntryPointReplicaSetTest, TestReadConcernClientUnspecifiedWithDefault) {
    testReadConcernClientUnspecifiedWithDefault();
}

TEST_F(ServiceEntryPointReplicaSetTest, TestReadConcernClientSuppliedLevelNotAllowed) {
    testReadConcernClientSuppliedLevelNotAllowed(true);
}

TEST_F(ServiceEntryPointReplicaSetTest, TestReadConcernClientSuppliedAllowed) {
    testReadConcernClientSuppliedAllowed();
}

TEST_F(ServiceEntryPointReplicaSetTest, TestReadConcernExtractedOnException) {
    testReadConcernExtractedOnException();
}

TEST_F(ServiceEntryPointReplicaSetTest, TestCommandInvocationHooks) {
    testCommandInvocationHooks();
}

TEST_F(ServiceEntryPointReplicaSetTest, TestExhaustCommandNextInvocationSet) {
    testExhaustCommandNextInvocationSet();
}

TEST_F(ServiceEntryPointReplicaSetTest, TestWriteConcernClientSpecified) {
    _replCoordMock->setWriteConcernMajorityShouldJournal(false);
    testWriteConcernClientSpecified();
}

TEST_F(ServiceEntryPointReplicaSetTest, TestWriteConcernClientUnspecifiedNoDefault) {
    _replCoordMock->setWriteConcernMajorityShouldJournal(false);
    testWriteConcernClientUnspecifiedNoDefault();
}

TEST_F(ServiceEntryPointReplicaSetTest, TestWriteConcernClientUnspecifiedWithDefault) {
    _replCoordMock->setWriteConcernMajorityShouldJournal(false);
    testWriteConcernClientUnspecifiedWithDefault();
}

#ifdef MONGO_CONFIG_OTEL
TEST_F(ServiceEntryPointReplicaSetTest, TelemetryContextDeserializedFromRequest) {
    testTelemetryContextDeserializedFromRequest();
}

TEST_F(ServiceEntryPointReplicaSetTest, TelemetryContextNotSetWhenNotInRequest) {
    testTelemetryContextNotSetWhenNotInRequest();
}
#endif

}  // namespace
}  // namespace mongo
