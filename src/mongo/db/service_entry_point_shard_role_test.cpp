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
#include "mongo/db/commands.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/service_context.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/rpc/message.h"
#include "mongo/transport/service_entry_point.h"
#include "mongo/transport/service_entry_point_test_fixture.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/duration.h"

namespace mongo {
namespace {

MONGO_REGISTER_COMMAND(TestCmdProcessInternalCommand).testOnly().forShard();
MONGO_REGISTER_COMMAND(TestCmdProcessInternalSucceedCommand).testOnly().forShard();

class ServiceEntryPointShardRoleTest : public ServiceEntryPointTestFixture {
public:
    void setUp() override {
        ServiceEntryPointTestFixture::setUp();
        auto shardService = getGlobalServiceContext()->getService(ClusterRole::ShardServer);
        ReadWriteConcernDefaults::create(shardService, _lookupMock.getFetchDefaultsFn());
        _lookupMock.setLookupCallReturnValue({});

        repl::ReplSettings replSettings;
        replSettings.setReplSetString("rs0/host1");

        auto replCoordMock = std::make_unique<repl::ReplicationCoordinatorMock>(
            getGlobalServiceContext(), replSettings);
        _replCoordMock = replCoordMock.get();
        invariant(replCoordMock->setFollowerMode(repl::MemberState::RS_PRIMARY));
        repl::ReplicationCoordinator::set(getGlobalServiceContext(), std::move(replCoordMock));

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
    testReadConcernClientUnspecifiedWithDefault(false);
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
    testWriteConcernClientUnspecifiedWithDefault(false);
}


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
    testReadConcernClientUnspecifiedWithDefault(true);
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
    testWriteConcernClientUnspecifiedWithDefault(true);
}

}  // namespace
}  // namespace mongo
